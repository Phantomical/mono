/**
 * \file
 * \brief Slab allocation for JIT'd code.
 */

#include "codemem.hpp"

#include <llvm/ADT/STLExtras.h>
#include <llvm/ExecutionEngine/JITLink/JITLink.h>
#include <llvm/Support/Memory.h>
#include <llvm/Support/Process.h>

#include <algorithm>
#include <cassert>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <string_view>
#include <utility>

#include <sys/mman.h>

using namespace llvm;

namespace mono {
namespace {

constexpr size_t default_slab_size = size_t (2) * 1024 * 1024 * 1024;
constexpr size_t max_slab_size = size_t (2) * 1024 * 1024 * 1024;
constexpr size_t min_slab_size = size_t (16) * 1024 * 1024;

/*
 * The top page of a slab is never handed out, to either region. A fixup at the
 * very top of a 2GB slab reaching the very bottom is a four-byte field at S-4,
 * so with a PC32's usual -4 addend the distance is exactly -2^31 - representable,
 * with nothing to spare - and an instruction carrying an immediate after the
 * displacement (movq $0, sym(%rip)) takes the addend to -8 and pushes it over.
 * Holding a page back leaves every pair of bytes a slab hands out inside what a
 * 32-bit relocation can encode, in both directions. A page rather than the four
 * bytes strictly needed, because that is the granularity writable_floor tracks
 * and the difference is address space only.
 */
constexpr size_t guard_pages = 1;

size_t
align_up (size_t value, size_t align)
{
	return (value + align - 1) & ~(align - 1);
}

/*
 * The cap is not a tuning choice. An object's code sits at the bottom of a slab
 * and its mutable data at the top, and the reference between them is a PCRel32
 * with no stub to fall back on, so the whole slab has to fit inside that reach
 * however large the environment asks for it to be. 2GB is the far edge of that
 * reach, which is what the guard page above buys the margin for.
 */
size_t
configured_slab_size (size_t page_size)
{
	size_t size = default_slab_size;
	const char *env = getenv ("MONO_LLVM_SLAB_SIZE");

	if (env && *env) {
		char *end = nullptr;
		unsigned long long value = strtoull (env, &end, 0);
		std::string_view suffix (end);
		size_t scale = 0;

		if (suffix.empty ())
			scale = 1;
		else if (suffix == "k" || suffix == "K")
			scale = 1024;
		else if (suffix == "m" || suffix == "M")
			scale = 1024 * 1024;
		else if (suffix == "g" || suffix == "G")
			scale = 1024 * 1024 * 1024;

		if (value != 0 && scale != 0)
			size = size_t (value) * scale;
	}

	return align_up (std::min (std::max (size, min_slab_size), max_slab_size),
	                 page_size);
}

int
prot_flags (bool writable)
{
	return writable ? PROT_READ | PROT_WRITE | PROT_EXEC : PROT_READ | PROT_EXEC;
}

} // namespace

CodeSlabs::CodeSlabs (size_t page_size)
	: page_size_ (page_size), slab_size_ (configured_slab_size (page_size))
{
}

CodeSlabs::~CodeSlabs ()
{
	for (auto &slab : slabs_)
		munmap (slab->base, slab->size);
}

Expected<std::shared_ptr<CodeSlabs>>
CodeSlabs::create ()
{
	auto page_size = sys::Process::getPageSize ();

	if (!page_size)
		return page_size.takeError ();
	return std::shared_ptr<CodeSlabs> (new CodeSlabs (*page_size));
}

Error
CodeSlabs::add_slab ()
{
	size_t want = slab_size_;
	void *base = MAP_FAILED;

	/*
	 * RLIMIT_AS counts a PROT_NONE reservation, and while nothing in this
	 * deployment sets one, a process that does would otherwise fail on the
	 * first compile rather than on the byte it cannot afford.
	 */
	for (;;) {
		base = mmap (nullptr, want, PROT_NONE,
		             MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
		if (base != MAP_FAILED)
			break;
		if (want <= min_slab_size)
			return createStringError (
				inconvertibleErrorCode (),
				"cannot reserve %zu bytes for JIT code", want);
		want /= 2;
	}

	auto slab = std::make_unique<Slab> ();

	slab->base = static_cast<char *> (base);
	slab->size = want;
	slab->writable_bump = want - guard_pages * page_size_;
	slab->writable_floor = slab->writable_bump / page_size_;
	slabs_.push_back (std::move (slab));
	return Error::success ();
}

/// Takes SIZE bytes out of the free set, or off the bump pointer. No syscall:
/// committing the pages is open ()'s job.
bool
CodeSlabs::carve (Slab &s, size_t index, size_t size, size_t align, Alloc &out)
{
	for (auto it = s.free.begin (); it != s.free.end (); ++it) {
		size_t start = align_up (it->first, align);
		size_t pad = start - it->first;

		if (pad + size > it->second)
			continue;

		size_t offset = it->first, length = it->second;

		s.free.erase (it);
		if (pad != 0)
			s.free[offset] = pad;
		if (length - pad - size != 0)
			s.free[start + size] = length - pad - size;

		out = Alloc { s.base + start, size, index };
		s.live += size;
		return true;
	}

	size_t start = align_up (s.bump, align);

	if (start + size > s.writable_floor * page_size_)
		return false;

	s.bump = start + size;
	out = Alloc { s.base + start, size, index };
	s.live += size;
	return true;
}

/// Undoes a carve () that a later step of the same allocation could not
/// complete. Exact, because the lock has not been dropped in between.
void
CodeSlabs::uncarve (Slab &s, const Alloc &a)
{
	size_t offset = size_t (a.base - s.base);

	s.live -= a.size;
	if (offset + a.size == s.bump)
		s.bump = offset;
	else
		put_free (s.free, offset, a.size);
}

Expected<bool>
CodeSlabs::carve_writable (Slab &s, size_t index, size_t size, size_t align,
                           Alloc &out)
{
	for (auto it = s.writable_free.begin (); it != s.writable_free.end (); ++it) {
		size_t start = align_up (it->first, align);
		size_t pad = start - it->first;

		if (pad + size > it->second)
			continue;

		size_t offset = it->first, length = it->second;

		s.writable_free.erase (it);
		if (pad != 0)
			s.writable_free[offset] = pad;
		if (length - pad - size != 0)
			s.writable_free[start + size] = length - pad - size;

		out = Alloc { s.base + start, size, index };
		s.live += size;
		return true;
	}

	if (s.writable_bump < size)
		return false;

	size_t start = (s.writable_bump - size) & ~(align - 1);
	size_t floor = start / page_size_;

	if (floor * page_size_ < align_up (s.bump, page_size_))
		return false;

	if (floor < s.writable_floor) {
		if (mprotect (s.base + floor * page_size_,
		              (s.writable_floor - floor) * page_size_,
		              prot_flags (true)) != 0)
			return createStringError (
				std::error_code (errno, std::generic_category ()),
				"cannot commit writable JIT pages");
		s.committed_pages += s.writable_floor - floor;
		s.writable_floor = floor;
	}

	s.writable_bump = start;
	out = Alloc { s.base + start, size, index };
	s.live += size;
	return true;
}

Expected<CodeSlabs::Object>
CodeSlabs::allocate_object (size_t code_size, size_t code_align,
                            size_t data_size, size_t data_align)
{
	if (code_align == 0)
		code_align = 1;
	if (data_align == 0)
		data_align = 1;

	std::lock_guard<std::mutex> lock (mutex_);

	for (size_t attempt = 0; attempt < 2; attempt++) {
		for (size_t i = 0; i < slabs_.size (); i++) {
			Slab &s = *slabs_[i];
			Object obj;

			if (code_size != 0 && !carve (s, i, code_size, code_align, obj.code))
				continue;

			if (data_size != 0) {
				Expected<bool> got = carve_writable (s, i, data_size,
				                                     data_align, obj.data);

				if (!got) {
					if (code_size != 0)
						uncarve (s, obj.code);
					return got.takeError ();
				}
				if (!*got) {
					if (code_size != 0)
						uncarve (s, obj.code);
					continue;
				}
			}

			if (code_size != 0)
				if (Error err = open (obj.code))
					return std::move (err);
			return obj;
		}

		if (attempt == 0)
			if (Error err = add_slab ())
				return std::move (err);
	}

	return createStringError (inconvertibleErrorCode (),
	                          "an object of %zu code and %zu data bytes does "
	                          "not fit in a code slab",
	                          code_size, data_size);
}

Expected<CodeSlabs::Alloc>
CodeSlabs::allocate (size_t size, size_t align)
{
	Expected<Object> obj = allocate_object (size == 0 ? 1 : size, align, 0, 1);

	if (!obj)
		return obj.takeError ();
	return obj->code;
}

Expected<CodeSlabs::Alloc>
CodeSlabs::allocate_writable (size_t size, size_t align)
{
	Expected<Object> obj = allocate_object (0, 1, size == 0 ? 1 : size, align);

	if (!obj)
		return obj.takeError ();
	return obj->data;
}

Error
CodeSlabs::open (const Alloc &a)
{
	Slab &s = *slabs_[a.slab];
	size_t first = size_t (a.base - s.base) / page_size_;
	size_t last = size_t (a.base + a.size - 1 - s.base) / page_size_;

	if (s.writers.size () < last + 1) {
		s.writers.resize (last + 1, 0);
		s.prot.resize (last + 1, PageProt::None);
	}

	for (size_t p = first; p <= last; p++)
		s.writers[p]++;

	return reprotect (s, first, last, /*seal=*/false);
}

Error
CodeSlabs::close (const Alloc &a)
{
	Slab &s = *slabs_[a.slab];
	size_t first = size_t (a.base - s.base) / page_size_;
	size_t last = size_t (a.base + a.size - 1 - s.base) / page_size_;

	for (size_t p = first; p <= last; p++)
		s.writers[p]--;

	return reprotect (s, first, last, /*seal=*/true);
}

/*
 * Walks [FIRST, LAST] and mprotects each maximal run of pages that has to
 * change, so a multi-page allocation costs one syscall rather than one per
 * page. A page only seals when the last allocation writing to it says so,
 * which is what makes a page shared between two in-flight allocations come out
 * right whichever of them finishes first.
 */
Error
CodeSlabs::reprotect (Slab &s, size_t first, size_t last, bool seal)
{
	PageProt want = seal ? PageProt::Rx : PageProt::Rwx;
	size_t run = SIZE_MAX;

	for (size_t p = first; p <= last + 1; p++) {
		bool change = p <= last
		              && (seal ? s.writers[p] == 0 && s.prot[p] == PageProt::Rwx
		                       : s.prot[p] != PageProt::Rwx);

		if (change) {
			if (run == SIZE_MAX)
				run = p;
			continue;
		}
		if (run == SIZE_MAX)
			continue;

		if (mprotect (s.base + run * page_size_, (p - run) * page_size_,
		              prot_flags (!seal)) != 0)
			return createStringError (std::error_code (errno,
			                                           std::generic_category ()),
			                          "cannot protect JIT code pages");

		for (size_t q = run; q < p; q++) {
			if (s.prot[q] == PageProt::None)
				s.committed_pages++;
			s.prot[q] = want;
		}
		run = SIZE_MAX;
	}

	return Error::success ();
}

Error
CodeSlabs::finish (const Alloc &a)
{
	if (a.base == nullptr)
		return Error::success ();

	std::lock_guard<std::mutex> lock (mutex_);
	return close (a);
}

Error
CodeSlabs::abandon (const Alloc &a)
{
	if (a.base == nullptr)
		return Error::success ();

	std::lock_guard<std::mutex> lock (mutex_);
	Slab &s = *slabs_[a.slab];
	Error err = close (a);

	uncarve (s, a);
	return err;
}

Error
CodeSlabs::abandon (const Object &o)
{
	Error err = abandon (o.code);

	release_writable (o.data);
	return err;
}

void
CodeSlabs::release (const Alloc &a)
{
	if (a.base == nullptr)
		return;

	std::lock_guard<std::mutex> lock (mutex_);
	Slab &s = *slabs_[a.slab];
	size_t offset = size_t (a.base - s.base), length = a.size;

	s.live -= a.size;
	put_free (s.free, offset, length, &offset, &length);
	drop_pages (s, offset, length);
}

/*
 * Hands back the whole pages inside a free range, so that a domain minting and
 * dropping dynamic methods settles at its live size rather than its high-water
 * mark. Only pages the range covers entirely: a page it shares with a live
 * allocation still has something on it.
 *
 * The mapping keeps its protection - MADV_DONTNEED drops the physical page, not
 * the VMA - but the page is recorded as uncommitted so that the next allocation
 * to reach it charges for it again.
 */
void
CodeSlabs::drop_pages (Slab &s, size_t offset, size_t length)
{
	size_t first = align_up (offset, page_size_) / page_size_;
	size_t last = (offset + length) / page_size_;

	if (last <= first)
		return;

	madvise (s.base + first * page_size_, (last - first) * page_size_,
	         MADV_DONTNEED);

	for (size_t p = first; p < last && p < s.prot.size (); p++)
		if (s.prot[p] != PageProt::None) {
			s.prot[p] = PageProt::None;
			s.committed_pages--;
		}
}

void
CodeSlabs::release_writable (const Alloc &a)
{
	if (a.base == nullptr)
		return;

	std::lock_guard<std::mutex> lock (mutex_);
	Slab &s = *slabs_[a.slab];

	s.live -= a.size;
	put_free (s.writable_free, size_t (a.base - s.base), a.size);
}

void
CodeSlabs::release (const Object &o)
{
	release (o.code);
	release_writable (o.data);
}

void
CodeSlabs::put_free (std::map<size_t, size_t> &set, size_t offset, size_t length,
                     size_t *merged_offset, size_t *merged_length)
{
	auto next = set.lower_bound (offset);

	if (next != set.end () && offset + length == next->first) {
		length += next->second;
		next = set.erase (next);
	}
	if (next != set.begin ()) {
		auto prev = std::prev (next);

		if (prev->first + prev->second == offset) {
			offset = prev->first;
			length += prev->second;
			set.erase (prev);
		}
	}

	set[offset] = length;
	if (merged_offset)
		*merged_offset = offset;
	if (merged_length)
		*merged_length = length;
}

size_t
CodeSlabs::slab_count ()
{
	std::lock_guard<std::mutex> lock (mutex_);
	return slabs_.size ();
}

size_t
CodeSlabs::live_bytes ()
{
	std::lock_guard<std::mutex> lock (mutex_);
	size_t total = 0;

	for (auto &slab : slabs_)
		total += slab->live;
	return total;
}

size_t
CodeSlabs::committed_bytes ()
{
	std::lock_guard<std::mutex> lock (mutex_);
	size_t total = 0;

	for (auto &slab : slabs_)
		total += slab->committed_pages * page_size_;
	return total;
}

class SlabMemoryManager::InFlight final
	: public jitlink::JITLinkMemoryManager::InFlightAlloc {
public:
	InFlight (SlabMemoryManager *owner, jitlink::LinkGraph &g,
	          CodeSlabs::Object object)
		: owner_ (owner), graph_ (&g), object_ (object)
	{
	}

	~InFlight () override
	{
		assert (settled_ && "in-flight allocation neither finalized nor "
		                    "abandoned");
	}

	void finalize (OnFinalizedFunction on_finalized) override
	{
		settled_ = true;

		/*
		 * Before the pages seal, because __register_frame reads the
		 * .eh_frame bytes and an alloc action is entitled to write into
		 * the allocation it belongs to.
		 */
		Expected<std::vector<orc::shared::WrapperFunctionCall>> dealloc =
			orc::shared::runFinalizeActions (graph_->allocActions ());

		if (!dealloc)
			return on_finalized (dealloc.takeError ());

		if (Error err = owner_->slabs_->finish (object_))
			return on_finalized (std::move (err));

		if (object_.code.base != nullptr)
			sys::Memory::InvalidateInstructionCache (object_.code.base,
			                                         object_.code.size);
		on_finalized (owner_->record (object_, std::move (*dealloc)));
	}

	void abandon (OnAbandonedFunction on_abandoned) override
	{
		settled_ = true;
		on_abandoned (owner_->slabs_->abandon (object_));
	}

private:
	SlabMemoryManager *owner_;
	jitlink::LinkGraph *graph_;
	CodeSlabs::Object object_;
	bool settled_ = false;
};

void
SlabMemoryManager::allocate (const jitlink::JITLinkDylib *, jitlink::LinkGraph &g,
                             OnAllocatedFunction on_allocated)
{
	using Placement = std::vector<
		std::pair<jitlink::BasicLayout::Segment *, uint64_t>>;

	jitlink::BasicLayout layout (g);
	Placement code, data;
	uint64_t code_size = 0, data_size = 0;
	Align code_align (1), data_align (1);

	for (auto &kv : layout.segments ()) {
		if (kv.first.getMemLifetime () != orc::MemLifetime::Standard)
			return on_allocated (make_error<StringError> (
				"graph " + g.getName ()
					+ " wants a finalize-lifetime segment, which "
					  "this target does not produce",
				inconvertibleErrorCode ()));

		jitlink::BasicLayout::Segment &seg = kv.second;
		bool writable = (kv.first.getMemProt () & orc::MemProt::Write)
		                == orc::MemProt::Write;
		uint64_t &extent = writable ? data_size : code_size;
		Align &align = writable ? data_align : code_align;

		extent = alignTo (extent, seg.Alignment);
		(writable ? data : code).emplace_back (&seg, extent);
		extent += seg.ContentSize + seg.ZeroFillSize;
		align = std::max (align, seg.Alignment);
	}

	Expected<CodeSlabs::Object> object = slabs_->allocate_object (
		code_size, code_align.value (), data_size, data_align.value ());
	if (!object)
		return on_allocated (object.takeError ());

	for (bool writable : { false, true }) {
		Placement &placed = writable ? data : code;
		char *base = writable ? object->data.base : object->code.base;

		for (auto &[seg, offset] : placed) {
			seg->Addr = orc::ExecutorAddr::fromPtr (base + offset);
			seg->WorkingMem = seg->Addr.toPtr<char *> ();

			/* The stock in-process manager zeroes its whole slab up
			 * front; a shared slab has to be zeroed a segment at a
			 * time. */
			memset (seg->WorkingMem + seg->ContentSize, 0,
			        seg->ZeroFillSize);
		}
	}

	if (Error err = layout.apply ()) {
		if (Error unwind = slabs_->abandon (*object))
			err = joinErrors (std::move (err), std::move (unwind));
		return on_allocated (std::move (err));
	}

	on_allocated (std::make_unique<InFlight> (this, g, *object));
}

jitlink::JITLinkMemoryManager::FinalizedAlloc
SlabMemoryManager::record (CodeSlabs::Object object,
                           std::vector<orc::shared::WrapperFunctionCall> actions)
{
	std::lock_guard<std::mutex> lock (mutex_);
	FinalizedInfo *info = infos_.Allocate<FinalizedInfo> ();

	new (info) FinalizedInfo { object, std::move (actions) };
	return FinalizedAlloc (orc::ExecutorAddr::fromPtr (info));
}

void
SlabMemoryManager::deallocate (std::vector<FinalizedAlloc> allocs,
                               OnDeallocatedFunction on_deallocated)
{
	std::vector<FinalizedInfo *> infos;

	infos.reserve (allocs.size ());
	for (FinalizedAlloc &alloc : allocs)
		infos.push_back (alloc.release ().toPtr<FinalizedInfo *> ());

	Error all = Error::success ();

	/* Reverse order, which is the contract the base class states. */
	for (FinalizedInfo *info : llvm::reverse (infos)) {
		while (!info->dealloc_actions.empty ()) {
			if (Error err = info->dealloc_actions.back ()
			                    .runWithSPSRetErrorMerged ())
				all = joinErrors (std::move (all), std::move (err));
			info->dealloc_actions.pop_back ();
		}

		slabs_->release (info->object);

		std::lock_guard<std::mutex> lock (mutex_);
		info->~FinalizedInfo ();
		infos_.Deallocate (info);
	}

	on_deallocated (std::move (all));
}

} // namespace mono

/**
 * \file
 * \brief Slab allocation for JIT'd code.
 */

#include "codemem.hpp"

#include <llvm/Support/Process.h>

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <string_view>

#include <sys/mman.h>

using namespace llvm;

namespace mono {
namespace {

constexpr size_t default_slab_size = size_t (2) * 1024 * 1024 * 1024;
constexpr size_t max_slab_size = size_t (2) * 1024 * 1024 * 1024;
constexpr size_t min_slab_size = size_t (16) * 1024 * 1024;

size_t
align_up (size_t value, size_t align)
{
	return (value + align - 1) & ~(align - 1);
}

/*
 * The cap is not a tuning choice: a slab larger than 2GB could put an object's
 * .rodata out of PCRel32 range of its own .text (codemem.hpp), so the ceiling
 * has to survive whatever the environment asks for.
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

	size = align_up (std::min (std::max (size, min_slab_size), max_slab_size),
	                 page_size);
	return size;
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
	slab->stub_bump = want;
	slab->stub_floor = want / page_size_;
	slabs_.push_back (std::move (slab));
	return Error::success ();
}

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

	if (start + size > s.stub_floor * page_size_)
		return false;

	s.bump = start + size;
	out = Alloc { s.base + start, size, index };
	s.live += size;
	return true;
}

Expected<CodeSlabs::Alloc>
CodeSlabs::allocate (size_t size, size_t align)
{
	if (size == 0)
		size = 1;
	if (align == 0)
		align = 1;

	std::lock_guard<std::mutex> lock (mutex_);
	Alloc alloc;

	for (size_t i = 0; i < slabs_.size (); i++)
		if (carve (*slabs_[i], i, size, align, alloc)) {
			if (Error err = open (alloc))
				return std::move (err);
			return alloc;
		}

	if (Error err = add_slab ())
		return std::move (err);

	if (!carve (*slabs_.back (), slabs_.size () - 1, size, align, alloc))
		return createStringError (
			inconvertibleErrorCode (),
			"a %zu byte allocation does not fit in a code slab", size);

	if (Error err = open (alloc))
		return std::move (err);
	return alloc;
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
	std::lock_guard<std::mutex> lock (mutex_);
	return close (a);
}

Error
CodeSlabs::abandon (const Alloc &a)
{
	std::lock_guard<std::mutex> lock (mutex_);
	Slab &s = *slabs_[a.slab];
	Error err = close (a);
	size_t offset = size_t (a.base - s.base);

	s.live -= a.size;

	/* Winding the bump pointer back recovers the alignment padding too, which
	 * putting the range on the free list would not. */
	if (offset + a.size == s.bump)
		s.bump = offset;
	else
		put_free (s, offset, a.size);

	return err;
}

void
CodeSlabs::release (const Alloc &a)
{
	std::lock_guard<std::mutex> lock (mutex_);
	Slab &s = *slabs_[a.slab];

	s.live -= a.size;
	put_free (s, size_t (a.base - s.base), a.size);
}

void
CodeSlabs::put_free (Slab &s, size_t offset, size_t length)
{
	auto next = s.free.lower_bound (offset);

	if (next != s.free.end () && offset + length == next->first) {
		length += next->second;
		next = s.free.erase (next);
	}
	if (next != s.free.begin ()) {
		auto prev = std::prev (next);

		if (prev->first + prev->second == offset) {
			offset = prev->first;
			length += prev->second;
			s.free.erase (prev);
		}
	}

	s.free[offset] = length;
}

Expected<CodeSlabs::Alloc>
CodeSlabs::allocate_stubs (size_t size, size_t align)
{
	if (size == 0)
		size = 1;
	if (align == 0)
		align = 1;

	std::lock_guard<std::mutex> lock (mutex_);

	for (size_t attempt = 0; attempt < 2; attempt++) {
		for (size_t i = 0; i < slabs_.size (); i++) {
			Slab &s = *slabs_[i];

			if (s.stub_bump < size)
				continue;

			size_t start = (s.stub_bump - size) & ~(align - 1);
			size_t floor = start / page_size_;

			if (floor * page_size_ < align_up (s.bump, page_size_))
				continue;

			if (floor < s.stub_floor) {
				if (mprotect (s.base + floor * page_size_,
				              (s.stub_floor - floor) * page_size_,
				              prot_flags (true)) != 0)
					return createStringError (
						std::error_code (errno,
						                 std::generic_category ()),
						"cannot commit JIT stub pages");
				s.committed_pages += s.stub_floor - floor;
				s.stub_floor = floor;
			}

			s.stub_bump = start;
			return Alloc { s.base + start, size, i };
		}

		if (attempt == 0)
			if (Error err = add_slab ())
				return std::move (err);
	}

	return createStringError (inconvertibleErrorCode (),
	                          "a %zu byte stub allocation does not fit in a "
	                          "code slab",
	                          size);
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

} // namespace mono

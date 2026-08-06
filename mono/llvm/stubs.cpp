/**
 * \file
 * \brief A slab-allocated table of the redirectable stubs methods are published
 * as.
 *
 * ORC ships redirectable symbols of its own (JITLinkRedirectableSymbolManager),
 * but it builds and links a LinkGraph per batch of stubs, and the runtime
 * publishes stubs one method at a time. Each graph gets its own allocation,
 * whose segments are page-aligned, so a lone stub costs one page of code plus
 * one of data: 8K a method, which is most of a gigabyte over a game's worth of
 * them. Redirecting likewise goes through a full symbol lookup.
 *
 * So we carve stubs out of a slab instead. A stub costs 24 bytes plus an entry
 * in this table, publishing one is a bump-allocate (or a pop off the free list)
 * plus a few stores, and a redirect is a single atomic store to the slot.
 */

#include "stubs.hpp"

#include "arch/arch.hpp"
#include "codemem.hpp"

#include <llvm/Support/Memory.h>

#include <utility>

using namespace llvm;

namespace mono {

/*
 * How many stubs a batch holds. The slot and stub regions come out of one
 * allocation so the jump's rel32 displacement always reaches, and the slot
 * region is a whole number of 16-byte blocks at this count, which keeps the
 * stub region aligned without padding between them.
 */
namespace {
constexpr size_t stubs_per_slab = 2048;
constexpr size_t slot_region_size = stubs_per_slab * sizeof (void *);
constexpr size_t stub_region_size = stubs_per_slab * arch::stub_block_size;
} // namespace

/// Allocator over batches of (slot region, stub region) pairs carved from the
/// code slabs: bump within a batch, with a free list of the stubs handed back.
class StubSlabs {
public:
	explicit StubSlabs (CodeSlabs *slabs) : slabs_ (slabs) {}

	~StubSlabs ()
	{
		for (const CodeSlabs::Alloc &batch : batches_)
			slabs_->release_writable (batch);
	}

	/// Carve one stub, jumping to TARGET.
	Expected<StubTable::Stub> allocate (void *target)
	{
		if (!free_.empty ()) {
			StubTable::Stub stub = free_.back ();

			free_.pop_back ();
			/*
			 * The jump reads this stub's own slot and always did, so
			 * there is nothing to rewrite but the destination.
			 */
			stub.slot->store (target, std::memory_order_release);
			return stub;
		}

		if (next_ == stubs_per_slab)
			if (Error err = add_slab ())
				return std::move (err);

		char *base = batches_.back ().base;
		size_t i = next_++;

		auto *slot = reinterpret_cast<std::atomic<void *> *> (
			base + i * sizeof (void *));
		char *code = base + slot_region_size + i * arch::stub_block_size;

		slot->store (target, std::memory_order_release);
		arch::write_jump_stub (code, slot);
		sys::Memory::InvalidateInstructionCache (code, arch::stub_block_size);

		return StubTable::Stub { code, slot };
	}

	/// Take STUB back, for a later allocate () to hand out again.
	void release (StubTable::Stub stub) { free_.push_back (stub); }

private:
	/*
	 * The writable region, so stubs are never flipped to read-execute once
	 * written: they are carved one at a time out of a page other stubs are
	 * already running from, so there is no point at which the page is quiet
	 * enough to reprotect. A detour library mprotects the entry it patches
	 * anyway.
	 */
	Error add_slab ()
	{
		Expected<CodeSlabs::Alloc> batch = slabs_->allocate_writable (
			slot_region_size + stub_region_size, arch::stub_block_size);

		if (!batch)
			return batch.takeError ();

		batches_.push_back (*batch);
		next_ = 0;
		return Error::success ();
	}

	CodeSlabs *slabs_;
	std::vector<CodeSlabs::Alloc> batches_;
	std::vector<StubTable::Stub> free_;
	size_t next_ = stubs_per_slab;
};

Expected<std::unique_ptr<StubTable>>
StubTable::create (const Triple &tt, CodeSlabs &slabs)
{
	if (tt.getArch () != arch::target_arch)
		return make_error<StringError> (
			"redirectable stubs are not implemented for " + tt.str (),
			inconvertibleErrorCode ());

	return std::unique_ptr<StubTable> (
		new StubTable (std::make_unique<StubSlabs> (&slabs)));
}

StubTable::StubTable (std::unique_ptr<StubSlabs> slabs)
	: slabs_ (std::move (slabs))
{
}

StubTable::~StubTable () = default;

Expected<void *>
StubTable::reserve (StringRef name, void *target)
{
	std::lock_guard<std::mutex> lock (mutex_);

	if (stubs_.count (name))
		return make_error<StringError> ("a stub is already published for "
		                                    + name,
		                                inconvertibleErrorCode ());

	Expected<Stub> stub = slabs_->allocate (target);

	if (!stub)
		return stub.takeError ();

	stubs_[name] = Entry { *stub, false };
	return stub->code;
}

void *
StubTable::find (StringRef name)
{
	std::lock_guard<std::mutex> lock (mutex_);
	auto it = stubs_.find (name);

	return it == stubs_.end () ? nullptr : it->second.stub.code;
}

Error
StubTable::redirect (StringRef name, void *target)
{
	std::lock_guard<std::mutex> lock (mutex_);
	auto it = stubs_.find (name);

	if (it == stubs_.end ())
		return make_error<StringError> ("no stub to redirect for " + name,
		                                inconvertibleErrorCode ());

	/*
	 * Callers may be running through this stub right now: the store has to
	 * land whole, and everything the new target reads has to be visible by the
	 * time it does.
	 */
	it->second.stub.slot->store (target, std::memory_order_release);
	return Error::success ();
}

void *
StubTable::claim_for_linker (StringRef name)
{
	std::lock_guard<std::mutex> lock (mutex_);
	auto it = stubs_.find (name);

	if (it == stubs_.end () || it->second.defined)
		return nullptr;

	it->second.defined = true;
	return it->second.stub.code;
}

Expected<StubTable::Removed>
StubTable::remove (ArrayRef<std::string> names)
{
	std::lock_guard<std::mutex> lock (mutex_);
	Removed removed;

	/*
	 * All of them or none: the caller is working from its own record of what
	 * it published, so a name that was never here means that record is wrong,
	 * and half a batch removed would leave it wrong in a second way.
	 */
	for (const std::string &name : names)
		if (!stubs_.count (name))
			return make_error<StringError> ("no stub was published for "
			                                    + name,
			                                inconvertibleErrorCode ());

	for (const std::string &name : names) {
		auto it = stubs_.find (name);

		if (it->second.defined)
			removed.defined.push_back (name);
		removed.blocks.push_back (it->second.stub);
		stubs_.erase (it);
	}

	return removed;
}

void
StubTable::reclaim (Removed &&removed)
{
	std::lock_guard<std::mutex> lock (mutex_);

	for (const Stub &stub : removed.blocks)
		slabs_->release (stub);
}

} // namespace mono

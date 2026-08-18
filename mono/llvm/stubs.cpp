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
 * So we carve stubs out of one batch of code memory instead. A stub costs
 * stub_group_size bytes plus an entry in this table, publishing one is a
 * bump-allocate (or a pop off the free list) plus a few stores, and a redirect
 * is a single atomic store to the slot.
 */

#include "stubs.hpp"

#include "arch/amd64/amd64.hpp"
#include "arch/arch.hpp"
#include "jitlink-memory.hpp"
#include "debugging/perf/jitdump.hpp"

#include <mono/arch/amd64/amd64-thunk.hpp>
#include "mono/metadata/abi-details.h"
#include "mono/metadata/object.h"

#include <llvm/Support/raw_ostream.h>
#include <llvm/ADT/StringRef.h>
#include <llvm/Support/Memory.h>

#include <mutex>
#include <utility>

using namespace llvm;

namespace mono {

namespace {

/*
 * One stub's group: the slot it jumps through, the unbox prologue that runs into
 * it, and the block itself.
 *
 *      +0  slot                    the jump's target, and the group's base
 *      +8  int3 padding            unreachable
 *     +12  addq $16, %rdi          the unbox entry
 *     +16  the stub block
 *
 * The slot leads, so a Stub finds its group again from the slot alone and the
 * free list needs nothing else. stub_alignment decides where the block sits, and
 * the padding is whatever that leaves in front of the prologue. One allocation
 * holds all three, so the jump's rel32 displacement to its own slot always
 * reaches.
 */
constexpr size_t slot_size = sizeof (void *);
constexpr size_t stub_offset =
	(slot_size + arch::unbox_prologue_size + arch::stub_alignment - 1)
	& ~(arch::stub_alignment - 1);
constexpr size_t group_size = stub_offset + arch::stub_block_size;

static_assert (group_size % arch::stub_alignment == 0,
               "a group has to leave the group behind it aligned");

/* amd64-thunk.hpp bakes this layout as constant bytes rather than assembling
 * it at runtime; these tie the two together so the baked bytes cannot
 * silently drift out of step with the geometry carved here. */
static_assert (arch::thunk_entry_offset == stub_offset,
               "the baked stub bytes assume a different group layout");
static_assert (arch::thunk_size == group_size,
               "the baked stub bytes assume a different group size");

/* The baked unbox prologue adds a fixed 0x10; MonoObject's layout is what has
 * to hold still for that to keep stepping past exactly the header. */
static_assert (MONO_ABI_SIZEOF (MonoObject) == 0x10,
               "the baked unbox prologue assumes a different MonoObject size");

/// How many groups a batch holds.
constexpr size_t stubs_per_slab = 2048;

void
stub_not_initialized ()
{
	llvm::report_fatal_error ("called a stub that has not been initialized with a target");
}

} // namespace

const size_t stub_group_size = group_size;

void *
Stub::unbox_entry () const
{
	return static_cast<char *> (code_) - arch::unbox_prologue_size;
}

/*
 * A perf dump's record keeps its frame description in the room past the code it
 * names. A second record inside that room takes it, so the slack separates the
 * groups while a dump is open. The slack is a multiple of 16, so a wider stride
 * leaves every group as aligned as a tight one.
 */
StubSlabs::StubSlabs (CodeArena *arena)
	: arena_ (arena), next_ (stubs_per_slab),
	  stride_ (group_size + perf::code_slack ())
{
}

llvm::Expected<Stub>
StubSlabs::allocate (void *key)
{
	auto stub = acquire ();
	if (!stub)
		return stub.takeError ();

	char *group = static_cast<char *> (stub->code ()) - stub_offset;
	char *prologue = static_cast<char *> (stub->unbox_entry ());

	/* Fills the whole group, slot included; the group is either fresh or
	 * already retired to the free list, so nothing can be reading the slot
	 * this leaves null. The redirect below is what makes it live. */
	arch::write_thunk (group, key);
	stub->redirect ((void *) stub_not_initialized);

	sys::Memory::InvalidateInstructionCache (
		prologue, arch::unbox_prologue_size + arch::stub_block_size);

	return stub;
}

void
StubSlabs::release (Stub stub)
{
	if (!stub)
		return;

	stub.redirect ((void *) stub_not_initialized);
	free_.push_back (stub);
}

llvm::Expected<Stub>
StubSlabs::acquire ()
{
	if (!free_.empty ()) {
		auto stub = free_.back ();
		free_.pop_back ();
		return stub;
	}

	if (next_ == stubs_per_slab) {
		if (auto err = add_slab ())
			return err;
	}

	char *group = batch_ + (next_++) * stride_;

	return Stub (group + stub_offset,
	             reinterpret_cast<std::atomic<void *> *> (group));
}

llvm::Error
StubSlabs::add_slab ()
{
	llvm::Expected<char *> batch =
		arena_->reserve (stubs_per_slab * stride_, arch::stub_alignment);

	if (!batch)
		return batch.takeError ();

	batch_ = *batch;
	next_ = 0;
	return llvm::Error::success ();
}

std::optional<Stub>
StubTable::find (llvm::StringRef name)
{
	std::lock_guard<std::mutex> lock (mutex_);

	auto it = stubs_.find (name);
	if (it == stubs_.end ())
		return std::nullopt;

	return it->second;
}

llvm::Expected<Stub>
StubTable::create (llvm::StringRef name)
{
	return create (name, nullptr);
}

llvm::Expected<Stub>
StubTable::create (llvm::StringRef name, void *key)
{
	std::lock_guard<std::mutex> lock (mutex_);

	auto it = stubs_.find (name);
	if (it != stubs_.end ())
		return llvm::make_error<StubExistsError> (name);

	auto stub = slabs_.allocate (key);
	if (!stub)
		return stub.takeError ();

	stubs_.insert (std::make_pair (name, *stub));
	return *stub;
}

bool
StubTable::remove (llvm::StringRef name)
{
	std::lock_guard<std::mutex> lock (mutex_);
	return remove_locked (name, lock);
}

bool
StubTable::remove_locked (llvm::StringRef name, std::lock_guard<std::mutex> &)
{
	auto it = stubs_.find (name);
	if (it == stubs_.end ())
		return false;

	auto stub = it->second;
	stubs_.erase (it);
	slabs_.release (stub);
	return true;
}

char StubExistsError::ID = 0;

StubExistsError::StubExistsError (llvm::StringRef name) : name (name) {}

void
StubExistsError::log (llvm::raw_ostream &OS) const
{
	OS << "a stub already exists for " << name;
}

std::error_code
StubExistsError::convertToErrorCode () const
{
	return std::error_code ();
}

} // namespace mono

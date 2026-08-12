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

#include "arch/amd64/amd64.hpp"
#include "arch/arch.hpp"
#include "codemem.hpp"
#include "debugging/perf/jitdump.hpp"

#include <llvm/Support/raw_ostream.h>
#include <llvm/ADT/StringRef.h>
#include <llvm/Support/Memory.h>

#include <mutex>
#include <utility>

using namespace llvm;

namespace mono {

namespace {

/*
 * How many stubs a batch holds. The slot and stub regions come out of one
 * allocation so the jump's rel32 displacement always reaches, and the slot
 * region is a whole number of 16-byte blocks at this count, which keeps the
 * stub region aligned without padding between them.
 */
constexpr size_t stubs_per_slab = 2048;
constexpr size_t slot_region_size = stubs_per_slab * sizeof (void *);

void
stub_not_initialized ()
{
	llvm::report_fatal_error ("called a stub that has not been initialized with a target");
}

} // namespace

/*
 * A stub is a jump and never fills its block. The rest of the block is the room
 * a perf dump's record for one stub keeps its frame description in, and a second
 * stub inside that room would take it. The slack is a multiple of 16, so a wider
 * block leaves every stub as aligned as a bare one.
 */
StubSlabs::StubSlabs (CodeSlabs *slabs)
	: slabs_ (slabs), next_ (stubs_per_slab),
	  stride_ (arch::stub_block_size + perf::code_slack ())
{
}

StubSlabs::~StubSlabs ()
{
	for (const CodeSlabs::Alloc &batch : batches_)
		slabs_->release_writable (batch);
}

llvm::Expected<Stub>
StubSlabs::allocate (void *key)
{
	auto stub = acquire ();
	if (!stub)
		return stub.takeError ();
	stub->redirect ((void *) stub_not_initialized);

	char *code = static_cast<char *> (stub->code ());

	if (key != nullptr)
		arch::write_keyed_jump_stub (code, stub->slot_, key);
	else
		arch::write_jump_stub (code, stub->slot_);
	sys::Memory::InvalidateInstructionCache (code, arch::stub_block_size);

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

	char *base = batches_.back ().base;
	size_t i = next_++;

	return Stub (base + slot_region_size + i * stride_,
	             reinterpret_cast<std::atomic<void *> *> (base + i * sizeof (void *)));
}

llvm::Error
StubSlabs::add_slab ()
{
	llvm::Expected<CodeSlabs::Alloc> batch = slabs_->allocate_writable (
		slot_region_size + stubs_per_slab * stride_, arch::stub_block_size);

	if (!batch)
		return batch.takeError ();

	batches_.push_back (*batch);
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

llvm::Expected<Stub>
StubTable::get_or_create (llvm::StringRef name)
{
	return get_or_create (name, nullptr);
}

llvm::Expected<Stub>
StubTable::get_or_create (llvm::StringRef name, void *key)
{
	std::lock_guard<std::mutex> lock (mutex_);

	auto it = stubs_.find (name);
	if (it != stubs_.end ())
		return it->second;

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

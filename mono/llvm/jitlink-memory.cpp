#include "jitlink-memory.hpp"

#include "debugging/perf/jitdump.hpp"
#include "timing.hpp"

#include <llvm/ADT/STLExtras.h>
#include <llvm/ExecutionEngine/JITLink/JITLink.h>
#include <llvm/Support/Memory.h>

#include <algorithm>
#include <cassert>
#include <climits>
#include <cstdint>
#include <cstring>
#include <optional>
#include <utility>

#include <glib.h>

#include "mono/utils/mono-codeman.h"

using namespace llvm;

namespace mono {
namespace {

/// What mono_code_manager_reserve_align () will accept and guarantee, which is
/// MIN_ALIGN in mono/utils/mono-codeman.c.
constexpr size_t code_manager_alignment = 16;

/// One reservation cannot be larger than this, because the code manager counts
/// in int.
constexpr size_t max_reservation = size_t (INT_MAX);

/*
 * Code chunks are mapped MAP_32BIT (ARCH_MAP_FLAGS in mono/utils/mono-codeman.c).
 * So every address the code manager hands out is below 2GB, and any two of them
 * are inside what a 32-bit displacement can encode.
 *
 * The backend needs that. jit.cpp asks for CodeModel::Small with PIC
 * relocations, so a method that reaches its own .rodata or a mutable global does
 * it with a PCRel32. Unlike a call, that has no stub to fall back on.
 *
 * A port without MAP_32BIT has to solve reach some other way. Until it does, the
 * check below is what says so. The alternative is a JITLink relocation error
 * that names none of this.
 */
constexpr uintptr_t code_reach_limit = uintptr_t (2) * 1024 * 1024 * 1024;

} // namespace

// A private code manager and mutex, not mono_mem_manager_code_reserve ():
// that call takes mono_domain_lock (), and reserve () runs with JITLink's
// locks held. A mutator can arrive at those locks while it holds the domain
// lock already, so taking the two in that order deadlocks.
CodeArena::CodeArena () : code_ (mono_code_manager_new ()) {}

CodeArena::~CodeArena ()
{
	mono_code_manager_destroy (code_);
}

Expected<char *>
CodeArena::reserve (size_t size, size_t align)
{
	if (align == 0)
		align = 1;
	if (size == 0)
		size = 1;

	size_t want = size;

	if (align > code_manager_alignment)
		want += align - 1;

	if (want > max_reservation)
		return createStringError (inconvertibleErrorCode (),
		                          "%zu bytes of code memory is more than one "
		                          "reservation can hold",
		                          size);

	void *reserved = nullptr;

	{
		std::lock_guard<std::mutex> lock (mutex_);

		reserved = mono_code_manager_reserve_align (
			code_, int (want), int (std::min (align, code_manager_alignment)));
	}

	if (reserved == nullptr)
		return createStringError (inconvertibleErrorCode (),
		                          "out of code memory: cannot reserve %zu bytes",
		                          want);

	uintptr_t base = uintptr_t (reserved);
	uintptr_t aligned = (base + align - 1) & ~(uintptr_t (align) - 1);

	if (aligned + size > code_reach_limit)
		return createStringError (inconvertibleErrorCode (),
		                          "code memory at %p is out of PCRel32 reach of "
		                          "the rest of the code the runtime has compiled",
		                          reserved);

	return reinterpret_cast<char *> (aligned);
}

void
CodeArena::unreserve (char *base, size_t size)
{
	if (base == nullptr || size == 0)
		return;

	std::lock_guard<std::mutex> lock (mutex_);

	mono_code_manager_commit (code_, base, int (size), 0);
}

class CodeMemoryManager::InFlight final : public jitlink::JITLinkMemoryManager::InFlightAlloc {
public:
	InFlight (CodeMemoryManager *owner, jitlink::LinkGraph &g, char *base, size_t size)
	    : owner_ (owner), graph_ (&g), base_ (base), size_ (size)
	{
	}

	~InFlight () override
	{
		assert (settled_
		        && "in-flight allocation neither finalized nor "
		           "abandoned");
	}

	void finalize (OnFinalizedFunction on_finalized) override
	{
		std::optional<timing::Scope> timed (std::in_place, timing::Phase::memfin);

		settled_ = true;

		/*
		 * Before the allocation is declared finished, because
		 * __register_frame reads the .eh_frame bytes and an alloc action
		 * is entitled to write into the allocation it belongs to.
		 */
		Expected<std::vector<orc::shared::WrapperFunctionCall>> dealloc =
			orc::shared::runFinalizeActions (graph_->allocActions ());

		if (!dealloc)
			return on_finalized (dealloc.takeError ());

		sys::Memory::InvalidateInstructionCache (base_, size_);

		FinalizedAlloc finalized =
			owner_->record (base_, size_, std::move (*dealloc));

		timed.reset ();
		on_finalized (std::move (finalized));
	}

	void abandon (OnAbandonedFunction on_abandoned) override
	{
		settled_ = true;
		owner_->arena_->unreserve (base_, size_);
		on_abandoned (Error::success ());
	}

private:
	CodeMemoryManager *owner_;
	jitlink::LinkGraph *graph_;
	char *base_;
	size_t size_;
	bool settled_ = false;
};

void
CodeMemoryManager::allocate (const jitlink::JITLinkDylib *, jitlink::LinkGraph &g,
                             OnAllocatedFunction on_allocated)
{
	jitlink::BasicLayout layout (g);
	std::vector<std::pair<jitlink::BasicLayout::Segment *, uint64_t>> placed;
	uint64_t extent = 0;
	Align align (1);

	for (auto &kv : layout.segments ()) {
		if (kv.first.getMemLifetime () != orc::MemLifetime::Standard)
			return on_allocated (make_error<StringError> (
				"graph " + g.getName ()
					+ " wants a finalize-lifetime segment, which "
					  "this target does not produce",
				inconvertibleErrorCode ()));

		jitlink::BasicLayout::Segment &seg = kv.second;

		extent = alignTo (extent, seg.Alignment);
		placed.emplace_back (&seg, extent);
		extent += seg.ContentSize + seg.ZeroFillSize;
		align = std::max (align, seg.Alignment);
	}

	// Nothing reads these bytes. They keep the next object out of the range a
	// perf dump gives this one's frame descriptions.
	size_t size = extent + (extent != 0 ? perf::code_slack () : 0);

	Expected<char *> base = arena_->reserve (size, align.value ());

	if (!base)
		return on_allocated (base.takeError ());

	for (auto &[seg, offset] : placed) {
		seg->Addr = orc::ExecutorAddr::fromPtr (*base + offset);
		seg->WorkingMem = seg->Addr.toPtr<char *> ();

		/* The stock in-process manager zeroes its whole slab up front. An
		 * arena everything shares has to be zeroed a segment at a time. */
		memset (seg->WorkingMem + seg->ContentSize, 0, seg->ZeroFillSize);
	}

	if (Error err = layout.apply ()) {
		arena_->unreserve (*base, size);
		return on_allocated (std::move (err));
	}

	on_allocated (std::make_unique<InFlight> (this, g, *base, size));
}

jitlink::JITLinkMemoryManager::FinalizedAlloc
CodeMemoryManager::record (char *base, size_t size,
                           std::vector<orc::shared::WrapperFunctionCall> actions)
{
	std::lock_guard<std::mutex> lock (mutex_);
	FinalizedInfo *info = infos_.Allocate<FinalizedInfo> ();

	new (info) FinalizedInfo{base, size, std::move (actions)};
	return FinalizedAlloc (orc::ExecutorAddr::fromPtr (info));
}

void
CodeMemoryManager::deallocate (std::vector<FinalizedAlloc> allocs,
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
			if (Error err = info->dealloc_actions.back ().runWithSPSRetErrorMerged ())
				all = joinErrors (std::move (all), std::move (err));
			info->dealloc_actions.pop_back ();
		}

		std::lock_guard<std::mutex> lock (mutex_);
		info->~FinalizedInfo ();
		infos_.Deallocate (info);
	}

	on_deallocated (std::move (all));
}

} // namespace mono

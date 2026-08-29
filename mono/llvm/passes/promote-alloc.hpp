/**
 * \file
 * \brief Turning an allocation nothing outside the function reaches into a slot
 * in the frame.
 */

#ifndef MONO_LLVM_PASSES_PROMOTE_ALLOC_HPP
#define MONO_LLVM_PASSES_PROMOTE_ALLOC_HPP

#include <llvm/IR/PassManager.h>

namespace llvm {
class AAResults;
class Function;
class LoopInfo;
} // namespace llvm

namespace mono {

/// The largest object this takes to the frame, in bytes.
///
/// A frame is not a heap. The bound is what stops a method with many promotable
/// allocations, or one large one, from running the stack out.
constexpr uint64_t promote_alloc_limit = 1024;

/// Rewrites each allocation in \p f that nothing outside can reach into an
/// alloca, and says whether it changed anything.
///
/// The alloca is the same size and carries the same alignment the collector
/// gives, so every offset the translator wrote still lands where it did. It is
/// zeroed where the allocation stood, because the allocation claims `zeroed` and
/// a frame slot holds what the last frame left.
///
/// What this buys is the passes behind it. SROA reaches an alloca and never an
/// allocation, so the fields become registers and the stores that fill them go
/// with the reads. `fold_stack_barriers ()` (`passes/fold-barrier.hpp`) takes
/// the write barriers, because a frame slot owes no card.
///
/// Four things keep an allocation on the heap. A `.kept` form says the program
/// can tell the allocation happened. An invoke site names pads its edges would
/// have to be repaired for. A size the compile cannot read, or one over
/// `promote_alloc_limit`, has no frame slot to be. And a site inside a loop
/// would hand every turn the same slot, where the heap gives each one an object
/// of its own.
///
/// A promoted object also gives up `!invariant.load` on the reads that can reach
/// it, which \p aa is what decides. The allocator wrote the object's header
/// before, and a store in this function writes it now, so the mark would put a
/// reader in front of that store and leave DSE nothing to keep it for.
bool promote_allocations (llvm::Function &f, const llvm::LoopInfo &loops,
                          llvm::AAResults &aa);

/// Runs promote_allocations () over one function.
class PromoteAllocationsPass : public llvm::PassInfoMixin<PromoteAllocationsPass> {
public:
	llvm::PreservedAnalyses run (llvm::Function &f, llvm::FunctionAnalysisManager &fam);
};

} // namespace mono

#endif

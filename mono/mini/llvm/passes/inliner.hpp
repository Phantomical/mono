/**
 * \file
 * inliner.hpp - mono's tier-1 LLVM inliner, packaged as the -O2 pipeline's
 * inlining stage.
 *
 * The tier-1 pipeline is the stock per-module -O2 pipeline with exactly one
 * entry swapped out: LLVM's own inlining stage is replaced by a mono pass that
 * pulls callee bodies into the module (they arrive as bodyless trampoline
 * declarations otherwise) and then drives that same stock inliner over them,
 * round by round, until nothing more folds into the root. inliner.cpp has the
 * algorithm and the gates deciding what may be pulled in.
 */

#ifndef MONO_MINI_LLVM_INLINER_HPP
#define MONO_MINI_LLVM_INLINER_HPP

/*
 * Same reason engine.cpp drops mono's PIC macro: PassBuilder.h uses `PIC` as an
 * identifier, so the macro would rewrite it and break the header.
 */
#ifdef PIC
#undef PIC
#endif

#include <memory>
#include <utility>

#include <llvm/ADT/DenseSet.h>
#include <llvm/ADT/SmallVector.h>
#include <llvm/IR/PassManager.h>
#include <llvm/Passes/OptimizationLevel.h>

namespace llvm {
class Function;
class PassBuilder;
class PassInstrumentationCallbacks;
}

namespace mono {

/*
 * Shared between the pass and the instrumentation callbacks build_tier1_pipeline
 * () registers alongside it. The pass object gets copied into the pipeline's
 * type-erased pass model, so this lives behind a shared_ptr rather than in the
 * pass itself.
 */
struct RoundState;

/*
 * The tier-1 inlining stage. It takes the slot LLVM's own inlining stage would
 * occupy in the -O2 pipeline, which means it also carries the per-function
 * simplification pipeline nested inside that slot.
 */
class MonoInlinerPass : public llvm::PassInfoMixin<MonoInlinerPass> {
public:
	MonoInlinerPass (llvm::PassBuilder &pb, llvm::OptimizationLevel level,
	                 std::shared_ptr<RoundState> state)
	    : pb_ (&pb), level_ (level), state_ (std::move (state))
	{
	}

	llvm::PreservedAnalyses run (llvm::Module &m, llvm::ModuleAnalysisManager &mam);

	/*
	 * This pass carries the per-function simplification pipeline that the stock
	 * inlining stage it replaced used to carry, so skipping it would quietly
	 * turn -O2 into something much weaker.
	 */
	static bool isRequired () { return true; }

private:
	void expose_callees (llvm::Module &m, llvm::Function &root, void *root_cfg,
	                     llvm::DenseSet<void *> &refused,
	                     llvm::SmallVectorImpl<llvm::Function *> &added);
	void run_stock_inliner (llvm::Module &m, llvm::ModuleAnalysisManager &mam,
	                        bool module_mutated);

	/*
	 * Held by pointer, not reference: a reference member would delete the
	 * class's implicit copy/move-assignment and has non-obvious
	 * lifetime/rebinding behaviour. The PassBuilder outlives the pass (it owns
	 * the pipeline the pass is spliced into).
	 */
	llvm::PassBuilder *pb_;
	llvm::OptimizationLevel level_;
	std::shared_ptr<RoundState> state_;
};

/*
 * Build the per-module default pipeline at LEVEL with mono's tier-1 inliner
 * substituted for LLVM's stock inlining stage, in that stage's exact position.
 * PIC must be the same instrumentation-callbacks object PB was constructed
 * with - the pass observes the stock inliner's per-function analysis
 * invalidations through it to tell whether a round changed a root.
 */
llvm::ModulePassManager
build_tier1_pipeline (llvm::PassBuilder &pb,
                      llvm::PassInstrumentationCallbacks &pic,
                      llvm::OptimizationLevel level);

} // namespace mono

#endif /* MONO_MINI_LLVM_INLINER_HPP */

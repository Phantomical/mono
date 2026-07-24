/**
 * \file
 * inliner.hpp - the top-down tier-1 LLVM inliner (new-PM module pass).
 *
 * This is the LLVM-side inliner for promoted (tier-1) methods. It is a genuine
 * top-down, budget-driven inliner in the end; this first slice (S0) only stands
 * up the new-PM plumbing and proves the three primitives compose once:
 * reach the FunctionAnalysisManager through the module proxy, run the stock
 * function-simplification pipeline on one function, and InlineFunction one call
 * site. Lazy cross-module materialization, the fixpoint loop, the budget, and
 * the eligibility filter are later slices.
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

#include <llvm/IR/PassManager.h>
#include <llvm/Passes/OptimizationLevel.h>

namespace llvm {
class PassBuilder;
}

namespace mono {

/*
 * The top-down inliner, packaged as a new-PM module pass slotted into the
 * otherwise-intact -O2 pipeline (see register_top_down_inliner ()). It holds a
 * reference to the PassBuilder that owns it so run () can build the same
 * function-simplification pipeline the stock CGSCC adaptor runs, and the
 * OptimizationLevel that pipeline is built at.
 */
class MonoTopDownInlinerPass
    : public llvm::PassInfoMixin<MonoTopDownInlinerPass> {
public:
	MonoTopDownInlinerPass (llvm::PassBuilder &pb, llvm::OptimizationLevel level)
	    : pb_ (pb), level_ (level)
	{
	}

	llvm::PreservedAnalyses run (llvm::Module &m,
	                             llvm::ModuleAnalysisManager &mam);

private:
	llvm::PassBuilder &pb_;
	llvm::OptimizationLevel level_;
};

/*
 * Insert MonoTopDownInlinerPass into PB's default pipeline at the pipeline-start
 * extension point, so it runs before the stock CGSCC inliner. Call this on the
 * PassBuilder before building the per-module default pipeline.
 */
void register_top_down_inliner (llvm::PassBuilder &pb);

} // namespace mono

#endif /* MONO_MINI_LLVM_INLINER_HPP */

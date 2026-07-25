/**
 * \file
 * inliner.hpp - mono's tier-1 LLVM inliner, packaged as the -O2 pipeline's
 * inlining stage.
 *
 * The tier-1 pipeline is the stock per-module -O2 pipeline with exactly one
 * entry swapped out: LLVM's own inlining stage is replaced by a mono pass that
 * pulls callee bodies into the module (they arrive as bodyless trampoline
 * declarations otherwise) and then drives that same stock inliner over them,
 * round by round, until nothing more folds into the root. build_tier1_pipeline
 * () is the whole public surface; inliner.cpp has the algorithm.
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
class PassInstrumentationCallbacks;
}

namespace mono {

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

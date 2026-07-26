/**
 * \file
 * \brief The top-down inlining pass used by mono's LLVM JIT tier1 pipeline.
 *
 * Normally, LLVM inlining is done bottom-up. Leaf functions get inlined into
 * their callers and so on, until they get big enough to not be worth inlining.
 * Unfortunately, when we compile a method we don't have access to the LLVM IR
 * for all the methods. Even if we did, we couldn't afford to compile everything.
 * As such, we take a bit of a hybrid approach:
 *
 * 1. Start with one or more "root" methods. These are the methods that we
 *    are actually trying to compile.
 * 2. Add methods called by our root methods to the module. We do this recursively
 *    up to a depth of 2.
 * 3. Run the regular function optimization pipeline on these methods in order
 *    to simplify them as much as possible.
 * 4. Run LLVM's usual bottom-up inliner on the module.
 * 5. If any of the root functions have been modified (i.e. something got inlined
 *    into them) then go back to step 2 with the new inlined function body.
 * 6. Otherwise strip out any non-root function then continue down the pipeline.
 *
 * This is loosely based on these EuroLLVM slides for Azul's Falcon JIT:
 * https://llvm.org/devmtg/2022-05/slides/2022EuroLLVM-CustomBenefitDrivenInliner-in-FalconJIT.pdf
 */

#ifndef MONO_MINI_LLVM_INLINER_HPP
#define MONO_MINI_LLVM_INLINER_HPP

/* MonoCompile and MonoMethod, the two mono types the pass threads around. */
#include <mono/mini/mini.h>

/*
 * PassBuilder.h uses PIC as an identifier so we need to undef it. This has to
 * come after mini.h: libtool passes -DPIC, and mono-tls.h defines it itself
 * when we are built as PIE, so it is back in scope after any mono header.
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
} // namespace llvm

namespace mono {

struct RoundState;

/*
 * A hybrid top-down/bottom-up inlining pass.
 */
class MonoInlinerPass : public llvm::PassInfoMixin<MonoInlinerPass> {
public:
	MonoInlinerPass (llvm::PassBuilder &pb, llvm::OptimizationLevel level,
	                 std::shared_ptr<RoundState> state)
	    : pb_ (&pb), level_ (level), state_ (std::move (state))
	{
	}

	llvm::PreservedAnalyses run (llvm::Module &m, llvm::ModuleAnalysisManager &mam);

	static bool isRequired () { return true; }

private:

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
llvm::ModulePassManager build_tier1_pipeline (llvm::PassBuilder &pb,
                                              llvm::PassInstrumentationCallbacks &pic,
                                              llvm::OptimizationLevel level);

} // namespace mono

#endif /* MONO_MINI_LLVM_INLINER_HPP */

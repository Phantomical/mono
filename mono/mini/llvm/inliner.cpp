/**
 * \file
 * inliner.cpp - the top-down tier-1 LLVM inliner (new-PM module pass), S0.
 *
 * S0 proves the new-PM plumbing end to end on the simplest real case a tier-1
 * module actually contains: the self-recursive direct call the translator emits
 * for a recursive method (translator-call.cpp, `cfg->method == call->method`).
 * Every other call in a tier-1 module lowers to `call @trampoline_symbol`, a
 * declaration with no body, so the self-recursive call is the one direct call to
 * a co-present definition we can inline without any of the cross-module
 * materialization machinery that lands in S1.
 *
 * What this pass does, once, per module:
 *   1. Reach the FunctionAnalysisManager through the module->function proxy.
 *   2. Build and run the stock function-simplification pipeline on the root, so
 *      the inline sees canonical IR (exactly what the stock CGSCC adaptor runs).
 *   3. InlineFunction one self-recursive call site.
 *   4. Invalidate the root's analyses (InlineFunction is not new-PM aware), then
 *      verify the module so any malformed IR aborts here instead of miscompiling.
 *
 * No fixpoint, no budget, no priority, no eligibility filter beyond the two
 * safety guards below - those are S1-S3.
 *
 * Why the stock CGSCC inliner stays inert: the only call this pass ever touches
 * is a self-recursive one, which LLVM's own inliner already refuses to inline
 * (recursion). Inlining one level of a self-call leaves behind only more
 * self-recursive calls (in the cloned body and the untouched sites) plus the
 * usual `call @trampoline` declarations - the exact shape that has always kept
 * the stock inliner from firing. So the two inliners coexist and only this one
 * ever acts.
 */

#include "inliner.hpp"

#include <llvm/IR/Function.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/IR/InstrTypes.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/PassManager.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Pass.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/Transforms/Utils/Cloning.h>

using namespace llvm;

namespace mono {

/*
 * A call site is an S0 candidate when it is a plain self-recursive direct call:
 * the callee is resolved, is the containing function itself, and has a body. We
 * skip invokes and musttail calls - both carry semantics (EH edges, guaranteed
 * tail position) that one-shot inlining would have to preserve, and S0 has no
 * business touching them.
 */
static bool
is_self_recursive_candidate (Function &f, const CallBase &cb)
{
	if (isa<InvokeInst> (&cb))
		return false;
	if (cb.isMustTailCall ())
		return false;
	Function *callee = cb.getCalledFunction ();
	return callee == &f && !callee->isDeclaration ();
}

/*
 * Pick the single root for this S0 run: the first defined function carrying a
 * self-recursive candidate call. We skip functions with a personality function
 * (i.e. EH clauses): inlining a clause-bearing body into itself would collide
 * the un-namespaced clause indices the custom-emit EH path relies on, a silent
 * wrong-handler miscompile. Excluding EH functions wholesale here is the S0-safe
 * stand-in for the real clause-bearing-callee gate that lands in S3.
 */
static Function *
find_root (Module &m)
{
	for (Function &f : m) {
		if (f.isDeclaration ())
			continue;
		if (f.hasPersonalityFn ())
			continue;
		for (Instruction &i : instructions (f)) {
			if (auto *cb = dyn_cast<CallBase> (&i)) {
				if (is_self_recursive_candidate (f, *cb))
					return &f;
			}
		}
	}
	return nullptr;
}

static CallBase *
find_self_call (Function &f)
{
	for (Instruction &i : instructions (f)) {
		if (auto *cb = dyn_cast<CallBase> (&i)) {
			if (is_self_recursive_candidate (f, *cb))
				return cb;
		}
	}
	return nullptr;
}

PreservedAnalyses
MonoTopDownInlinerPass::run (Module &m, ModuleAnalysisManager &mam)
{
	Function *root = find_root (m);
	if (!root)
		return PreservedAnalyses::all ();

	FunctionAnalysisManager &fam =
		mam.getResult<FunctionAnalysisManagerModuleProxy> (m).getManager ();

	FunctionPassManager fpm =
		pb_.buildFunctionSimplificationPipeline (level_,
		                                         ThinOrFullLTOPhase::None);

	/*
	 * Simplify the root once before we inline, so the body we clone in is the
	 * canonicalized one - the same order the stock pipeline uses (simplify the
	 * function, then inline). This may move or fold instructions, so we locate
	 * the call site again afterward rather than holding a pointer across it.
	 */
	fpm.run (*root, fam);

	CallBase *cs = find_self_call (*root);
	if (!cs) {
		/* Simplification removed the self-call; the root still changed. */
		return PreservedAnalyses::none ();
	}

	InlineFunctionInfo ifi;
	InlineResult res = InlineFunction (*cs, ifi);
	if (!res.isSuccess ()) {
		/* Nothing inlined, but the simplification above did change the root. */
		return PreservedAnalyses::none ();
	}

	/*
	 * InlineFunction is not new-PM aware and preserves nothing, so drop every
	 * cached analysis for the root before anything else consults it. (The
	 * PreservedAnalyses::none () we return would also invalidate the whole
	 * FAM through the proxy, but doing it explicitly here is what the fixpoint
	 * loop in later slices needs between an inline and the next in-loop
	 * FPM.run, so establish the discipline now.)
	 */
	fam.invalidate (*root, PreservedAnalyses::none ());

	/*
	 * optimize () builds its PassBuilder without any verifier instrumentation,
	 * so a malformed-IR bug in the inline would surface as a JIT crash or a
	 * wrong result rather than a verifier error. Verify here so it aborts at
	 * the pass boundary instead. VerifierPass is fatal by default.
	 */
	VerifierPass ().run (m, mam);

	return PreservedAnalyses::none ();
}

void
register_top_down_inliner (PassBuilder &pb)
{
	pb.registerPipelineStartEPCallback (
		[&pb] (ModulePassManager &mpm, OptimizationLevel level) {
			mpm.addPass (MonoTopDownInlinerPass (pb, level));
		});
}

} // namespace mono

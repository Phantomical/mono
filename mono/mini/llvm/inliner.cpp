/**
 * \file
 * inliner.cpp - the top-down tier-1 LLVM inliner (new-PM module pass), S1.
 *
 * S1 adds lazy cross-module callee materialization and a single inlining round
 * on top of S0's new-PM plumbing. For each annotated tier-1 root the pass:
 *
 *   1. Reaches the FunctionAnalysisManager through the module->function proxy and
 *      builds the stock function-simplification pipeline (once).
 *   2. Simplifies the root, then scans it for direct managed leaf-call sites.
 *   3. Per candidate: checks cheap eligibility, MATERIALIZES the callee (runs its
 *      front-end and translates its body into THIS module as an internal
 *      function - inliner-support.hpp / translator.cpp), checks body-level
 *      eligibility, per-function-simplifies the body, checks a size cap, then
 *      either InlineFunctions it or reverts the call site back to its trampoline
 *      declaration.
 *   4. Strips every materialized body that ended up unused, so nothing is emitted
 *      standalone and the stock CGSCC inliner has no leftover to act on.
 *   5. Verifies the module (a bug in the hand-rolled inline aborts here rather
 *      than miscompiling).
 *
 * This is ONE round, not a fixpoint: no cross-round budget, no priority queue, no
 * re-optimize-and-rescan loop (those are S2). Because S1 only inlines LEAF
 * callees, an inlined body exposes no new managed calls, so the in-round rescan
 * of InlinedCallSites is present (for S2) but naturally finds nothing.
 *
 * Eligibility at this slice is deliberately narrow - only trivially-safe callees:
 *   - direct calls (getCalledFunction non-null) to a managed method (the callee
 *     declaration symbol maps back to a MonoMethod);
 *   - NOT self-recursive (the root's own body is a definition, not a trampoline
 *     declaration, so it is skipped by the "callee is a declaration" test);
 *   - NO rgctx/mrgctx argument (the `nest`-attributed arg) - gate #26, the
 *     generic-context silent-miscompile trap: a separately-translated callee body
 *     has no independent frame slot for a recovered generic context;
 *   - clause-free (no personality function) - gate #8, the wrong-EH-handler
 *     silent-miscompile trap: the custom-emit EH clause_index join key is not
 *     namespaced per inlined callee;
 *   - not `noinline` (covers clause-bearing/user-NoInlining/reflection-frame
 *     callees, which the translator already tags);
 *   - leaf (the body makes no non-intrinsic calls);
 *   - and the invoke / musttail guards kept from S0.
 * The rgctx and EH exclusions are hard safety gates; getting them wrong is a
 * silent miscompile, so they are asserted by the differential fixtures, not
 * eyeballed. materialize_callee () additionally refuses wrappers, synchronized
 * methods and gshared/gsharedvt bodies mono-side.
 */

#include "inliner.hpp"
#include "inliner-support.hpp"

#include <llvm/ADT/DenseMap.h>
#include <llvm/ADT/DenseSet.h>
#include <llvm/ADT/SmallPtrSet.h>
#include <llvm/ADT/SmallVector.h>
#include <llvm/IR/Attributes.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/IR/InstrTypes.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/PassManager.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Transforms/Utils/Cloning.h>

#include <cstdlib>

using namespace llvm;

namespace mono {

/*
 * Optional tracing of every inline decision, gated on MONO_INLINER_TRACE. The
 * differential fixtures assert on this: e.g. that a gshared rgctx-passing callee
 * or a clause-bearing callee is REFUSED (never INLINEd), which is otherwise a
 * silent miscompile to eyeball.
 */
static bool
trace_enabled ()
{
	static int state = -1;
	if (state < 0)
		state = getenv ("MONO_INLINER_TRACE") != nullptr;
	return state != 0;
}

static void
trace (const char *what, StringRef sym)
{
	if (trace_enabled ())
		errs () << "[inliner] " << what << " " << sym << "\n";
}

/*
 * A single-round S1 cap on the size of an individual materialized callee, in
 * post-simplification IR instructions. This is a plain sanity brake, NOT the
 * flat cross-round budget (that is S2): it just keeps one pathologically large
 * leaf out of the root. Tuned properly in S4.
 */
static const unsigned kInlineInstrCap = 400;

/*
 * True if CB carries a generic-context (rgctx/mrgctx or imt) argument. Those
 * travel in the `nest`-attributed parameter (see translator-call.cpp), so a
 * single attribute check is the whole gate - #26. Refusing these is a hard
 * correctness requirement: a folded callee body has no independent frame slot
 * from which the runtime could recover its generic context.
 */
static bool
passes_generic_context (const CallBase &cb)
{
	for (unsigned i = 0, n = cb.arg_size (); i < n; ++i)
		if (cb.paramHasAttr (i, Attribute::Nest))
			return true;
	return false;
}

/*
 * Resolve a call site to its managed callee for the cheap, pre-materialization
 * eligibility gates. Returns the target MonoMethod (opaque) and the callee
 * declaration, or {null, null} if CB is not an S1 candidate.
 */
static void *
candidate_target (CallBase &cb, Function *&decl_out)
{
	decl_out = nullptr;

	/* S0 guards: invokes carry EH edges, musttail a guaranteed tail position. */
	if (isa<InvokeInst> (&cb))
		return nullptr;
	if (cb.isMustTailCall ())
		return nullptr;

	/* Direct call only (#21): indirect/virtual targets need devirt, out of scope. */
	Function *callee = cb.getCalledFunction ();
	if (!callee)
		return nullptr;

	/*
	 * A managed callee is a body-less trampoline declaration in this module. The
	 * self-recursive call targets the defined root itself, so requiring a
	 * declaration also excludes self-recursion (and any already-materialized
	 * body) for free.
	 */
	if (!callee->isDeclaration ())
		return nullptr;

	/* #26: never inline through a generic-context-passing call. */
	if (passes_generic_context (cb)) {
		trace ("refuse-rgctx", callee->getName ());
		return nullptr;
	}

	/* Managed method symbol? (Filters icalls, intrinsics, runtime helpers.) */
	void *method = managed_method_from_symbol (callee->getName ().data ());
	if (!method)
		return nullptr;

	decl_out = callee;
	return method;
}

/*
 * A leaf body makes no non-intrinsic calls. Anything else - a managed call, a
 * runtime-helper/icall call, a GC safepoint poll, an indirect call, an invoke -
 * disqualifies it at S1 (inlining a body with its own calls, safepoints or EH is
 * later slices' work).
 */
static bool
is_leaf_body (Function &f)
{
	for (Instruction &i : instructions (f)) {
		auto *cb = dyn_cast<CallBase> (&i);
		if (!cb)
			continue;
		if (auto *ci = dyn_cast<CallInst> (cb))
			if (ci->getIntrinsicID () != Intrinsic::not_intrinsic)
				continue;
		return false;
	}
	return true;
}

/*
 * Revert a materialized call site back to its trampoline declaration. Used when
 * a candidate is materialized but then declined (body ineligible, over cap, type
 * mismatch) or InlineFunction fails: the direct edge to the materialized body is
 * dropped so the body becomes unreferenced and strip_dead_materialized_bodies ()
 * can erase it, leaving the ordinary `call @trampoline` behind.
 */
static void
revert_to_trampoline (CallBase &cb, Function *trampoline_decl)
{
	cb.setCalledFunction (trampoline_decl);
}

/*
 * Erase every materialized body that ended up with no uses - the declines and
 * reverts above, plus any body that was never wired to a surviving call. This
 * runs before the pass returns so the module leaves the driver in exactly the
 * shape the stock CGSCC inliner treats as inert: every surviving call is again a
 * trampoline declaration with no co-present body.
 */
static void
strip_dead_materialized_bodies (const SmallVectorImpl<Function *> &bodies)
{
	for (Function *f : bodies) {
		if (f->use_empty ())
			f->eraseFromParent ();
	}
}

/*
 * mono emits every method into its own LLVM module but keeps all intrinsic
 * declarations (and the EH helper stubs) in one shared "jit-global-module", so a
 * method that calls an intrinsic references a Function that lives in a DIFFERENT
 * module. That is codegen-safe (intrinsics lower by ID, the helpers resolve by
 * name) but the strict verifier rejects it as "Referencing function in another
 * module!". It is pre-existing and pervasive - it has nothing to do with
 * inlining - but our post-inline VerifierPass would trip on it.
 *
 * Make the module self-contained before verifying: for every cross-module
 * declaration referenced from m, add a local declaration of the same name/type
 * and redirect m's uses to it. This is strictly more correct (a self-contained
 * module) and codegen-identical, so it is safe to leave in the module that goes
 * on to codegen.
 */
static void
localize_foreign_declarations (Module &m)
{
	SmallVector<Function *, 16> foreign;
	SmallPtrSet<Function *, 16> seen;

	auto note = [&] (Value *v) {
		auto *f = dyn_cast_or_null<Function> (v);
		if (f && f->getParent () != &m && seen.insert (f).second)
			foreign.push_back (f);
	};

	for (Function &f : m) {
		if (f.hasPersonalityFn ())
			note (f.getPersonalityFn ());
		for (Instruction &i : instructions (f))
			for (Value *op : i.operands ())
				note (op);
	}

	for (Function *f : foreign) {
		FunctionCallee local =
			m.getOrInsertFunction (f->getName (), f->getFunctionType (),
			                       f->getAttributes ());
		auto *local_fn = cast<Function> (local.getCallee ());
		f->replaceUsesWithIf (local_fn, [&] (Use &u) {
			User *user = u.getUser ();
			if (auto *ins = dyn_cast<Instruction> (user))
				return ins->getFunction ()->getParent () == &m;
			if (auto *gv = dyn_cast<GlobalValue> (user))
				return gv->getParent () == &m;
			return false;
		});
	}
}

PreservedAnalyses
MonoTopDownInlinerPass::run (Module &m, ModuleAnalysisManager &mam)
{
	/* Enumerate annotated roots (v1: exactly one). */
	SmallVector<Function *, 1> roots;
	for (Function &f : m) {
		if (f.isDeclaration ())
			continue;
		if (f.hasFnAttribute ("mono-tier1-root"))
			roots.push_back (&f);
	}
	if (roots.empty ())
		return PreservedAnalyses::all ();

	FunctionAnalysisManager &fam =
		mam.getResult<FunctionAnalysisManagerModuleProxy> (m).getManager ();

	FunctionPassManager fpm =
		pb_->buildFunctionSimplificationPipeline (level_,
		                                          ThinOrFullLTOPhase::None);

	bool changed = false;
	/* Every materialized body, so unused ones can be stripped at the end. */
	SmallVector<Function *, 8> materialized_bodies;

	for (Function *root : roots) {
		void *root_cfg = tier1_root_cfg (root);
		/* Caller-level gates #1/#19/#2 (and: no cfg => nothing to materialize with). */
		if (!root_cfg || !tier1_root_allows_inlining (root_cfg))
			continue;

		/*
		 * Simplify the root once so eligibility and costs see canonical IR (the
		 * same order the stock CGSCC adaptor uses: simplify, then inline).
		 */
		fpm.run (*root, fam);

		/* One materialized body per callee method, reused across its call sites. */
		DenseMap<void *, Function *> cache;
		/* Bodies already run through the per-function simplification pipeline. */
		DenseSet<Function *> simplified;

		/* Seed the worklist from the root's current direct managed leaf calls. */
		SmallVector<CallBase *, 16> worklist;
		for (Instruction &i : instructions (*root)) {
			if (auto *cb = dyn_cast<CallBase> (&i)) {
				Function *decl;
				if (candidate_target (*cb, decl))
					worklist.push_back (cb);
			}
		}

		while (!worklist.empty ()) {
			CallBase *cs = worklist.pop_back_val ();

			Function *decl;
			void *target = candidate_target (*cs, decl);
			if (!target)
				continue;

			/* Materialize (cached): pull the callee body into this module. */
			Function *body;
			auto found = cache.find (target);
			if (found != cache.end ()) {
				body = found->second;
			} else {
				body = materialize_callee (target, root_cfg, &m);
				cache [target] = body;              /* cache misses too (as null) */
				if (body)
					materialized_bodies.push_back (body);
			}
			if (!body)
				continue;                            /* unmaterializable -> leave trampoline */

			/* Body-level eligibility: #8 (EH clauses) and the noinline family. */
			if (body->hasPersonalityFn ()) {
				trace ("refuse-eh", body->getName ());
				continue;
			}
			if (body->hasFnAttribute (Attribute::NoInline)) {
				trace ("refuse-noinline", body->getName ());
				continue;
			}
			if (!is_leaf_body (*body)) {
				trace ("refuse-nonleaf", body->getName ());
				continue;
			}

			/* Per-function simplify the body once, then measure its cost. */
			if (simplified.insert (body).second)
				fpm.run (*body, fam);

			if (body->getInstructionCount () > kInlineInstrCap) {
				trace ("refuse-oversize", body->getName ());
				continue;
			}

			/*
			 * The call's stored function type must match the body's, or the
			 * inlined arguments would not line up. They derive from the same
			 * MonoMethodSignature so they normally match; if they somehow do not,
			 * decline rather than force a bitcast.
			 */
			if (cs->getFunctionType () != body->getFunctionType ())
				continue;

			/* Wire the call to the materialized body and inline it. */
			cs->setCalledFunction (body);

			StringRef body_name = body->getName ();
			InlineFunctionInfo ifi;
			InlineResult res = InlineFunction (*cs, ifi);
			if (!res.isSuccess ()) {
				trace ("revert", body_name);
				revert_to_trampoline (*cs, decl);
				continue;
			}

			trace ("inline", body_name);
			changed = true;

			/*
			 * InlineFunction preserves nothing, so drop the root's cached
			 * analyses before anything else consults them.
			 */
			fam.invalidate (*root, PreservedAnalyses::none ());

			/*
			 * Re-scan the newly exposed call sites into the same round. For a
			 * leaf callee this is empty; the machinery is here for S2.
			 */
			for (CallBase *ncs : ifi.InlinedCallSites) {
				Function *ndecl;
				if (candidate_target (*ncs, ndecl))
					worklist.push_back (ncs);
			}
		}
	}

	/* Reject/never-inlined bodies are unreferenced now: erase them. */
	strip_dead_materialized_bodies (materialized_bodies);

	/*
	 * optimize () builds its PassBuilder without verifier instrumentation, so a
	 * malformed-IR bug in an inline would surface as a JIT crash or wrong result.
	 * Verify here so it aborts at the pass boundary instead. Fatal by default.
	 * Localize mono's pre-existing cross-module intrinsic/helper references first,
	 * so the verifier judges the inline, not that (benign, codegen-safe) pattern.
	 */
	if (changed) {
		localize_foreign_declarations (m);
		VerifierPass ().run (m, mam);
	}

	return changed ? PreservedAnalyses::none () : PreservedAnalyses::all ();
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

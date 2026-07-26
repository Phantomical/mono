/**
 * \file
 * inliner.cpp - mono's tier-1 LLVM inliner.
 *
 * mono translates one method per LLVM module, so every managed call in a
 * tier-1 root is a direct call to a bodyless trampoline declaration - there is
 * nothing in the module for an inliner to fold in. This pass supplies the
 * missing bodies and then lets LLVM's own bottom-up inliner make every actual
 * inlining decision. Per root, per round:
 *
 *   1. Walk the root two call levels deep and materialize each callee that
 *      clears the hard eligibility gates below: run its front-end, translate
 *      its body into THIS module as an internal function (inliner-support.hpp,
 *      implemented in translator.cpp), and rewire its call sites to call that
 *      body directly.
 *   2. Run the stock function-simplification pipeline over the bodies that
 *      round newly translated, so the inliner's cost model sees canonical IR.
 *   3. Run LLVM's stock inliner pipeline over the module - buildInlinerPipeline
 *      (), cost model and CGSCC ordering and all.
 *   4. If it inlined anything into the root, go round again: a call that was
 *      three levels down is now one level down, so the next walk reaches
 *      deeper than this one could.
 *   5. Otherwise stop, and revert every materialized body the inliner did not
 *      consume back to its trampoline call, so none is ever emitted standalone.
 *
 * The loop is capped at kMaxInlinerRounds, but normally exits well before that
 * - as soon as a round folds nothing new into the root.
 *
 * Note that this pass occupies the stock inliner's slot in the -O2 pipeline
 * (build_tier1_pipeline () at the bottom), and that slot carries the whole
 * per-function simplification pipeline nested inside it. So the pass has to run
 * buildInlinerPipeline () at least once even when it finds no root to work on,
 * or -O2 silently loses its function simplification stage.
 *
 * Eligibility is decided entirely at wiring time, because once a call site
 * names a materialized body the stock inliner is free to fold it in with no
 * further say from us. A callee that fails any gate simply never gets
 * materialized: its call site stays a trampoline declaration, which is what
 * structurally keeps it out of the stock inliner's reach. The gates:
 *   - direct calls only (getCalledFunction non-null), to a managed method (the
 *     callee declaration symbol maps back to a MonoMethod);
 *   - NOT self-recursive: the root's own body is a definition, not a
 *     declaration, so it is skipped by the "callee is a declaration" test;
 *   - NO rgctx/mrgctx argument (the `nest`-attributed arg) - gate #26, the
 *     generic-context silent-miscompile trap: a separately-translated callee
 *     body has no independent frame slot for a recovered generic context;
 *   - clause-free (no personality function) - gate #8, the wrong-EH-handler
 *     silent-miscompile trap: the custom-emit EH clause_index join key is not
 *     namespaced per inlined callee;
 *   - not `noinline` (covers clause-bearing/user-NoInlining/reflection-frame
 *     callees, which the translator already tags);
 *   - does not read/write class-init-guarded static state - a callee whose body
 *     touches a static field of a class with a non-trivial cctor. Inlining
 *     drops the class-init barrier that its own managed call carried, so the
 *     static read can see a value from before the cctor ran (a silent
 *     miscompile). The barrier is often elided in the materialized body
 *     (beforefieldinit / same-class access), so this is decided from the
 *     callee's metadata rather than from anything visible in the IR;
 *   - does not still owe its own class's cctor - the call itself is that
 *     class's init trigger (the first one goes through a trampoline, which
 *     compiles the method and then runs the cctor), so folding the callee in
 *     deletes the trigger and the class stays uninitialized for the life of
 *     the process. The mirror image of the gate above: that one is about
 *     static state the callee reads, this one about the initialization the
 *     call itself performs;
 *   - does not ask the runtime about its own frame - GetCurrentMethod,
 *     GetCallingAssembly, StackTrace's frame 0. Folding such a callee in makes
 *     those report the caller's frame instead (gate #25);
 *   - and the invoke / musttail guards.
 * The rgctx and EH exclusions are hard safety gates; getting them wrong is a
 * silent miscompile, so they are asserted by the differential fixtures in
 * inliner-tests.cs, not eyeballed. materialize_callee () additionally refuses
 * wrappers, synchronized methods and gshared/gsharedvt bodies mono-side.
 *
 * There is deliberately no size or leaf-only restriction here: bounding what is
 * worth folding in is the stock inliner's cost model's job, and starving it of
 * non-leaf callees would defeat the point of handing the decision over.
 */

#include "inliner.hpp"
#include "inliner-support.hpp"

#include <llvm/ADT/Any.h>
#include <llvm/ADT/DenseSet.h>
#include <llvm/ADT/STLExtras.h>
#include <llvm/ADT/SmallPtrSet.h>
#include <llvm/ADT/SmallVector.h>
#include <llvm/ADT/StringMap.h>
#include <llvm/ADT/StringRef.h>
#include <llvm/IR/Attributes.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/IR/InstrTypes.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/PassInstrumentation.h>
#include <llvm/IR/PassManager.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/Support/raw_ostream.h>

#include <algorithm>
#include <cassert>
#include <cstdlib>
#include <memory>
#include <string>
#include <utility>

using namespace llvm;

namespace mono {

namespace {

/*
 * Ceiling on how many materialize/inline rounds one root gets, purely to bound
 * compile time. The loop normally stops long before this, the moment a round
 * folds nothing new into the root.
 */
const unsigned kMaxInlinerRounds = 10;

/*
 * How many call levels below the root one round exposes. Deeper callees are
 * left as trampoline calls; they come into reach on a later round, once the
 * stock inliner has collapsed a level above them. This is what bounds how much
 * front-end work a single round can trigger.
 */
const unsigned kMaterializeDepth = 2;

/* The translator marks the method being promoted with this (translator.cpp). */
const char kRootAttr[] = "mono-tier1-root";

/*
 * Marks a function this pass translated into the module, with the symbol of the
 * trampoline declaration it stands in for as the attribute's value. The module
 * is then the only bookkeeping we need: the stock inliner erases materialized
 * bodies it fully consumes, so any pointer table we kept on the side would go
 * stale mid-round.
 */
const char kMaterializedAttr[] = "mono-materialized";

/*
 * Optional tracing of every materialization decision, gated on
 * MONO_INLINER_TRACE. Worth having because the interesting failures here are
 * quiet ones: a gshared rgctx-passing callee or a clause-bearing callee that
 * gets exposed when it should have been refused is a silent miscompile, and a
 * root that stands down entirely looks exactly like a corpus with no candidates
 * in it unless the pass says why.
 */
bool
trace_enabled ()
{
	static int state = -1;
	if (state < 0)
		state = getenv ("MONO_INLINER_TRACE") != nullptr;
	return state != 0;
}

void
trace (const char *what, StringRef sym)
{
	if (trace_enabled ())
		errs () << "[inliner] " << what << " " << sym << "\n";
}

/*
 * Shared between the pass and the instrumentation callbacks registered
 * alongside it. The pass object gets copied into the pipeline's type-erased
 * pass model, so this lives behind a shared_ptr rather than in the pass itself.
 */
struct RoundState {
	/* Set while a round's stock-inliner sub-pipeline is running. */
	bool recording = false;
	/* Set while the stock InlinerPass itself is on the stack. */
	bool in_inliner = false;
	/* Functions the stock inliner inlined something into this round. */
	SmallPtrSet<const Function *, 8> inlined_into;
};

/*
 * The tier-1 inlining stage. See the file comment for the algorithm; this is
 * the module pass that build_tier1_pipeline () splices into the -O2 pipeline
 * where LLVM's own inlining stage would otherwise sit.
 */
class MonoInlinerPass : public PassInfoMixin<MonoInlinerPass> {
public:
	MonoInlinerPass (PassBuilder &pb, OptimizationLevel level,
	                 std::shared_ptr<RoundState> state)
	    : pb_ (&pb), level_ (level), state_ (std::move (state))
	{
	}

	PreservedAnalyses run (Module &m, ModuleAnalysisManager &mam);

	/*
	 * This pass carries the per-function simplification pipeline that the stock
	 * inlining stage it replaced used to carry, so skipping it would quietly
	 * turn -O2 into something much weaker.
	 */
	static bool isRequired () { return true; }

private:
	void expose_callees (Module &m, Function &root, void *root_cfg,
	                     DenseSet<void *> &refused,
	                     SmallVectorImpl<Function *> &added);
	void run_stock_inliner (Module &m, ModuleAnalysisManager &mam,
	                        bool module_mutated);

	/*
	 * Held by pointer, not reference: a reference member would delete the
	 * class's implicit copy/move-assignment and has non-obvious
	 * lifetime/rebinding behaviour. The PassBuilder outlives the pass (it owns
	 * the pipeline the pass is spliced into).
	 */
	PassBuilder *pb_;
	OptimizationLevel level_;
	std::shared_ptr<RoundState> state_;
};

/*
 * Walk ROOT kMaterializeDepth call levels deep, materializing every callee that
 * clears the gates and pointing its call sites at the materialized body - which
 * is what puts the call within the stock inliner's reach. Bodies translated for
 * the first time on this walk land in ADDED (they still need simplifying);
 * REFUSED accumulates targets that failed a gate, so a later round does not pay
 * for them again.
 */
void
MonoInlinerPass::expose_callees (Module &m, Function &root, void *root_cfg,
                                 DenseSet<void *> &refused,
                                 SmallVectorImpl<Function *> &added)
{
	StringMap<Function *> by_symbol = index_materialized (m);
	SmallPtrSet<Function *, 16> scanned;

	SmallVector<Function *, 8> level;
	level.push_back (&root);
	scanned.insert (&root);

	for (unsigned depth = 0; depth < kMaterializeDepth && !level.empty (); ++depth) {
		SmallVector<Function *, 8> next;

		for (Function *caller : level) {
			for (Instruction &i : instructions (*caller)) {
				auto *cb = dyn_cast<CallBase> (&i);
				if (!cb)
					continue;

				/*
				 * Already pointed at a body an earlier round materialized:
				 * nothing to wire, but descend into it so the walk keeps
				 * measuring depth from the root through materialized bodies.
				 */
				Function *called = cb->getCalledFunction ();
				if (called && called->hasFnAttribute (kMaterializedAttr)) {
					if (scanned.insert (called).second)
						next.push_back (called);
					continue;
				}

				Function *decl;
				void *target = candidate_target (*cb, decl);
				if (!target || refused.contains (target))
					continue;

				Function *body = nullptr;
				auto found = by_symbol.find (decl->getName ());
				if (found != by_symbol.end ()) {
					body = found->second;
				} else {
					if (!callee_gates_pass (target, root_cfg, decl->getName ())) {
						refused.insert (target);
						continue;
					}

					body = materialize_callee (target, root_cfg, &m);
					if (!body) {
						trace ("refuse-materialize", decl->getName ());
						refused.insert (target);
						continue;
					}

					if (!body_gates_pass (*body)) {
						body->eraseFromParent ();
						refused.insert (target);
						continue;
					}

					body->addFnAttr (kMaterializedAttr, decl->getName ());
					by_symbol [decl->getName ()] = body;
					added.push_back (body);
					trace ("materialize", body->getName ());
				}

				/*
				 * The call's stored function type must match the body's, or the
				 * arguments would not line up once it is folded in. They derive
				 * from the same MonoMethodSignature so they normally match; if
				 * they somehow do not, leave the trampoline call alone rather
				 * than force a bitcast.
				 */
				if (cb->getFunctionType () != body->getFunctionType ())
					continue;

				cb->setCalledFunction (body);
				trace ("expose", body->getName ());

				if (scanned.insert (body).second)
					next.push_back (body);
			}
		}

		level = std::move (next);
	}
}

/*
 * Run LLVM's stock inlining stage over the whole module. A fresh wrapper every
 * time: ModuleInlinerWrapperPass::run () appends the CGSCC adaptor to its own
 * nested pipeline as it runs, so the object is single-use.
 */
void
MonoInlinerPass::run_stock_inliner (Module &m, ModuleAnalysisManager &mam,
                                    bool module_mutated)
{
	/*
	 * Materializing a body adds a function to the module behind the analysis
	 * managers' backs, and the stock inliner runs off a cached LazyCallGraph
	 * that would not know about it. Drop everything cached about the module -
	 * and, through the function-analysis proxy, everything cached about its
	 * functions - so the call graph it walks is the module we actually have.
	 * (It keeps its own call graph honest across the inlines it performs, so
	 * the previous round's run is not what makes this necessary.)
	 */
	if (module_mutated)
		mam.invalidate (m, PreservedAnalyses::none ());

	ModulePassManager mpm;
	mpm.addPass (pb_->buildInlinerPipeline (level_, ThinOrFullLTOPhase::None));
	mpm.run (m, mam);
}

PreservedAnalyses
MonoInlinerPass::run (Module &m, ModuleAnalysisManager &mam)
{
	FunctionAnalysisManager &fam =
		mam.getResult<FunctionAnalysisManagerModuleProxy> (m).getManager ();

	FunctionPassManager fpm =
		pb_->buildFunctionSimplificationPipeline (level_,
		                                          ThinOrFullLTOPhase::None);

	/* Annotated roots (v1: exactly one - the method being promoted). */
	SmallVector<Function *, 1> roots;
	for (Function &f : m) {
		if (!f.isDeclaration () && f.hasFnAttribute (kRootAttr))
			roots.push_back (&f);
	}

	bool materialized_anything = false;
	bool ran_stock_inliner = false;

	for (Function *root : roots) {
		void *root_cfg = tier1_root_cfg (root);
		/* Caller-level gates #1/#19/#2 (and: no cfg => nothing to materialize with). */
		if (!root_cfg || !tier1_root_allows_inlining (root_cfg)) {
			trace (root_cfg ? tier1_root_refusal_reason (root_cfg)
			                : "refuse-root-nocfg",
			       root->getName ());
			continue;
		}

		/* Targets that failed a gate once and need not be reconsidered. */
		DenseSet<void *> refused;

		for (unsigned round = 0; round < kMaxInlinerRounds; ++round) {
			SmallVector<Function *, 8> added;
			expose_callees (m, *root, root_cfg, refused, added);

			/*
			 * Nothing new to offer since the last round, so the stock inliner
			 * would be looking at the module it just finished with.
			 */
			if (round > 0 && added.empty ())
				break;

			if (!added.empty ()) {
				materialized_anything = true;
				/*
				 * A fresh body can reference declarations that live in the
				 * shared jit-global module. That has always been tolerable for
				 * codegen, but the stock inliner builds a call graph over this
				 * module and cannot follow an edge that leaves it, so make the
				 * module self-contained first.
				 */
				localize_foreign_declarations (m);
			}

			/* Canonicalize the fresh bodies before the cost model sees them. */
			for (Function *f : added)
				fpm.run (*f, fam);

			state_->inlined_into.clear ();
			state_->recording = true;
			run_stock_inliner (m, mam, true);
			state_->recording = false;
			ran_stock_inliner = true;

			/* Another round only reaches deeper if the root itself grew. */
			if (!state_->inlined_into.contains (root))
				break;
		}
	}

	strip_materialized_bodies (m);

	/*
	 * We stand in for the stock inlining stage, and that stage is where -O2
	 * runs its per-function simplification pipeline. If no root drove a round
	 * above, run it once anyway so the rest of the pipeline sees the same IR it
	 * would have without us. Nothing touched the module in that case, so the
	 * analyses the pipeline has cached so far are still good.
	 */
	if (!ran_stock_inliner)
		run_stock_inliner (m, mam, false);

	/*
	 * optimize () builds its PassBuilder without verifier instrumentation, so a
	 * malformed-IR bug in a materialized body would surface as a JIT crash or a
	 * wrong result. Verify here so it aborts at the pass boundary instead. The
	 * cross-module references mono emits by default were already localized
	 * above, so the verifier judges what we did rather than that (benign,
	 * codegen-safe) pattern.
	 */
	if (materialized_anything)
		VerifierPass ().run (m, mam);

	return PreservedAnalyses::none ();
}

/*
 * Observe the stock inliner's own per-caller analysis invalidation to learn
 * which functions it inlined into. InlinerPass::run () calls FAM.invalidate (F,
 * none ()) once it has folded something into F and not otherwise, and that
 * invalidation is reported to instrumentation per Function - it is a real
 * signal, unlike the coarse module-scope PreservedAnalyses the stage returns.
 *
 * The in_inliner window is what makes it specific: plain simplification
 * invalidates function analyses too, and the stage runs the whole function
 * simplification pipeline alongside the inliner. InlinerPass runs no nested
 * passes, so between its before- and after-pass callbacks any function
 * invalidation is one of its own.
 */
void
register_round_callbacks (PassInstrumentationCallbacks &pic,
                          const std::shared_ptr<RoundState> &state)
{
	pic.registerBeforeNonSkippedPassCallback ([state] (StringRef id, Any) {
		if (state->recording && id == "InlinerPass")
			state->in_inliner = true;
	});
	pic.registerAfterPassCallback (
		[state] (StringRef id, Any, const PreservedAnalyses &) {
			if (id == "InlinerPass")
				state->in_inliner = false;
		});
	pic.registerAfterPassInvalidatedCallback (
		[state] (StringRef id, const PreservedAnalyses &) {
			if (id == "InlinerPass")
				state->in_inliner = false;
		});
	pic.registerAnalysisInvalidatedCallback ([state] (StringRef, Any ir) {
		if (!state->in_inliner)
			return;
		if (const Function *const *f = any_cast<const Function *> (&ir))
			state->inlined_into.insert (*f);
	});
}

/*
 * A built ModulePassManager is a flat vector of type-erased passes, but the
 * vector is protected - reachable by a subclass, and nothing else. This exists
 * only to reach it, so build_tier1_pipeline () can put its own pass exactly
 * where LLVM's inlining stage was rather than somewhere merely nearby.
 */
class PipelineSplicer : public ModulePassManager {
public:
	explicit PipelineSplicer (ModulePassManager &&built)
	    : ModulePassManager (std::move (built))
	{
	}

	/*
	 * Swap PASS in for the sole entry named NAME, keeping its position. Fails
	 * (changing nothing) unless exactly one entry matches - see the caller for
	 * why that is treated as a hard error rather than a fallback.
	 */
	template <typename PassT>
	bool replace_sole (StringRef name, PassT &&pass)
	{
		size_t at = 0;
		unsigned found = 0;
		for (size_t i = 0, n = Passes.size (); i < n; ++i) {
			if (Passes [i]->name () == name) {
				at = i;
				++found;
			}
		}
		if (found != 1)
			return false;

		/* addPass () appends, wrapping in the right pass model; move it home. */
		addPass (std::forward<PassT> (pass));
		Passes [at] = std::move (Passes.back ());
		Passes.pop_back ();
		return true;
	}

	template <typename PassT>
	void prepend (PassT &&pass)
	{
		addPass (std::forward<PassT> (pass));
		std::rotate (Passes.begin (), Passes.end () - 1, Passes.end ());
	}

	ModulePassManager take () { return std::move (*this); }
};

/*
 * What buildInlinerPipeline () contributes to the default pipeline, by name.
 * It returns a ModuleInlinerWrapperPass rather than a ModulePassManager, so -
 * unlike every other stage, which gets flattened into the enclosing manager by
 * addPass ()'s same-type overload - it lands as one opaque, unambiguously named
 * entry. That is what makes swapping it out a single lookup.
 */
const char kStockInlinerPass[] = "ModuleInlinerWrapperPass";

} // namespace

ModulePassManager
build_tier1_pipeline (PassBuilder &pb, PassInstrumentationCallbacks &pic,
                      OptimizationLevel level)
{
	auto state = std::make_shared<RoundState> ();
	register_round_callbacks (pic, state);

	PipelineSplicer pipeline (pb.buildPerModuleDefaultPipeline (level));

	if (!pipeline.replace_sole (kStockInlinerPass,
	                            MonoInlinerPass (pb, level, state))) {
		/*
		 * The stage we replace is not where we expect it: an LLVM upgrade
		 * renamed it, or -enable-module-inliner swapped in the other one. Say
		 * so loudly - the quiet failure is far worse, since LLVM's inliner
		 * would then run over our materialized bodies with its own budget while
		 * everything still appears to work.
		 */
		errs () << "mono: no unique " << kStockInlinerPass
		        << " in the -O2 pipeline; running the tier-1 inliner at the "
		           "pipeline start instead\n";
		assert (false && "tier-1 inliner could not take the stock inliner's slot");
		pipeline.prepend (MonoInlinerPass (pb, level, state));
	}

	return pipeline.take ();
}

} // namespace mono

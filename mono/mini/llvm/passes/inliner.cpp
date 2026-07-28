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
 *      body directly. A callee the walk has already materialized is rewired
 *      again rather than passed over, because a body translated since then
 *      arrived with call sites of its own that the first rewiring could not
 *      have seen.
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
 * Steps 1 and 5 are plain code rather than passes, so they announce themselves
 * to pass instrumentation by hand (report_stage () below) - otherwise a
 * MONO_LLVM_DUMP_PASS_IR dump would show the stock inliner's passes with the
 * materialized bodies appearing and vanishing between them for no visible
 * reason.
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
 *   - not `noinline` (covers user-NoInlining and reflection-frame callees,
 *     which the translator already tags);
 *   - does not ask the runtime about its own frame - GetCurrentMethod,
 *     GetCallingAssembly, StackTrace's frame 0. Folding such a callee in makes
 *     those report the caller's frame instead (gate #25);
 *   - and, for a callee that carries EH clauses of its own, only call sites that
 *     are not already inside one of the CALLER's try regions - i.e. plain calls,
 *     not invokes (replace_eligible_uses ()). This one is per SITE rather than
 *     per callee: the body is materialized once and folded in wherever it is
 *     safe, and the sites left out keep calling the trampoline.
 * The EH rule is a hard safety gate; getting it wrong is a silent miscompile,
 * so it is asserted by the differential fixtures in inliner-tests.cs, not
 * eyeballed. materialize_callee () additionally refuses wrappers, synchronized
 * methods, open/shared-generic callees and anything that comes back gsharedvt
 * mono-side.
 *
 * Generics come in two shapes. A callee whose instantiation is closed is
 * materialized as that exact instantiation rather than as the shared body
 * (materialize_callee ()), so a call site that passes a vtable/mrgctx in `nest`
 * is eligible like any other: the folded body is specialized, has no generic
 * context to recover, and ignores the argument. A callee shared over exactly the
 * type parameters the ROOT is shared over is materialized shared instead - its
 * runtime generic context is the root's, which the call site already passes, and
 * which a stack walk over the root's frame recovers for the folded-in clauses
 * too.
 *
 * There is deliberately no size or leaf-only restriction here: bounding what is
 * worth folding in is the stock inliner's cost model's job, and starving it of
 * non-leaf callees would defeat the point of handing the decision over.
 */

#include "inliner.hpp"
#include "inliner-support.hpp"
#include "devirt.hpp"

#include <llvm/ADT/Any.h>
#include <llvm/ADT/DenseSet.h>
#include <llvm/ADT/STLExtras.h>
#include <llvm/ADT/SmallPtrSet.h>
#include <llvm/ADT/SmallVector.h>
#include <llvm/ADT/StringMap.h>
#include <llvm/ADT/StringRef.h>
#include <llvm/ADT/Twine.h>
#include <llvm/IR/Attributes.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/IR/InstrTypes.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/PassInstrumentation.h>
#include <llvm/IR/PassManager.h>
#include <llvm/IR/ValueHandle.h>
#include <llvm/IR/ValueMap.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/Support/raw_ostream.h>

#include <algorithm>
#include <cassert>
#include <cstdlib>
#include <memory>
#include <set>
#include <string>
#include <utility>

using namespace llvm;

namespace mono {

struct RoundState {
	/* Set while a round's stock-inliner sub-pipeline is running. */
	bool recording = false;
	/* Set while the stock InlinerPass itself is on the stack. */
	bool in_inliner = false;
	/* Functions the stock inliner inlined something into this round. */
	SmallPtrSet<const Function *, 8> inlined_into;
};

namespace {

/* The translator marks the method being promoted with this (translator.cpp). */

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
 * quiet ones: a gshared rgctx-passing callee, or a clause-bearing callee folded
 * into a try region it has no selector cases for, is a silent miscompile, and a
 * root that stands down entirely looks exactly like a corpus with no candidates
 * in it unless the pass says why.
 */
bool
trace_enabled ()
{
	/* Read once for the process; concurrent tier-1 compiles all pass through here. */
	static const bool enabled = getenv ("MONO_INLINER_TRACE") != nullptr;
	return enabled;
}

void
trace (const char *what, StringRef sym)
{
	if (trace_enabled ())
		errs () << "[inliner] " << what << " " << sym << "\n";
}

/*
 * Same, naming METHOD the way the rest of the trace names methods. A refusal has
 * the MonoMethod to hand where the LLVM declaration only has a mangled symbol,
 * and a trace whose lines are half mangled and half not cannot be matched
 * against one set of expectations - see check-inliner-tags.sh.
 */
void
trace_method (const char *what, MonoMethod *method)
{
	if (!trace_enabled ())
		return;

	char *name = mono_method_full_name (method, TRUE);

	errs () << "[inliner] " << what << " " << name << "\n";
	g_free (name);
}

/*
 * Report a stage of this pass to LLVM's pass instrumentation as if it were a
 * pass in its own right. The round loop's materialize and strip steps are plain
 * code rather than passes, so nothing would otherwise report them - and they are
 * precisely the steps that put callee bodies into the module and take the
 * unconsumed ones back out, which is what someone reading a
 * MONO_LLVM_DUMP_PASS_IR dump is usually after.
 *
 * runAfterPass () only ever asks the pass for its name, and it asks the object
 * rather than the type, so a stand-in with a per-instance name is enough - and
 * that name can carry the round number, which a real pass's static name ()
 * could not.
 */
void
report_stage (const PassInstrumentation &pi, Module &module, const Twine &stage)
{
	struct StageMarker {
		std::string text;
		StringRef name () const { return text; }
	} marker { ("mono::MonoInlinerPass " + stage).str () };

	pi.runAfterPass (marker, module, PreservedAnalyses::none ());
}

class MonoInlinerState {
private:
	llvm::Module *module;
	std::shared_ptr<RoundState> round_state;
	Tier1Root root;

	llvm::SmallVector<llvm::Function *, 8> roots;

	// Functions that are known to be statically ineligible for inlining.
	llvm::DenseSet<llvm::Function *> ineligible;

	// A map of imported body back to its original function declaration.
	llvm::DenseMap<llvm::Function *, llvm::Function *> definitions;
	/*
	 * The other direction, declaration -> body. Held by weak handle because the
	 * stock inliner erases a body once it has consumed its last call site: the
	 * entry then reads back as null, and the declaration is free to be
	 * materialized again for whatever call sites a later round turns up.
	 */
	llvm::DenseMap<llvm::Function *, llvm::WeakTrackingVH> declarations;

	/*
	 * What is the lowest depth that this function has been processed at, if any.
	 * Keyed by value handle: a body the stock inliner erased must take its entry
	 * with it, or a body materialized later at the same address would inherit the
	 * dead one's depth and never be walked.
	 */
	llvm::ValueMap<llvm::Function *, unsigned> depth_cache;

	llvm::SmallVector<llvm::Function *, 8> added;

	/*
	 * Names of the bodies this pass put into the module, so strip time can say
	 * which of them the stock inliner actually consumed - a body it fully folded
	 * in is either use-free by then or already erased, and one it passed on still
	 * has its call sites. Only read when tracing. std::set rather than a hash so
	 * the report comes out in a stable order.
	 */
	std::set<std::string> materialized;

public:
	MonoInlinerState (llvm::Module *module, const std::shared_ptr<RoundState> &round_state,
	                  const Tier1Root &root)
	    : module (module), round_state (round_state), root (root)
	{
		/*
		 * No root to work on: a module the engine was handed directly rather
		 * than through a tier-1 compile (the unit tests do this), or a root
		 * whose body an earlier stage already deleted. Either way there is
		 * nothing to materialize into, and the pass still has to run the
		 * nested simplification pipeline - see the file comment.
		 */
		if (!root.func || root.func->isDeclaration ())
			return;

		if (!root.cfg || !root.module) {
			// Nothing to materialize with - the cfg drives every callee
			// compile, and the module names the context they must be built in.
			trace ("refuse-root-nocfg", root.func->getName ());
			return;
		}

		// -O=-inline, or a method the user asked not to have optimized.
		// Honouring this here is what keeps a whole-run "inlined nothing"
		// from being indistinguishable from "found nothing to inline".
		if (!tier1_root_allows_inlining (root.cfg)) {
			trace (tier1_root_refusal_reason (root.cfg), root.func->getName ());
			return;
		}

		// Make sure we can tell which method is under consideration here.
		trace ("root", root.func->getName ());

		roots.push_back (root.func);
	}

	static const unsigned MaxInlinerRounds = 10;

	PreservedAnalyses run (llvm::ModuleAnalysisManager &mam, llvm::PassBuilder &pb,
	                       llvm::OptimizationLevel level)
	{
		// PassInstrumentationAnalysis is a required analysis and never
		// invalidated, so one fetch covers every round.
		const PassInstrumentation pi = mam.getResult<PassInstrumentationAnalysis> (*module);

		for (unsigned round = 0; round < MaxInlinerRounds; ++round) {
			/*
			 * Resolve what the previous round exposed before deciding whether
			 * this round has work. Inlining a callee brings its allocations
			 * into the root, which is what lets a receiver be proven, and each
			 * site this turns direct is a candidate import_candidates () can
			 * then pick up - so the two have to alternate, not run once each.
			 */
			unsigned devirted = devirtualize (*module, root);
			if (devirted)
				report_stage (pi, *module, "round " + Twine (round) + ": after devirt");

			// Nothing new was added, so there's nothing else to do.
			// We always need to run at least one round, though, because the
			if (!import_candidates () && !devirted && round != 0)
				break;

			report_stage (pi, *module, "round " + Twine (round) + ": after materialize");

			// Invalidate all module-level analyses, preserve function-level ones
			PreservedAnalyses pa;
			pa.preserve<llvm::FunctionAnalysisManagerModuleProxy> ();
			mam.invalidate (*module, pa);

			round_state->recording = true;
			round_state->inlined_into.clear ();
			// A fresh pipeline every round: ModuleInlinerWrapperPass::run ()
			// appends the CGSCC adaptor to its own nested pipeline as it runs, so
			// the object is single-use.
			ModulePassManager mpm;
			mpm.addPass (pb.buildInlinerPipeline (level, ThinOrFullLTOPhase::None));
			pa = mpm.run (*module, mam);
			round_state->recording = false;

			report_stage (pi, *module, "round " + Twine (round) + ": after inlining");

			// inliner did nothing, so nothing for us to do
			if (round_state->inlined_into.empty ())
				break;
		}

		strip_materialized_bodies ();
		report_stage (pi, *module, "after strip");
		return PreservedAnalyses::none ();
	}

private:
	// Walk through the module, starting at each root, and import any functions
	// that are eligible as inlining candidates.
	bool import_candidates ()
	{
		added.clear ();
		for (auto root : roots)
			depth_cache.erase (root);

		for (auto func : roots)
			process_candidate (func, 0, root.cfg);

		return !added.empty ();
	}

	void strip_materialized_bodies ()
	{
		for (auto &func : llvm::make_early_inc_range (*module)) {
			auto attr = func.getFnAttribute (kMaterializedAttr);
			if (!attr.isValid ())
				continue;

			if (!func.use_empty ()) {
				// Still called from somewhere, so the stock inliner passed on at
				// least one of its call sites and this body is about to go back to
				// being a trampoline call.
				trace ("unfolded", func.getName ());
				materialized.erase (func.getName ().str ());

				// definitions maps body -> declaration; declarations is the
				// other direction, and keyed by a body it yields null.
				auto decl = definitions.lookup (&func);
				g_assert (decl);
				func.replaceAllUsesWith (decl);
			}

			func.eraseFromParent ();
		}

		// Whatever is left was folded into every one of its callers: either it
		// reached here with no uses, or the stock inliner had already erased it
		// once it consumed the last one.
		for (const auto &name : materialized)
			trace ("folded", name);
		materialized.clear ();
	}

	static constexpr unsigned MaxInlineDepth = 2;
	static constexpr unsigned MaxAggressiveInlineDepth = 8;

	void process_candidate (llvm::Function *candidate, unsigned depth, MonoCompile *config)
	{
		llvm::Function *body = nullptr;

		if (candidate->isDeclaration ()) {
			/*
			 * Wiring is a snapshot: replace_eligible_uses () can only reach the
			 * call sites that exist at the moment it runs. Every body
			 * materialized after this one arrives carrying its own fresh calls
			 * to the same declaration, and this walk is the only thing that ever
			 * sees them - so an already-imported callee still has to have its
			 * new sites wired up, before either of the two stopping conditions
			 * below gets to cut the walk short. Skipping that is how a callee
			 * ends up reported folded while the root still calls its trampoline:
			 * every site the wiring did reach was consumed, and the ones it
			 * never reached were never candidates for the stock inliner at all.
			 */
			body = cast_or_null<Function> (declarations.lookup (candidate));
			if (body)
				replace_eligible_uses (candidate, body);
		}

		if (depth > 0) {
			auto it = depth_cache.find (candidate);
			if (it != depth_cache.end () && it->second <= depth) {
				// This function has already been processed at this depth, no
				// need to do it again.
				return;
			}

			depth_cache[candidate] = depth;
		}

		if (candidate->isDeclaration ()) {
			if (!body) {
				bool transient = false;
				body = materialize_candidate (candidate, depth, config, &transient);
				if (!body) {
					if (!transient)
						ineligible.insert (candidate);
					return;
				}
			}

			g_assert (!body->isDeclaration ());
			candidate = body;
		}

		if (depth == MaxAggressiveInlineDepth)
			return;

		for (auto &inst : llvm::instructions (*candidate)) {
			auto *cb = llvm::dyn_cast<llvm::CallBase> (&inst);
			if (!cb)
				continue;

			auto called = cb->getCalledFunction ();
			if (!called)
				continue;

			process_candidate (called, depth + 1, config);
		}
	}

	llvm::Function *materialize_candidate (llvm::Function *candidate, unsigned depth,
	                                       MonoCompile *config, bool *transient)
	{
		if (ineligible.contains (candidate))
			return nullptr;

		std::string name = candidate->getName ().str ();
		auto method = managed_method_from_symbol (name.c_str ());
		if (!method)
			return nullptr;

		if (method->iflags & METHOD_IMPL_ATTRIBUTE_NOINLINING) {
			trace_method ("method is noinline", method);
			return nullptr;
		}

		if (method->iflags & METHOD_IMPL_ATTRIBUTE_NOOPTIMIZATION) {
			trace_method ("method has optimization disabled", method);
			return nullptr;
		}

		if (mini_method_body_reports_caller_frame (method)) {
			trace_method ("method reports its own frame to the runtime", method);
			return nullptr;
		}

		// We allow a depth of 2 for regular methods, but more for methods that
		// opt into aggressive inlining.
		unsigned limit = method->iflags & METHOD_IMPL_ATTRIBUTE_AGGRESSIVE_INLINING
		                         ? MaxAggressiveInlineDepth
		                         : MaxInlineDepth;
		if (depth > limit) {
			/*
			 * Depth is a property of where this callee sits in the call graph
			 * right now, not of the callee. A later round folds its callers into
			 * the root and moves it closer, so it must stay reconsiderable.
			 */
			*transient = true;
			return nullptr;
		}

		auto body = materialize_callee (method, root, candidate);
		if (!body) {
			trace_method ("method is not supported by the tier1 jit", method);
			return nullptr;
		}

		body->addFnAttr (kMaterializedAttr);

		definitions[body] = candidate;
		declarations[candidate] = body;
		added.push_back (body);

		/*
		 * A body no call site ended up naming is dead on arrival - the strip
		 * below erases it. Say so rather than letting it reach the "left with no
		 * uses, so it must have been folded" tally, which is only true of a body
		 * something actually called.
		 */
		if (!replace_eligible_uses (candidate, body)) {
			trace_method ("method is only called from inside a protected region", method);
			return nullptr;
		}

		trace ("expose", body->getName ());
		if (trace_enabled ())
			materialized.insert (body->getName ().str ());
		return body;
	}

	/* How many call sites now name BODY instead of the trampoline DECL. */
	unsigned replace_eligible_uses (llvm::Function *decl, llvm::Function *body)
	{
		unsigned wired = 0;
		/*
		 * A body that carries its own EH clauses can only be folded into a call
		 * site that is NOT already inside a protected region - see the invoke
		 * check below.
		 */
		bool has_clauses = body->hasPersonalityFn ();

		for (auto &use : llvm::make_early_inc_range (decl->uses ())) {
			auto *cb = llvm::dyn_cast<llvm::CallBase> (use.getUser ());

			if (!cb || !cb->isCallee (&use))
				continue;

			/*
			 * An invoke site sits inside one of the CALLER's try regions, so
			 * folding a clause-bearing body in would nest the two methods' clauses
			 * together. LLVM does its half of that for us - it appends the call
			 * site's pad clauses to every pad it brings along, which is the chain
			 * .mono_lsda needs - but each of those pads also carries a selector
			 * switch, emitted back when the callee was translated and knowing only
			 * the callee's own clauses. The runtime would arrive at one with RDX
			 * naming an enclosing clause the switch has no case for and fall
			 * through to the callee's own handler: the wrong handler, silently.
			 *
			 * At a plain call site nothing in the caller protects the region, so
			 * the callee's clauses land as an island - no cross-method nesting,
			 * every pad's switch already complete. Leave the invoke sites calling
			 * the trampoline; the body is still folded everywhere else.
			 */
			if (has_clauses && llvm::isa<llvm::InvokeInst> (cb))
				continue;

			// A call site may pass a vtable/mrgctx in the `nest` parameter - the
			// caller decides that from the callee's metadata. The body accepts it
			// and ignores it: materialize_callee () compiles the exact
			// instantiation, so there is no generic context to recover from it,
			// and it refuses anything that comes back gshared/gsharedvt.
			//
			// If the declaration signature is not equivalent then we should ICE.
			g_assert (cb->getFunctionType () == body->getFunctionType ());

			cb->setCalledFunction (body);
			wired ++;
		}

		return wired;
	}
};

} // namespace

PreservedAnalyses
MonoInlinerPass::run (Module &module, ModuleAnalysisManager &mam)
{
	MonoInlinerState state (&module, state_, root_);
	auto pa = state.run (mam, *pb_, level_);

	VerifierPass ().run (module, mam);

	return pa;
}

namespace {

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
	pic.registerAfterPassCallback ([state] (StringRef id, Any, const PreservedAnalyses &) {
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
	explicit PipelineSplicer (ModulePassManager &&built) : ModulePassManager (std::move (built))
	{
	}

	/*
	 * Swap PASS in for the sole entry named NAME, keeping its position. Fails
	 * (changing nothing) unless exactly one entry matches - see the caller for
	 * why that is treated as a hard error rather than a fallback.
	 */
	template<typename PassT>
	bool replace_sole (StringRef name, PassT &&pass)
	{
		size_t at = 0;
		unsigned found = 0;
		for (size_t i = 0, n = Passes.size (); i < n; ++i) {
			if (Passes[i]->name () == name) {
				at = i;
				++found;
			}
		}
		if (found != 1)
			return false;

		/* addPass () appends, wrapping in the right pass model; move it home. */
		addPass (std::forward<PassT> (pass));
		Passes[at] = std::move (Passes.back ());
		Passes.pop_back ();
		return true;
	}

	template<typename PassT>
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
build_tier1_pipeline (PassBuilder &pb, PassInstrumentationCallbacks &pic, OptimizationLevel level,
                      const Tier1Root &root)
{
	auto state = std::make_shared<RoundState> ();
	register_round_callbacks (pic, state);

	PipelineSplicer pipeline (pb.buildPerModuleDefaultPipeline (level));

	if (!pipeline.replace_sole (kStockInlinerPass, MonoInlinerPass (pb, level, state, root))) {
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
		pipeline.prepend (MonoInlinerPass (pb, level, state, root));
	}

	return pipeline.take ();
}

} // namespace mono

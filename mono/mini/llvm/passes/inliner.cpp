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
 *   - does not ask the runtime about its own frame - GetCurrentMethod,
 *     GetCallingAssembly, StackTrace's frame 0. Folding such a callee in makes
 *     those report the caller's frame instead (gate #25);
 *   - and the invoke / musttail guards.
 * The EH exclusion is a hard safety gate; getting it wrong is a silent
 * miscompile, so it is asserted by the differential fixtures in
 * inliner-tests.cs, not eyeballed. materialize_callee () additionally refuses
 * wrappers, synchronized methods, open/shared-generic callees and anything that
 * still comes back gshared/gsharedvt mono-side.
 *
 * Generic callees are folded in by compiling the exact instantiation rather than
 * the shared body (materialize_callee ()), which is why a call site passing a
 * vtable/mrgctx in `nest` is no longer refused: the folded body is specialized,
 * so it has no generic context to recover and simply ignores the argument.
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

	llvm::SmallVector<llvm::Function *, 8> roots;

	// Functions that are known to be statically ineligible for inlining.
	llvm::DenseSet<llvm::Function *> ineligible;

	// A map of imported body back to its original function declaration.
	llvm::DenseMap<llvm::Function *, llvm::Function *> definitions;
	llvm::DenseMap<llvm::Function *, llvm::Function *> declarations;

	// What is the lowest depth that this function has been processed at, if any.
	llvm::DenseMap<llvm::Function *, unsigned> depth_cache;

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
	MonoInlinerState (llvm::Module *module, const std::shared_ptr<RoundState> &round_state)
	    : module (module), round_state (round_state)
	{
		for (auto &func : *module) {
			if (func.isDeclaration ())
				continue;
			if (!func.hasFnAttribute (kRootAttr))
				continue;

			auto config = tier1_root_cfg (&func);
			if (!config)
				continue;

			// Make sure we can tell which method is under consideration here.
			trace ("root", func.getName ());

			roots.push_back (&func);
		}
	}

	static const unsigned MaxInlinerRounds = 10;

	PreservedAnalyses run (llvm::ModuleAnalysisManager &mam, llvm::PassBuilder &pb,
	                       llvm::OptimizationLevel level)
	{
		// PassInstrumentationAnalysis is a required analysis and never
		// invalidated, so one fetch covers every round.
		const PassInstrumentation pi = mam.getResult<PassInstrumentationAnalysis> (*module);

		for (unsigned round = 0; round < MaxInlinerRounds; ++round) {
			// Nothing new was added, so there's nothing else to do.
			// We always need to run at least one round, though, because the
			if (!import_candidates () && round != 0)
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

		for (auto root : roots) {
			auto config = tier1_root_cfg (root);
			process_candidate (root, 0, config);
		}

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
			// We've already imported this method, nothing to do here.
			if (declarations.contains (candidate))
				return;

			auto body = materialize_candidate (candidate, depth, config);
			if (!body) {
				ineligible.insert (candidate);
				return;
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
	                                       MonoCompile *config)
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

		if (callee_reads_cctor_guarded_static (method)) {
			trace_method ("method reads a static field of an uninitialized class", method);
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
		if (depth > limit)
			return nullptr;

		auto body = materialize_callee (method, config, module);
		if (!body) {
			trace_method ("method is not supported by the tier1 jit", method);
			return nullptr;
		}

		body->addFnAttr (kMaterializedAttr);

		definitions[body] = candidate;
		declarations[candidate] = body;
		added.push_back (body);

		trace ("expose", body->getName ());
		if (trace_enabled ())
			materialized.insert (body->getName ().str ());
		replace_eligible_uses (candidate, body);
		return body;
	}

	void replace_eligible_uses (llvm::Function *decl, llvm::Function *body)
	{
		for (auto &use : llvm::make_early_inc_range (decl->uses ())) {
			auto *cb = llvm::dyn_cast<llvm::CallBase> (use.getUser ());

			if (!cb || !cb->isCallee (&use))
				continue;

			// A call site may still pass a vtable/mrgctx in the `nest` parameter -
			// the caller decided that from the callee's metadata. The body accepts
			// it and ignores it: materialize_callee () compiles the exact
			// instantiation, so there is no generic context left to recover from
			// it, and it refuses anything that comes back gshared/gsharedvt.
			//
			// If the declaration signature is not equivalent then we should ICE.
			g_assert (cb->getFunctionType () == body->getFunctionType ());

			cb->setCalledFunction (body);
		}
	}
};

} // namespace

PreservedAnalyses
MonoInlinerPass::run (Module &module, ModuleAnalysisManager &mam)
{
	MonoInlinerState state (&module, state_);
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
build_tier1_pipeline (PassBuilder &pb, PassInstrumentationCallbacks &pic, OptimizationLevel level)
{
	auto state = std::make_shared<RoundState> ();
	register_round_callbacks (pic, state);

	PipelineSplicer pipeline (pb.buildPerModuleDefaultPipeline (level));

	if (!pipeline.replace_sole (kStockInlinerPass, MonoInlinerPass (pb, level, state))) {
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

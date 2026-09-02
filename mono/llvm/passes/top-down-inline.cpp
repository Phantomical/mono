#include "top-down-inline.hpp"

#include "pipelines.hpp"

#include "clause-marker.hpp"
#include "finally-marker.hpp"
#include "inline-copies.hpp"
#include "inline-cost.hpp"
#include "tier-counter.hpp"

#include "../mono_lsda_format.hpp"

#include <llvm/ADT/SmallVector.h>
#include <llvm/Analysis/AssumptionCache.h>
#include <llvm/Analysis/BlockFrequencyInfo.h>
#include <llvm/Analysis/InlineCost.h>
#include <llvm/Analysis/ProfileSummaryInfo.h>
#include <llvm/Analysis/TargetLibraryInfo.h>
#include <llvm/Analysis/TargetTransformInfo.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/ValueHandle.h>
#include <llvm/Linker/Linker.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/Transforms/Utils/Cloning.h>

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

using namespace llvm;

namespace mono {

InlineCandidates::~InlineCandidates () = default;

AnalysisKey InlineCandidatesAnalysis::Key;

namespace {

struct Site {
	WeakTrackingVH call;
	uint64_t count = 0;
	unsigned depth = 0;
};

struct Colder {
	bool operator() (const Site &a, const Site &b) const { return a.count < b.count; }
};

/// A musttail site is a real tail call, and a body in its place takes that
/// away.
bool
foldable_site (const CallBase &call)
{
	const Function *callee = call.getCalledFunction ();

	return callee != nullptr && !callee->isIntrinsic () && !call.isMustTailCall ();
}

/// The analysis managers a module of the inliner's own is prepared under.
///
/// One set is built for the pass and emptied between candidates. The root's
/// managers describe a different module, and an analysis cached against one
/// module answers nothing about another.
struct ScratchAnalyses {
	LoopAnalysisManager lam;
	FunctionAnalysisManager fam;
	CGSCCAnalysisManager cgam;
	ModuleAnalysisManager mam;

	explicit ScratchAnalyses (PassBuilder &pb)
	{
		pb.registerModuleAnalyses (mam);
		pb.registerCGSCCAnalyses (cgam);
		pb.registerFunctionAnalyses (fam);
		register_mono_analyses (fam);
		pb.registerLoopAnalyses (lam);
		pb.crossRegisterProxies (lam, fam, cgam, mam);
	}

	void clear ()
	{
		mam.clear ();
		cgam.clear ();
		fam.clear ();
		lam.clear ();
	}
};

/// Brings the body behind \p decl into \p m, in the shape the cost model reads.
///
/// It is translated into a module of its own and prepared there, then linked
/// over the declaration the site already calls. Preparing it away from the root
/// is what keeps the preparation from reaching the root: it runs module passes,
/// and one of them is an always-inliner, which would otherwise fold bodies into
/// the root behind the walk that is working on it.
///
/// Returns null when the engine refused the method or the link declined, and
/// the site then keeps its call.
Function *
materialize_candidate (Module &m, Function &decl, InlineCandidates &candidates,
                       ModulePassManager &prepare, ScratchAnalyses &scratch)
{
	std::string name = decl.getName ().str ();
	auto into = std::make_unique<Module> (name, m.getContext ());

	// The preparation reads both, and the link refuses a module that disagrees
	// with the one it goes into.
	into->setDataLayout (m.getDataLayout ());
	into->setTargetTriple (m.getTargetTriple ());

	Function *made = candidates.materialize (decl, *into);

	if (made == nullptr)
		return nullptr;

	/*
	 * The engine returned a body the module holds already, so there is
	 * nothing to prepare and nothing to link. Move the sites off the
	 * declaration the way the link below does and hand it back.
	 */
	if (made->getParent () == &m) {
		decl.replaceAllUsesWith (made);
		decl.eraseFromParent ();
		return made;
	}

	/*
	 * A copy is internal, which is what lets an inliner delete it once every
	 * call to it is folded. Internal is also what stops it from satisfying the
	 * declaration the site calls, so it crosses as external and is put back
	 * once it is across.
	 */
	std::string copy = made->getName ().str ();

	made->setLinkage (GlobalValue::ExternalLinkage);

	/*
	 * The mark comes off for the run below and goes back on once the body is
	 * across. The preparation runs StripInlineCopiesPass, which takes the
	 * body off everything wearing the mark, and here the copy is what the
	 * module is about rather than a spare beside a caller.
	 *
	 * It has to go back on, because that is what the root's own strip finds a
	 * candidate the cost model refused by, and what a later site calling the
	 * same method recognizes the body it already has by.
	 */
	Attribute mark = made->getFnAttribute (inline_copy_attribute);

	made->removeFnAttr (inline_copy_attribute);

	prepare.run (*into, scratch.mam);
	scratch.clear ();

	// Read before the link. Satisfying a declaration is what the link does to
	// it, and the pointer does not survive that.
	if (Linker::linkModules (m, std::move (into)))
		return nullptr;

	Function *body = m.getFunction (copy);

	if (body == nullptr || body->isDeclaration ())
		return nullptr;

	body->setLinkage (GlobalValue::InternalLinkage);

	if (mark.isValid ())
		body->addFnAttr (mark);

	/*
	 * The translator names a copy for itself rather than after the declaration
	 * it stands in for, so the site can still be calling the one it always
	 * did.
	 */
	Function *site = m.getFunction (name);

	if (site != nullptr && site != body) {
		site->replaceAllUsesWith (body);
		site->eraseFromParent ();
	}

	return body;
}

/// Whether callee's own translation wrote a clause of its own, rather than
/// merely calling into code the caller's clauses already cover.
bool
has_own_clause (const Function &callee)
{
	for (const Instruction &i : instructions (callee))
		if (isa<LandingPadInst> (i) || finally_body_marker (i))
			return true;

	return false;
}

/// Whether every clause on every landing pad callee's own translation wrote
/// is a kind the merge below knows how to place: a catch, a finally or a
/// fault. It decodes each one the way eh-gather.cpp reads a landing pad's
/// TypeIds back at the machine level, because the front end publishes every
/// clause kind - filter included - through the same smuggled global, so this
/// is the one place that tells them apart before codegen.
///
/// A filter clause answers no, the same as a decode this pass does not
/// understand. What "merged" describes is scoped to this function alone:
/// a decline here still leaves clause_survives_fold ()'s own trial as the
/// correctness gate underneath, and it is eh-gather.cpp's own decode of the
/// same marker that places a merged clause once this has let the real fold
/// through. mono_lsda.cpp's per-owner join already resolves catch_class off
/// whichever header the clause's owner names, the same as it always has for
/// the root's own catch clauses, so a catch needs no case of its own there.
bool
mergeable_clause_kinds_only (const Function &callee)
{
	for (const Instruction &i : instructions (callee)) {
		const auto *lpi = dyn_cast<LandingPadInst> (&i);

		if (lpi == nullptr)
			continue;

		for (unsigned c = 0; c < lpi->getNumClauses (); ++c) {
			// mono never writes an LLVM-level filter clause of its own
			// (eh-gather.cpp's has_filter is the ECMA `catch (T) when (cond)`
			// case, a different thing entirely) - one here is not a shape
			// this backend produces, so it is refused rather than read.
			if (lpi->isFilter (c))
				return false;

			const auto *gv = dyn_cast<GlobalValue> (lpi->getClause (c));
			int clause_index, kind;

			if (!decode_clause_marker (gv, clause_index, kind))
				return false;

			switch ((std::uint32_t) kind) {
			case MONO_ECMA_CLAUSE_NONE:
			case MONO_ECMA_CLAUSE_FINALLY:
			case MONO_ECMA_CLAUSE_FAULT:
				break;
			default:
				return false;
			}
		}
	}

	return true;
}

/// The kind clause_survives_fold () tags callee's own landing pads and
/// finally markers with, to tell them apart once they are cloned into
/// another function.
constexpr StringRef clause_trial_tag = "mono.clause-trial";

/// Whether folding call leaves one of callee's own landing pads or finally
/// markers live.
///
/// eh-gather.cpp reads a folded body's geometry off the root's own
/// !mono.clauses, so a clause the fold leaves live has nothing left to
/// describe it. MonoFinallyRangePass reads a folded finally's markers
/// against that same table. This clones the site's function and folds call
/// into the clone. It then runs the same simplification the round applies
/// for real, and reads back whether a tagged landing pad or marker is still
/// standing.
///
/// The argument that answers callee's own type test is fixed at this call
/// site, so nothing the clone misses can change the verdict.
bool
clause_survives_fold (CallBase &call, Function &callee, FunctionPassManager &simplify,
                      FunctionAnalysisManager &fam,
                      function_ref<AssumptionCache & (Function &)> get_ac)
{
	MDNode *tag = MDNode::get (callee.getContext (), {});

	for (Instruction &i : instructions (callee))
		if (isa<LandingPadInst> (i) || finally_body_marker (i))
			i.setMetadata (clause_trial_tag, tag);

	ValueToValueMapTy vmap;
	Function *trial = CloneFunction (call.getFunction (), vmap);
	auto *site = cast<CallBase> (static_cast<Value *> (vmap[&call]));

	trial->removeFnAttr (tier_counter_attribute);

	InlineFunctionInfo ifi (get_ac);
	bool survives = true;

	if (InlineFunction (*site, ifi, /*MergeAttributes=*/true).isSuccess ()) {
		simplify.run (*trial, fam);

		survives = false;
		for (Instruction &i : instructions (*trial))
			if (i.getMetadata (clause_trial_tag) != nullptr) {
				survives = true;
				break;
			}
	}

	fam.clear (*trial, trial->getName ());
	trial->eraseFromParent ();

	for (Instruction &i : instructions (callee))
		i.setMetadata (clause_trial_tag, nullptr);

	return survives;
}

} // namespace

PreservedAnalyses
TopDownInlinerPass::run (Module &m, ModuleAnalysisManager &mam)
{
	InlineCandidates *candidates = mam.getResult<InlineCandidatesAnalysis> (m).candidates;

	if (candidates == nullptr)
		return PreservedAnalyses::all ();

	SmallVector<Function *, 4> roots;

	// The bodies a method promotes through, which is what the profile is about
	// and what a folded frame belongs to. A filter body and a copy carry no
	// counter, so neither is one.
	for (Function &fn : m)
		if (!fn.isDeclaration () && fn.hasFnAttribute (tier_counter_attribute))
			roots.push_back (&fn);

	if (roots.empty ())
		return PreservedAnalyses::all ();

	FunctionAnalysisManager &fam =
		mam.getResult<FunctionAnalysisManagerModuleProxy> (m).getManager ();
	ProfileSummaryInfo &psi = mam.getResult<ProfileSummaryAnalysis> (m);
	PassBuilder pb (target_);
	ScratchAnalyses scratch (pb);

	auto get_ac = [&] (Function &f) -> AssumptionCache & {
		return fam.getResult<AssumptionAnalysis> (f);
	};
	auto get_tli = [&] (Function &f) -> const TargetLibraryInfo & {
		return fam.getResult<TargetLibraryAnalysis> (f);
	};
	auto get_bfi = [&] (Function &f) -> BlockFrequencyInfo & {
		return fam.getResult<BlockFrequencyAnalysis> (f);
	};

	InlineParams params = mono::getInlineParams ();
	bool changed = false;

	for (Function *root : roots) {
		std::vector<Site> queue;

		/*
		 * Read through get_bfi () every time rather than holding a reference.
		 * A fold drops the root's cached analyses, so the reference from before
		 * one is a reference to freed memory.
		 */
		auto push = [&] (CallBase *call, unsigned depth) {
			if (!foldable_site (*call))
				return;

			uint64_t count = get_bfi (*root)
			                         .getBlockProfileCount (call->getParent ())
			                         .value_or (0);

			queue.push_back (Site { call, count, depth });
			std::push_heap (queue.begin (), queue.end (), Colder ());
		};

		auto read_sites = [&] () {
			for (Instruction &i : instructions (*root))
				if (auto *call = dyn_cast<CallBase> (&i))
					push (call, 0);
		};

		unsigned rounds = candidates->round_limit ();
		bool took_one = false;

		read_sites ();

		while (true) {
			/*
			 * A round ends when the queue runs out. What a fold buys is the
			 * caller's arguments as constants inside the folded body and the
			 * branches that kill, so simplification runs over what the round
			 * took. Here rather than as a pipeline row behind the pass, so a
			 * compile that folded nothing pays nothing.
			 *
			 * The sites are then read again. Those constants settle what a
			 * dispatch below the fold reads, and the simplification answers it
			 * with a direct call - a site nothing offered before. An interface
			 * dispatch enters the root as a load rather than as a call of a
			 * function, so this is the only way it becomes a site at all.
			 *
			 * A round that folded nothing has nothing to expose, so the loop
			 * stops there. What bounds the work is the engine's budget, which
			 * refuses a candidate once the compile has spent its translation.
			 * The count is what stops a cycle.
			 */
			if (queue.empty ()) {
				if (!took_one)
					break;

				PreservedAnalyses kept = simplify_.run (*root, fam);

				fam.invalidate (*root, kept);
				changed = true;

				if (--rounds == 0)
					break;

				took_one = false;
				read_sites ();
				continue;
			}

			std::pop_heap (queue.begin (), queue.end (), Colder ());
			Site site = queue.back ();
			queue.pop_back ();

			auto *call = dyn_cast_or_null<CallBase> (site.call);

			if (call == nullptr || site.depth >= candidates->depth_limit ()
			    || !foldable_site (*call))
				continue;

			Function *callee = call->getCalledFunction ();

			/*
			 * A body already beside the root is one an inliner put there, and
			 * it is weighed like any other. Anything else is a declaration of
			 * a published entry, which is what the engine translates from.
			 */
			if (callee->isDeclaration ()) {
				callee = materialize_candidate (m, *callee, *candidates,
				                                materialize_, scratch);

				if (callee == nullptr)
					continue;

				/*
				 * The link defines a function the module already held a
				 * declaration of, so the site still calls what it did. Read
				 * it back all the same: a link that had to rename would leave
				 * the site pointing at a declaration nothing defines.
				 */
				call = dyn_cast_or_null<CallBase> (site.call);

				if (call == nullptr || call->getCalledFunction () != callee)
					continue;
			} else if (!callee->hasFnAttribute (inline_copy_attribute)) {
				continue;
			}

			InlineCost cost = mono::getInlineCost (
				*call, callee, params,
				fam.getResult<TargetIRAnalysis> (*callee), get_ac, get_tli,
				get_bfi, &psi);

			if (!cost) {
				candidates->declined (*root, *callee, cost, site.count);
				continue;
			}

			if (has_own_clause (*callee)
			    && clause_survives_fold (*call, *callee, simplify_, fam, get_ac)
			    && !mergeable_clause_kinds_only (*callee)) {
				candidates->declined (
					*root, *callee,
					InlineCost::getNever ("its clause has nowhere to sit once folded"),
					site.count);
				continue;
			}

			/*
			 * A surviving catch, finally or fault clause falls through to the
			 * real fold below rather than being declined. eh-gather.cpp reads
			 * such a clause's owner straight off its own marker, whichever
			 * function's landing pad it ends up on after codegen, so nothing
			 * here has to flag that a merge happened - the fold itself is
			 * what makes one.
			 */

			/*
			 * No BFI on either side, so InlineFunction () scales nothing: the
			 * caller's is thrown away below, and the callee has no entry count
			 * for the subtraction the flag also gates. What the cloned blocks
			 * carry is the callee's own branch weights, which are ratios and
			 * are already right.
			 */
			InlineFunctionInfo ifi (get_ac, &psi);

			if (!InlineFunction (*call, ifi, /*MergeAttributes=*/true).isSuccess ())
				continue;

			/*
			 * Everything cached for the root describes the body before the
			 * fold, down to the dominator tree the next pass over it reads. The
			 * count a site is ranked by then comes from a BFI computed over the
			 * blocks that are really there.
			 */
			fam.invalidate (*root, PreservedAnalyses::none ());

			candidates->folded (*root, *callee);
			took_one = true;

			for (CallBase *exposed : ifi.InlinedCallSites)
				push (exposed, site.depth + 1);
		}
	}

	return changed ? PreservedAnalyses::none () : PreservedAnalyses::all ();
}

} // namespace mono

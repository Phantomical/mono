#include "top-down-inline.hpp"

#include "inline-copies.hpp"
#include "tier-counter.hpp"

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
	 * The engine answered with a body the module holds already, so there is
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

	InlineParams params = getInlineParams ();
	bool changed = false;

	for (Function *root : roots) {
		std::vector<Site> queue;
		bool took_one = false;

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

		for (Instruction &i : instructions (*root))
			if (auto *call = dyn_cast<CallBase> (&i))
				push (call, 0);

		while (!queue.empty ()) {
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

			InlineCost cost = getInlineCost (*call, callee, params,
			                                 fam.getResult<TargetIRAnalysis> (*callee),
			                                 get_ac, get_tli, get_bfi, &psi);

			if (!cost)
				continue;

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

		/*
		 * What a fold buys is the caller's arguments as constants inside the
		 * folded body and the branches that kills, so simplification has to run
		 * again over what the loop took. Here rather than as a pipeline row
		 * behind the pass, so a compile that folded nothing pays nothing.
		 */
		if (took_one) {
			PreservedAnalyses kept = simplify_.run (*root, fam);

			fam.invalidate (*root, kept);
			changed = true;
		}
	}

	return changed ? PreservedAnalyses::none () : PreservedAnalyses::all ();
}

} // namespace mono

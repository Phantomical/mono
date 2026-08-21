#include "top-down-inline.hpp"

#include "array-address.hpp"
#include "inline-copies.hpp"
#include "lower-builtins.hpp"
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
#include <llvm/Passes/PassBuilder.h>
#include <llvm/Transforms/IPO/AlwaysInliner.h>
#include <llvm/Transforms/Utils/Cloning.h>

#include <algorithm>
#include <vector>

using namespace llvm;

namespace mono {

InlineCandidates::~InlineCandidates () = default;

namespace {

/// A call the loop has still to weigh, with the count it was ranked by and how
/// many folds it sits behind the root.
struct Site {
	WeakTrackingVH call;
	uint64_t count = 0;
	unsigned depth = 0;
};

struct Colder {
	bool operator() (const Site &a, const Site &b) const { return a.count < b.count; }
};

/// Whether a site is one the loop may take at all.
///
/// A musttail site is a real tail call, and a body in its place takes that
/// away. An intrinsic has no managed method behind it, and neither does a call
/// through a pointer.
bool
foldable_site (const CallBase &call)
{
	const Function *callee = call.getCalledFunction ();

	return callee != nullptr && !callee->isIntrinsic () && !call.isMustTailCall ();
}

/// Puts a freshly translated body into the shape the cost model reads.
///
/// It arrives as raw translator output. That is behind the passes that lower
/// what the front end left symbolic, and behind the simplification the rest of
/// the module has had. Managed IR is at its most inflated there: a null check on
/// every dereference, a bounds check on every element. A threshold read against
/// that is spent before any of the real work is costed.
void
canonicalize (Module &m, Function &body, FunctionPassManager &simplify,
              FunctionAnalysisManager &fam, ModuleAnalysisManager &mam)
{
	ArrayAddressPass ().run (m, mam);
	LowerBuiltinsPass ().run (m, mam);

	// The body brought its own getters and forwarders with it, each marked
	// always-inline. Folding them here keeps the cost model from weighing a
	// chain of forwarders as though it were work.
	AlwaysInlinerPass ().run (m, mam);

	PreservedAnalyses kept = simplify.run (body, fam);

	// The pipeline caches analyses as it goes, and the cost model reads this
	// function's straight afterwards.
	fam.invalidate (body, kept);
}

} // namespace

PreservedAnalyses
TopDownInlinerPass::run (Module &m, ModuleAnalysisManager &mam)
{
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
	FunctionPassManager simplify =
		pb.buildFunctionSimplificationPipeline (OptimizationLevel::O3,
	                                                ThinOrFullLTOPhase::None);

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

			if (call == nullptr || site.depth >= candidates_->depth_limit ()
			    || !foldable_site (*call))
				continue;

			Function *callee = call->getCalledFunction ();

			/*
			 * A body already beside the root is one an inliner put there, and
			 * it is weighed like any other. Anything else is a declaration of
			 * a published entry, which is what the engine translates from.
			 */
			if (callee->isDeclaration ()) {
				callee = candidates_->materialize (*callee);

				if (callee == nullptr)
					continue;

				canonicalize (m, *callee, simplify, fam, mam);

				// Canonicalizing runs passes over the whole module, so read
				// the site back rather than trusting what it was.
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

			candidates_->folded (*root, *callee);
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
			PreservedAnalyses kept = simplify.run (*root, fam);

			fam.invalidate (*root, kept);
			changed = true;
		}
	}

	return changed ? PreservedAnalyses::none () : PreservedAnalyses::all ();
}

} // namespace mono

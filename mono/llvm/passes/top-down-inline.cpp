#include "top-down-inline.hpp"

#include "analysis/constant-values.hpp"
#include "class-init-elision.hpp"
#include "inline-copies.hpp"
#include "inline-cost.hpp"
#include "inline-materialize.hpp"
#include "tier-counter.hpp"

#include <llvm/ADT/DenseMap.h>
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
#include <llvm/Transforms/Utils/Cloning.h>

#include <algorithm>
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
	uint32_t size = 0;
};

/// True when \p a has lower profile density than \p b.
struct LessDense {
	bool operator() (const Site &a, const Site &b) const { return density (a) < density (b); }

	/// Unknown sizes are treated as one byte; materialize () still decides
	/// whether the candidate can be translated.
	static double density (const Site &s)
	{
		return double (s.count) / std::max (1u, s.size);
	}
};

/// A musttail site is a real tail call, and a body in its place takes that
/// away.
bool
inlinable_site (const CallBase &call)
{
	const Function *callee = call.getCalledFunction ();

	// Keep class-init calls visible to ClassInitGuardPass.
	return callee != nullptr && !callee->isIntrinsic () && !call.isMustTailCall ()
	       && !callee->hasFnAttribute (class_init_attribute);
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
	// and what an inlined frame belongs to. A filter body and a copy carry no
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
	auto get_constants = [&] (Function &f) -> ConstantValues & {
		return fam.getResult<MonoMemoryValues> (f);
	};

	InlineParams params = mono::getInlineParams ();
	bool changed = false;

	for (Function *root : roots) {
		std::vector<Site> queue;

		// Several sites can refer to the same callee, so avoid repeating the
		// metadata lookup for its IL size.
		DenseMap<const Function *, uint32_t> sizes;

		auto size_of = [&] (Function &callee) {
			auto [it, fresh] = sizes.try_emplace (&callee, 0);

			if (fresh)
				it->second = candidates->il_size (callee);

			return it->second;
		};

		/*
		 * Read through get_bfi () every time rather than holding a reference.
		 * An inline drops the root's cached analyses, so the reference from before
		 * one is a reference to freed memory.
		 */
		auto push = [&] (CallBase *call, unsigned depth) {
			if (!inlinable_site (*call))
				return;

			uint64_t count = get_bfi (*root).getBlockProfileCount (call->getParent ()).value_or (0);

			queue.push_back (Site{call, count, depth, size_of (*call->getCalledFunction ())});
			std::push_heap (queue.begin (), queue.end (), LessDense ());
		};

		/*
		 * A site an inline exposed keeps the depth it was reached at, so the
		 * queue is seeded with those before the root's own sites are read.
		 * Everything else starts over at zero, and the set is what keeps a
		 * site the walk below already holds from arriving twice.
		 */
		std::vector<Site> exposed;

		auto read_sites = [&] () {
			SmallPtrSet<const Value *, 32> queued;

			for (const Site &site : exposed) {
				auto *call = dyn_cast_or_null<CallBase> (site.call);

				if (call != nullptr && queued.insert (call).second)
					push (call, site.depth);
			}

			exposed.clear ();

			for (Instruction &i : instructions (*root))
				if (auto *call = dyn_cast<CallBase> (&i))
					if (queued.insert (call).second)
						push (call, 0);
		};

		/// A candidate the gathering loop took, waiting for the inlining one.
		struct Accepted {
			WeakTrackingVH call;
			Function *callee;
			InlineCost cost;
			uint64_t count;
			unsigned depth;
		};

		std::vector<Accepted> accepted;

		for (unsigned round = candidates->round_limit (); round > 0; --round) {
			// The last round's own simplify already ran, so every site
			// read_sites () is about to find below is ranked against what it
			// left standing. A root that is already out of budget declines
			// each of those the same way. Checking here is what spares such
			// a root the walk, a BFI query per site, and the round's own
			// re-simplify at the end - paid otherwise whether or not
			// anything left can inline.
			if (candidates->exhausted ())
				break;

			accepted.clear ();
			queue.clear ();
			read_sites ();

			while (!queue.empty ()) {
				std::pop_heap (queue.begin (), queue.end (), LessDense ());
				Site site = queue.back ();
				queue.pop_back ();

				auto *call = dyn_cast_or_null<CallBase> (site.call);

				if (call == nullptr || site.depth >= candidates->depth_limit ()
				    || !inlinable_site (*call))
					continue;

				Function *callee = call->getCalledFunction ();

				/*
				 * A body already beside the root is one an inliner put there, and
				 * it is weighed like any other. Anything else is a declaration of
				 * a published entry, which is what the engine translates from.
				 */
				if (callee->isDeclaration ()) {
					std::optional<SiteHeat> heat = mono::tier2_site_heat (*call, &get_bfi (*root));

					callee = materialize_candidate (m, *callee, *candidates, materialize_, scratch,
					                                *profile_fs_, heat, *call);

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
					*call, callee, params, fam.getResult<TargetIRAnalysis> (*callee), get_ac,
					get_tli, get_bfi, &psi, /*ORE=*/nullptr, /*GetEphValuesCache=*/nullptr,
					get_constants);

				if (!cost) {
					candidates->declined (*root, *callee, cost, site.count);
					continue;
				}

				// mergeable_clause_kinds_only () decides first: it is a cheap
				// read of callee's own clauses, and a filter is the only kind
				// that can still make this decline. Its answer already
				// settles the ordinary catch, finally and fault case.
				// Short-circuiting past clause_survives_inline () there keeps
				// its cost off every callee but the filter one it can still
				// affect.
				if (has_own_clause (*callee) && !mergeable_clause_kinds_only (*callee)
				    && clause_survives_inline (*call, *callee, simplify_, fam, get_ac)) {
					candidates->declined (
						*root, *callee,
						InlineCost::getNever ("its clause has nowhere to sit once inlined"),
						site.count);
					continue;
				}

				/*
				 * A surviving catch, finally or fault clause falls through to the
				 * real inline below rather than being declined. eh-gather.cpp reads
				 * such a clause's owner straight off its own marker, whichever
				 * function's landing pad it ends up on after codegen, so nothing
				 * here has to flag that a merge happened - the inline itself is
				 * what makes one.
				 */

				accepted.push_back (Accepted{site.call, callee, cost, site.count, site.depth});
			}

			if (accepted.empty ())
				break;

			for (const Accepted &take : accepted) {
				/*
				 * An earlier inline in this round can take a later site with it,
				 * and the handle goes null when it does.
				 */
				auto *call = dyn_cast_or_null<CallBase> (take.call);

				if (call == nullptr || call->getCalledFunction () != take.callee)
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

				candidates->inlined (*root, *take.callee, take.cost, take.count);

				for (CallBase *site : ifi.InlinedCallSites)
					exposed.push_back (Site{site, 0, take.depth + 1});
			}

			/*
			 * Everything cached for the root describes the body the round began
			 * with, down to the dominator tree the next pass over it reads. The
			 * count a site is ranked by then comes from a BFI computed over the
			 * blocks that are really there.
			 */
			fam.invalidate (*root, PreservedAnalyses::none ());

			/*
			 * What an inline buys is the caller's arguments as constants inside the
			 * inlined body and the branches that kill, so simplification runs over
			 * what the round took. Here rather than as a pipeline row behind the
			 * pass, so a compile that inlined nothing pays nothing.
			 *
			 * The next round reads the sites again. Those constants settle what a
			 * dispatch below the inline reads, and the simplification answers it
			 * with a direct call - a site nothing offered before. An interface
			 * dispatch enters the root as a load rather than as a call of a
			 * function, so this is the only way it becomes a site at all.
			 */
			PreservedAnalyses kept = simplify_.run (*root, fam);

			fam.invalidate (*root, kept);
			changed = true;
		}
	}

	return changed ? PreservedAnalyses::none () : PreservedAnalyses::all ();
}

} // namespace mono

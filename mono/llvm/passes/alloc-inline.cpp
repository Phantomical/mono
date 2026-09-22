#include "alloc-inline.hpp"

#include "alloc-func.hpp"
#include "analysis/constant-values.hpp"
#include "inline-copies.hpp"
#include "inline-cost.hpp"
#include "inline-materialize.hpp"
#include "tier-counter.hpp"
#include "top-down-inline.hpp"

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
#include <llvm/Support/CommandLine.h>
#include <llvm/Transforms/Utils/Cloning.h>

#include <algorithm>
#include <optional>
#include <vector>

using namespace llvm;

namespace mono {
namespace {

// Keep the option next to the pass it controls.
cl::opt<bool> InlineAllocator (
	"mono-inline-allocator", cl::Hidden, cl::init (false),
	cl::desc ("Weigh inlining a marked allocator's own body at each call "
	          "lower_allocations () resolved it to"));

struct Site {
	WeakTrackingVH call;
	uint64_t count = 0;
	uint32_t size = 0;
};

/// True when \p a has lower profile density than \p b.
struct LessDense {
	bool operator() (const Site &a, const Site &b) const { return density (a) < density (b); }

	static double density (const Site &s) { return double (s.count) / std::max (1u, s.size); }
};

bool
allocator_site (const CallBase &call)
{
	const Function *callee = call.getCalledFunction ();

	return callee != nullptr && !callee->isIntrinsic () && !call.isMustTailCall ()
	       && callee->hasFnAttribute (alloc_wrapper_attribute);
}

} // namespace

bool
allocation_inlining_enabled ()
{
	return InlineAllocator;
}

PreservedAnalyses
AllocationInlinerPass::run (Module &m, ModuleAnalysisManager &mam)
{
	InlineCandidates *candidates = mam.getResult<InlineCandidatesAnalysis> (m).candidates;

	if (candidates == nullptr || candidates->exhausted ())
		return PreservedAnalyses::all ();

	SmallVector<Function *, 4> roots;

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
		if (candidates->exhausted ())
			break;

		std::vector<Site> queue;

		for (Instruction &i : instructions (*root))
			if (auto *call = dyn_cast<CallBase> (&i))
				if (allocator_site (*call))
					queue.push_back (
						Site{call, get_bfi (*root).getBlockProfileCount (call->getParent ()).value_or (0),
						     candidates->il_size (*call->getCalledFunction ())});

		if (queue.empty ())
			continue;

		std::make_heap (queue.begin (), queue.end (), LessDense ());

		// Candidates are collected before any of them are inlined.
		struct Accepted {
			WeakTrackingVH call;
			Function *callee;
			InlineCost cost;
			uint64_t count;
		};

		std::vector<Accepted> accepted;

		while (!queue.empty ()) {
			std::pop_heap (queue.begin (), queue.end (), LessDense ());
			Site site = queue.back ();
			queue.pop_back ();

			auto *call = dyn_cast_or_null<CallBase> (site.call);

			if (call == nullptr || !allocator_site (*call))
				continue;

			Function *callee = call->getCalledFunction ();

			// Materialize declarations; an existing inline copy is already ready.
			if (callee->isDeclaration ()) {
				std::optional<SiteHeat> heat = mono::tier2_site_heat (*call, &get_bfi (*root));

				callee = materialize_candidate (m, *callee, *candidates, materialize_, scratch,
				                                *profile_fs_, heat, *call);

				if (callee == nullptr)
					continue;

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

			if (has_own_clause (*callee) && !mergeable_clause_kinds_only (*callee)
			    && clause_survives_inline (*call, *callee, simplify_, fam, get_ac)) {
				candidates->declined (
					*root, *callee,
					InlineCost::getNever ("its clause has nowhere to sit once inlined"),
					site.count);
				continue;
			}

			accepted.push_back (Accepted{site.call, callee, cost, site.count});
		}

		if (accepted.empty ())
			continue;

		for (const Accepted &take : accepted) {
			auto *call = dyn_cast_or_null<CallBase> (take.call);

			if (call == nullptr || call->getCalledFunction () != take.callee)
				continue;

			InlineFunctionInfo ifi (get_ac, &psi);

			if (!InlineFunction (*call, ifi, /*MergeAttributes=*/true).isSuccess ())
				continue;

			candidates->inlined (*root, *take.callee, take.cost, take.count);
		}

		fam.invalidate (*root, PreservedAnalyses::none ());

		// Simplify the root after its allocator calls have been inlined.
		PreservedAnalyses kept = simplify_.run (*root, fam);

		fam.invalidate (*root, kept);
		changed = true;
	}

	return changed ? PreservedAnalyses::none () : PreservedAnalyses::all ();
}

} // namespace mono

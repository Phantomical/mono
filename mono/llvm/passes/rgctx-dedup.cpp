/**
 * \file
 * \brief Dropping a generic-context fetch that a dominating fetch already did.
 *
 * The runtime keeps each entry of a generic context in a slot, and the value a
 * slot holds does not change. So a fetch that returns normally leaves the slot
 * holding the answer every later fetch of it gets. A dominated fetch can take
 * the dominating one's result instead of calling again.
 *
 * That the test is dominance rather than "an earlier fetch in program order" is
 * the exception-handling case. Filling a slot can create a vtable and run a
 * class initializer, so the fill can throw:
 *
 *     try { x = C.field; } catch { }     ; invoke fill_rgctx(%ctx, 2)
 *     y = C.field;                       ; call fill_rgctx(%ctx, 2) - kept
 *
 * The catch reaches the second fetch without the first having returned, so the
 * slot can still be empty there. An invoke establishes its value along its
 * normal edge alone, which is what a DominatorTree query already says.
 */

#include "rgctx-dedup.hpp"
#include "rgctx-fetch.hpp"
#include "strip-casts.hpp"

#include <llvm/ADT/MapVector.h>
#include <llvm/ADT/SmallVector.h>
#include <llvm/ADT/StringRef.h>
#include <llvm/IR/Dominators.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>

using namespace llvm;

namespace mono {
namespace {

/// What this pass replaces: the fetch to erase, and the fetch whose value it
/// takes. The second one can be dead as well, so a reader of this map follows
/// it to a survivor.
using Replacements = MapVector<CallBase *, CallBase *>;

/// Whether two fetches read one slot of one context.
///
/// A fetch takes the context and the slot index, so equal arguments name the
/// same slot of the same context. Unequal ones are left alone, which at worst
/// keeps a fetch that was not needed.
bool
reads_the_same_slot (const CallBase *a, const CallBase *b)
{
	if (a->getCalledFunction () != b->getCalledFunction ()
	    || a->arg_size () != b->arg_size ())
		return false;

	for (unsigned i = 0; i < a->arg_size (); ++i)
		if (strip_casts (a->getArgOperand (i))
		    != strip_casts (b->getArgOperand (i)))
			return false;

	return true;
}

/// Records in dead each fetch in sites that another fetch of the same slot
/// dominates, together with the fetch that dominates it.
void
collect_redundant (const DominatorTree &dt, ArrayRef<CallBase *> sites,
                   Replacements &dead)
{
	for (CallBase *site : sites) {
		/* Dominance calls every unreachable site dominated, so this skips
		 * those. */
		if (!dt.isReachableFromEntry (site->getParent ()))
			continue;

		for (CallBase *earlier : sites)
			if (earlier != site && reads_the_same_slot (earlier, site)
			    && dt.dominates (earlier, site)) {
				dead[site] = earlier;
				break;
			}
	}
}

/// Follows dead from site to the fetch that stays.
///
/// Dominance is a strict partial order, so this chain is finite: each step
/// moves to a fetch that strictly dominates the one before it.
CallBase *
surviving_fetch (const Replacements &dead, CallBase *site)
{
	auto found = dead.find (site);

	while (found != dead.end ()) {
		site = found->second;
		found = dead.find (site);
	}

	return site;
}

/// Removes site. If site is an invoke, its normal edge becomes an
/// unconditional branch.
void
erase_fetch (CallBase *site)
{
	if (auto *invoke = dyn_cast<InvokeInst> (site)) {
		invoke->getUnwindDest ()->removePredecessor (invoke->getParent ());
		IRBuilder<> (invoke).CreateBr (invoke->getNormalDest ());
	}

	site->eraseFromParent ();
}

} // namespace

PreservedAnalyses
RgctxDedupPass::run (Function &f, FunctionAnalysisManager &fam)
{
	/* Keyed by the walk each fetch carries, so only fetches that can make
	 * each other redundant are ever compared. */
	SmallMapVector<StringRef, SmallVector<CallBase *, 4>, 4> by_walk;
	unsigned total = 0;

	for (Function &decl : f.getParent ()->functions ()) {
		if (!decl.isDeclaration () || !decl.hasFnAttribute (rgctx_fetch_attribute))
			continue;

		for (User *user : decl.users ()) {
			auto *site = dyn_cast<CallBase> (user);

			/*
			 * A fetch names the walk that reaches its slot and hands
			 * the value back as its result. Anything shaped otherwise
			 * is not a fetch this pass understands.
			 */
			if (site == nullptr || site->getFunction () != &f
			    || site->getCalledFunction () != &decl
			    || site->getType ()->isVoidTy ()
			    || !site->hasFnAttr (rgctx_walk_attribute))
				continue;
			by_walk[site->getFnAttr (rgctx_walk_attribute).getValueAsString ()]
				.push_back (site);
			++total;
		}
	}

	if (total < 2)
		return PreservedAnalyses::all ();

	Replacements dead;
	const DominatorTree *dt = nullptr;

	for (auto &entry : by_walk) {
		if (entry.second.size () < 2)
			continue;
		if (dt == nullptr)
			dt = &fam.getResult<DominatorTreeAnalysis> (f);
		collect_redundant (*dt, entry.second, dead);
	}

	if (dead.empty ())
		return PreservedAnalyses::all ();

	/* The map is complete here, so each replacement value is resolved
	 * against the fetches that stay rather than against one on its way out. */
	for (auto &entry : dead) {
		entry.first->replaceAllUsesWith (surviving_fetch (dead, entry.second));
		erase_fetch (entry.first);
	}

	return PreservedAnalyses::none ();
}

} // namespace mono

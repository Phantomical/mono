/**
 * \file
 * \brief Dropping class-init checks a dominating check already made.
 *
 * If a call to initialize class C returns normally, C is initialized for this
 * thread from that point on. A second call for C that the first dominates can
 * only find the work already done. A cctor that throws does not weaken that
 * claim. The throw leaves the first call along its unwind edge, so the
 * dominated site is not reachable that way.
 *
 * That the argument is about dominance rather than "an earlier call in program
 * order" is the whole of the exception-handling case. A check inside a try does
 * not dominate code the try's handler can reach:
 *
 *     try { C.x = 1; } catch { }        ; invoke class_init(@C)
 *     C.y = 2;                          ; call class_init(@C) - kept
 *
 * The catch reaches the second site without the first having returned, so the
 * class can still be uninitialized there. LLVM's dominance already says this:
 * an invoke's effect is established only along its normal edge. That is why
 * the test below is a plain DominatorTree query, not a scan back through
 * predecessors.
 */

#include "class-init.hpp"
#include "analysis/strip-casts.hpp"

#include <llvm/ADT/MapVector.h>
#include <llvm/ADT/SmallVector.h>
#include <llvm/IR/Dominators.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>

using namespace llvm;

namespace mono {
namespace {

/// The class is whatever the call names. It is a symbol for the vtable when
/// the class is known while compiling, and a value read from the runtime
/// generic context otherwise. Equal operands are the same class either way.
/// Unequal ones are left alone, which at worst keeps a check that was not
/// needed.
bool
same_class (const CallBase *a, const CallBase *b)
{
	if (a->arg_size () != b->arg_size ())
		return false;

	for (unsigned i = 0; i < a->arg_size (); ++i)
		if (strip_casts (a->getArgOperand (i))
		    != strip_casts (b->getArgOperand (i)))
			return false;

	return true;
}

/// Appends to dead the checks in sites that another check for the same class
/// dominates.
///
/// Dominance is a strict partial order, so the sites left standing are its
/// minimal elements. Everything dropped here still has a dominating check
/// that survives, by transitivity, even when the check that justified
/// dropping it goes too.
void
collect_redundant (const DominatorTree &dt, ArrayRef<CallBase *> sites,
                   SmallVectorImpl<CallBase *> &dead)
{
	for (CallBase *site : sites) {
		/* Dominance calls every unreachable site dominated, so this skips
		 * those. */
		if (!dt.isReachableFromEntry (site->getParent ()))
			continue;

		for (CallBase *earlier : sites)
			if (earlier != site && same_class (earlier, site)
			    && dt.dominates (earlier, site)) {
				dead.push_back (site);
				break;
			}
	}
}

/// Removes site. If site is an invoke, its normal edge becomes an
/// unconditional branch.
void
erase_check (CallBase *site)
{
	if (auto *invoke = dyn_cast<InvokeInst> (site)) {
		invoke->getUnwindDest ()->removePredecessor (invoke->getParent ());
		IRBuilder<> (invoke).CreateBr (invoke->getNormalDest ());
	}

	site->eraseFromParent ();
}

} // namespace

PreservedAnalyses
ClassInitPass::run (Function &f, FunctionAnalysisManager &fam)
{
	/* Keyed by the class each check names, so only checks that can make
	 * each other redundant are ever compared. */
	SmallMapVector<const Value *, SmallVector<CallBase *, 4>, 4> by_class;
	unsigned total = 0;

	for (Function &decl : f.getParent ()->functions ()) {
		if (!decl.isDeclaration () || !decl.hasFnAttribute (class_init_attribute))
			continue;

		for (User *user : decl.users ()) {
			auto *site = dyn_cast<CallBase> (user);

			/*
			 * A check names a class and returns nothing, so removing
			 * one costs no replacement value. Anything shaped
			 * otherwise is not a check this pass understands.
			 */
			if (site == nullptr || site->getFunction () != &f
			    || site->getCalledFunction () != &decl
			    || site->arg_size () < 1 || !site->use_empty ())
				continue;
			by_class[strip_casts (site->getArgOperand (0))].push_back (site);
			++total;
		}
	}

	if (total < 2)
		return PreservedAnalyses::all ();

	SmallVector<CallBase *, 4> dead;
	const DominatorTree *dt = nullptr;

	for (auto &entry : by_class) {
		if (entry.second.size () < 2)
			continue;
		if (dt == nullptr)
			dt = &fam.getResult<DominatorTreeAnalysis> (f);
		collect_redundant (*dt, entry.second, dead);
	}

	if (dead.empty ())
		return PreservedAnalyses::all ();

	for (CallBase *site : dead)
		erase_check (site);

	return PreservedAnalyses::none ();
}

} // namespace mono

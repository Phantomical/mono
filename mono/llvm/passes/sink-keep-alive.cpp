/**
 * \file
 * \brief Moving a delegate keep_alive () marker out of the loop it pins.
 */

#include "sink-keep-alive.hpp"

#include "analysis/strip-casts.hpp"

#include <llvm/ADT/DenseMap.h>
#include <llvm/ADT/SmallVector.h>
#include <llvm/Analysis/LoopInfo.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Dominators.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Intrinsics.h>

using namespace llvm;

namespace mono {
namespace {

/// Whether \p call is the marker call.cpp's keep_alive () writes.
bool
is_keep_alive_marker (const CallInst &call)
{
	return call.getIntrinsicID () == Intrinsic::fake_use && call.arg_size () == 1;
}

/// The outermost loop enclosing \p from where \p delegate stays one value
/// throughout. Null where \p from is null or the value never reaches that
/// far.
///
/// `Loop::isLoopInvariant ()` only asks whether \p delegate is defined
/// outside the loop. It does not ask whether that definition reaches the
/// loop, so each step out here also asks \p dt.
Loop *
outermost_invariant_loop (Loop *from, Value *delegate, DominatorTree &dt)
{
	Loop *outer = nullptr;

	for (Loop *at = from; at != nullptr; at = at->getParentLoop ()) {
		if (!at->isLoopInvariant (delegate))
			break;

		if (const auto *def = dyn_cast<Instruction> (delegate))
			if (!dt.dominates (def, at->getHeader ()))
				break;

		outer = at;
	}

	return outer;
}

} // namespace

PreservedAnalyses
SinkKeepAlivePass::run (Function &f, FunctionAnalysisManager &fam)
{
	SmallVector<CallInst *, 8> markers;

	for (Instruction &at : instructions (f))
		if (auto *call = dyn_cast<CallInst> (&at))
			if (is_keep_alive_marker (*call))
				markers.push_back (call);

	if (markers.empty ())
		return PreservedAnalyses::all ();

	LoopInfo &loops = fam.getResult<LoopAnalysis> (f);
	DominatorTree &dt = fam.getResult<DominatorTreeAnalysis> (f);

	// Groups markers by the loop they leave and the delegate the exit copy
	// names, so two markers that agree on both share one copy.
	DenseMap<std::pair<Loop *, Value *>, SmallVector<CallInst *, 2>> groups;

	for (CallInst *marker : markers) {
		Value *delegate =
			const_cast<Value *> (strip_casts (marker->getArgOperand (0)));
		Loop *outer = outermost_invariant_loop (
			loops.getLoopFor (marker->getParent ()), delegate, dt);

		if (outer != nullptr)
			groups[{ outer, delegate }].push_back (marker);
	}

	bool changed = false;

	for (auto &group : groups) {
		Loop *loop = group.first.first;
		Value *delegate = group.first.second;
		SmallVector<CallInst *, 2> &sited = group.second;

		SmallVector<BasicBlock *, 4> exits;
		loop->getUniqueExitBlocks (exits);

		// Nowhere past the loop to hold the delegate for: every path still
		// running it stays inside, and so does the marker.
		if (exits.empty ())
			continue;

		for (BasicBlock *exit : exits) {
			CallInst *copy = cast<CallInst> (sited.front ()->clone ());

			copy->setArgOperand (0, delegate);
			copy->insertBefore (exit->getFirstInsertionPt ());
		}

		for (CallInst *marker : sited)
			marker->eraseFromParent ();

		changed = true;
	}

	if (!changed)
		return PreservedAnalyses::all ();

	PreservedAnalyses preserved;

	// Every marker moves within blocks the CFG already had. None of them is
	// split, and no edge changes.
	preserved.preserveSet<CFGAnalyses> ();
	return preserved;
}

} // namespace mono

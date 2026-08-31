/**
 * \file
 * \brief Recognizing a finally whose own effects the optimizer already took
 * away, and erasing what the front end built to protect them.
 *
 * `llvm.experimental.stackmap` carries no explicit memory effects, so DCE
 * never removes one on its own. A finally whose real work is gone still
 * leaves its two body markers standing, bracketing nothing, with the
 * thread-abort check emit_finally_abort_check () (method-to-llvm/exceptions.cpp)
 * built behind them. This pass is what takes the rest of that shell down
 * once the markers are all that is left of it.
 */

#include "fold-empty-finally.hpp"

#include "finally-marker.hpp"

#include <llvm/ADT/DenseMap.h>
#include <llvm/ADT/STLExtras.h>
#include <llvm/ADT/SmallPtrSet.h>
#include <llvm/Analysis/ConstantFolding.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/IntrinsicInst.h>
#include <llvm/IR/Intrinsics.h>
#include <llvm/IR/Module.h>
#include <llvm/Transforms/Utils/BasicBlockUtils.h>
#include <llvm/Transforms/Utils/Local.h>

#include <cstdint>

namespace mono {

namespace {

/// One clause's marker, and whether more than one turned up - which the
/// front end never legitimately writes, so this pass leaves such a clause
/// alone rather than guess which one is real.
struct Marker {
	llvm::Instruction *inst = nullptr;
	bool ambiguous = false;
};

void
note (llvm::DenseMap<std::uint32_t, Marker> &table, std::uint32_t clause, llvm::Instruction &i)
{
	Marker &m = table[clause];

	if (m.inst == nullptr)
		m.inst = &i;
	else
		m.ambiguous = true;
}

/// Whether i describes a value without computing one, and so is not part of
/// what the finally's own IL asked for.
bool
is_bookkeeping (const llvm::Instruction &i)
{
	const auto *call = llvm::dyn_cast<llvm::IntrinsicInst> (&i);

	if (call == nullptr)
		return false;

	switch (call->getIntrinsicID ()) {
	case llvm::Intrinsic::dbg_declare:
	case llvm::Intrinsic::dbg_value:
	case llvm::Intrinsic::dbg_label:
	case llvm::Intrinsic::lifetime_start:
	case llvm::Intrinsic::lifetime_end:
		return true;
	default:
		return false;
	}
}

/// Whether the only path from begin to end runs through nothing but
/// unconditional branches and is_bookkeeping () instructions.
///
/// A block this walk steps into must have begin's block as its only
/// predecessor. That is what stops a finally with control flow of its own,
/// or an unrelated branch that happens to land on the same code, from
/// passing as empty. visited stops a cycle the same way.
bool
region_is_empty (llvm::Instruction *begin, llvm::Instruction *end)
{
	llvm::BasicBlock *bb = begin->getParent ();
	llvm::BasicBlock::iterator it = std::next (begin->getIterator ());
	llvm::SmallPtrSet<llvm::BasicBlock *, 8> visited { bb };

	for (;;) {
		llvm::Instruction *term = bb->getTerminator ();

		while (&*it != term) {
			if (&*it == end)
				return true;
			if (!is_bookkeeping (*it))
				return false;
			++it;
		}

		const auto *br = llvm::dyn_cast<llvm::BranchInst> (term);

		if (br == nullptr || br->isConditional ())
			return false;

		llvm::BasicBlock *next = br->getSuccessor (0);

		if (next->getUniquePredecessor () == nullptr || !visited.insert (next).second)
			return false;

		bb = next;
		it = bb->begin ();
	}
}

/// The clause's abort_guard alloca, read off begin's live value.
///
/// method-to-llvm.cpp gives every finally clause one dedicated i8 slot and
/// nothing ever offsets into it (exceptions.cpp), so the operand is always
/// the alloca itself.
llvm::AllocaInst *
guarded_alloca (llvm::Instruction *begin)
{
	const auto *call = llvm::cast<llvm::CallBase> (begin);

	// id, shadow bytes, then the guard - emit_finally_body_marker () (exceptions.cpp).
	if (call->arg_size () <= 2)
		return nullptr;

	return llvm::dyn_cast<llvm::AllocaInst> (call->getArgOperand (2));
}

/// Whether guard's only uses besides begin are stores of the constant zero
/// enter_finally () always writes and at most one load - the shape that
/// function and emit_finally_abort_check () build (exceptions.cpp). *load
/// names that use when it is present. It is absent when an earlier fold
/// already removed the abort check this clause never needed.
///
/// Anything else using guard's address means some code this pass does not
/// know about reads or writes the slot, and the fold has nothing safe to say
/// about it.
bool
guard_uses_are_simple (llvm::AllocaInst *guard, llvm::Instruction *begin, llvm::LoadInst *&load)
{
	load = nullptr;

	for (llvm::User *user : guard->users ()) {
		if (user == begin)
			continue;

		if (auto *store = llvm::dyn_cast<llvm::StoreInst> (user)) {
			const auto *stored = llvm::dyn_cast<llvm::ConstantInt> (store->getValueOperand ());

			if (stored == nullptr || !stored->isZero ())
				return false;
			continue;
		}

		auto *ld = llvm::dyn_cast<llvm::LoadInst> (user);

		if (ld == nullptr || load != nullptr)
			return false;

		load = ld;
	}

	return true;
}

/// Constant-folds bb forward from whatever load's replacement just made
/// foldable, through to its terminator.
///
/// This does not assume which of a conditional branch's edges the constant
/// takes. The same simplification that gave the finally's own body its
/// chance to prove empty runs over this branch too, and is as free to
/// canonicalize its predicate or swap its arms as it is anything else.
/// ConstantFoldTerminator () reads the branch back correctly regardless -
/// successor order and predicate direction included.
void
fold_forward (llvm::BasicBlock *bb)
{
	const llvm::DataLayout &dl = bb->getModule ()->getDataLayout ();

	for (bool changed = true; changed;) {
		changed = false;

		for (llvm::Instruction &i : llvm::make_early_inc_range (*bb)) {
			if (i.isTerminator ())
				continue;

			llvm::Constant *folded = llvm::ConstantFoldInstruction (&i, dl);

			if (folded == nullptr)
				continue;

			i.replaceAllUsesWith (folded);
			if (llvm::isInstructionTriviallyDead (&i)) {
				i.eraseFromParent ();
				changed = true;
			}
		}
	}

	llvm::ConstantFoldTerminator (bb, /* DeleteDeadConditions */ true);
}

/// Removes the branch load's volatile read gates, now that nothing can ever
/// discover guard's frame slot to set the byte the branch tests.
void
fold_abort_check (llvm::LoadInst *load)
{
	llvm::BasicBlock *test = load->getParent ();
	auto *br = llvm::cast<llvm::BranchInst> (test->getTerminator ());
	llvm::BasicBlock *a = br->getSuccessor (0);
	llvm::BasicBlock *b = br->getSuccessor (1);

	load->replaceAllUsesWith (llvm::ConstantInt::get (load->getType (), 0));
	load->eraseFromParent ();

	// ConstantFoldTerminator () takes fold_forward ()'s constant condition
	// back to whichever edge it settles on, and drops test from the other
	// successor's predecessors. Only that successor is left with nothing
	// still reaching it.
	fold_forward (test);

	if (llvm::pred_empty (a))
		llvm::DeleteDeadBlock (a);
	if (llvm::pred_empty (b))
		llvm::DeleteDeadBlock (b);
}

/// Tries every clause whose begin and end marker both turned up exactly once,
/// folding each whose body is empty. Returns whether it changed anything, so
/// the caller can run another round: folding an inner clause can be what
/// leaves an outer one empty in turn.
bool
fold_one_round (llvm::Function &f)
{
	llvm::DenseMap<std::uint32_t, Marker> begins, ends;

	for (llvm::Instruction &i : llvm::instructions (f)) {
		std::uint32_t clause;
		bool opening;

		if (finally_body_marker (i, &clause, &opening))
			note (opening ? begins : ends, clause, i);
	}

	bool changed = false;

	for (auto &entry : begins) {
		Marker &begin_marker = entry.second;

		if (begin_marker.ambiguous)
			continue;

		auto found = ends.find (entry.first);

		if (found == ends.end () || found->second.ambiguous)
			continue;

		llvm::Instruction *begin = begin_marker.inst;
		llvm::Instruction *end = found->second.inst;

		if (!region_is_empty (begin, end))
			continue;

		llvm::AllocaInst *guard = guarded_alloca (begin);
		llvm::LoadInst *load;

		if (guard == nullptr || !guard_uses_are_simple (guard, begin, load))
			continue;

		end->eraseFromParent ();
		begin->eraseFromParent ();

		if (load != nullptr)
			fold_abort_check (load);

		for (llvm::User *user : llvm::make_early_inc_range (guard->users ()))
			llvm::cast<llvm::StoreInst> (user)->eraseFromParent ();

		guard->eraseFromParent ();
		changed = true;
	}

	return changed;
}

} // namespace

llvm::PreservedAnalyses
FoldEmptyFinallyPass::run (llvm::Function &f, llvm::FunctionAnalysisManager &)
{
	bool changed = false;

	while (fold_one_round (f))
		changed = true;

	return changed ? llvm::PreservedAnalyses::none () : llvm::PreservedAnalyses::all ();
}

} // namespace mono

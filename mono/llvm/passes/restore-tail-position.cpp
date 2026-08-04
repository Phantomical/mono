#include "restore-tail-position.hpp"

#include <llvm/ADT/SetVector.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Instructions.h>
#include <llvm/Transforms/Utils/BasicBlockUtils.h>

#include <optional>

namespace mono {

namespace {

/// Whether CALL is asking to become a jump. TCK_NoTail is the opposite request
/// and TCK_None is no request at all; both are left alone.
bool
wants_to_be_a_jump (const llvm::CallInst *call)
{
	llvm::CallInst::TailCallKind kind = call->getTailCallKind ();

	return kind == llvm::CallInst::TCK_Tail || kind == llvm::CallInst::TCK_MustTail;
}

/// What a ret in MERGED returns when it is reached from BLOCK: nothing for a void
/// return, the value otherwise. Empty when that cannot be established here.
///
/// A merged return block returns either nothing, a constant, or a phi over its
/// predecessors - the last being what the merge itself creates. Anything else is
/// a value defined somewhere that need not be available in BLOCK, so it is left
/// alone rather than reasoned about.
std::optional<llvm::Value *>
returned_value_from (llvm::ReturnInst *ret, llvm::BasicBlock *merged, llvm::BasicBlock *block)
{
	llvm::Value *value = ret->getReturnValue ();

	if (value == nullptr)
		return nullptr;

	if (auto *phi = llvm::dyn_cast<llvm::PHINode> (value);
	    phi != nullptr && phi->getParent () == merged)
		return phi->getIncomingValueForBlock (block);
	if (llvm::isa<llvm::Constant> (value))
		return value;

	return std::nullopt;
}

} // namespace

llvm::PreservedAnalyses
RestoreTailPositionPass::run (llvm::Function &f, llvm::FunctionAnalysisManager &)
{
	llvm::SmallSetVector<llvm::BasicBlock *, 4> merged_blocks;

	for (llvm::BasicBlock &block : f) {
		/*
		 * The shape the merge leaves behind: the call, then an unconditional
		 * branch that took the ret's place. Only that exact shape is repaired -
		 * anything between the call and the terminator is something the backend
		 * would have had to look through anyway, and it is not this pass's job
		 * to decide whether it could.
		 */
		auto *branch = llvm::dyn_cast<llvm::BranchInst> (block.getTerminator ());

		if (branch == nullptr || !branch->isUnconditional ())
			continue;

		auto *call = llvm::dyn_cast_or_null<llvm::CallInst> (branch->getPrevNode ());

		if (call == nullptr || !wants_to_be_a_jump (call))
			continue;

		/* The branch has to lead to a block that does nothing but return. */
		llvm::BasicBlock *merged = branch->getSuccessor (0);
		auto *ret = llvm::dyn_cast<llvm::ReturnInst> (merged->getTerminator ());

		if (ret == nullptr || &*merged->getFirstNonPHIIt () != ret)
			continue;

		std::optional<llvm::Value *> value = returned_value_from (ret, merged, &block);

		if (!value)
			continue;

		/*
		 * While the branch is still standing, so this block still reads as the
		 * predecessor whose incoming values are being withdrawn.
		 */
		merged->removePredecessor (&block);

		llvm::IRBuilder<> builder (branch);

		if (*value != nullptr)
			builder.CreateRet (*value);
		else
			builder.CreateRetVoid ();

		branch->eraseFromParent ();
		merged_blocks.insert (merged);
	}

	if (merged_blocks.empty ())
		return llvm::PreservedAnalyses::all ();

	/*
	 * A merge whose every predecessor was a tail call leaves the merged block
	 * with none at all. Nothing branches there any more, and its phis would have
	 * no incoming values to speak of, so it goes.
	 */
	for (llvm::BasicBlock *merged : merged_blocks) {
		if (llvm::pred_empty (merged) && merged != &f.getEntryBlock ())
			llvm::DeleteDeadBlock (merged);
	}

	return llvm::PreservedAnalyses::none ();
}

} // namespace mono

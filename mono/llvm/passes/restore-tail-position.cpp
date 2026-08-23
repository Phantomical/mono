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

/// Whether call is asking to become a jump. TCK_NoTail is the opposite
/// request and TCK_None is no request at all. Both are left alone.
bool
wants_to_be_a_jump (const llvm::CallInst *call)
{
	llvm::CallInst::TailCallKind kind = call->getTailCallKind ();

	return kind == llvm::CallInst::TCK_Tail || kind == llvm::CallInst::TCK_MustTail;
}

/// Returns what a ret in merged returns when it is reached from block: null
/// for a void return, the value otherwise. Empty when we cannot establish
/// that here.
///
/// The value in a merged block's ret is either nothing, or the phi the merge
/// itself created over its predecessors. When it is that phi, the answer is
/// its incoming value for block.
///
/// The other two cases are what the phi decays into as its predecessors are
/// removed one at a time. Taking away all but the last collapses it to that
/// predecessor's own value: a constant, an argument, or an instruction the
/// predecessor computed itself. Anything else is left alone: we do not reason
/// about whether block can see it.
std::optional<llvm::Value *>
returned_value_from (llvm::ReturnInst *ret, llvm::BasicBlock *merged, llvm::BasicBlock *block)
{
	llvm::Value *value = ret->getReturnValue ();

	if (value == nullptr)
		return nullptr;

	if (auto *phi = llvm::dyn_cast<llvm::PHINode> (value);
	    phi != nullptr && phi->getParent () == merged)
		return phi->getIncomingValueForBlock (block);
	if (llvm::isa<llvm::Constant> (value) || llvm::isa<llvm::Argument> (value))
		return value;
	if (auto *instruction = llvm::dyn_cast<llvm::Instruction> (value);
	    instruction != nullptr && instruction->getParent () == block)
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
		 * branch that took the ret's place. Only that exact shape is repaired.
		 * Codegen must then look through whatever sits between the call and
		 * the terminator, and deciding whether it can is not this pass's job.
		 */
		auto *branch = llvm::dyn_cast<llvm::BranchInst> (block.getTerminator ());

		if (branch == nullptr || !branch->isUnconditional ())
			continue;

		auto *call = llvm::dyn_cast_or_null<llvm::CallInst> (branch->getPrevNode ());

		if (call == nullptr || !wants_to_be_a_jump (call))
			continue;

		llvm::BasicBlock *merged = branch->getSuccessor (0);
		auto *ret = llvm::dyn_cast<llvm::ReturnInst> (merged->getTerminator ());

		if (ret == nullptr || &*merged->getFirstNonPHIIt () != ret)
			continue;

		std::optional<llvm::Value *> value = returned_value_from (ret, merged, &block);

		if (!value)
			continue;

		/*
		 * While the branch is still standing, so removePredecessor () still
		 * finds this block among merged's predecessors.
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
	 * A merge whose predecessors were all tail calls keeps none of them. Each
	 * branch is a ret again, so no block branches there any more and the
	 * merged block goes.
	 */
	for (llvm::BasicBlock *merged : merged_blocks) {
		if (llvm::pred_empty (merged) && merged != &f.getEntryBlock ())
			llvm::DeleteDeadBlock (merged);
	}

	return llvm::PreservedAnalyses::none ();
}

} // namespace mono

/**
 * \file
 * \brief Giving FastISel a keep_alive () marker it does not drop.
 */

#include "lower-keepalive.hpp"

#include <llvm/ADT/SmallVector.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/InlineAsm.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Intrinsics.h>
#include <llvm/Support/ModRef.h>

using namespace llvm;

namespace mono {

PreservedAnalyses
LowerKeepAlivePass::run (Function &f, FunctionAnalysisManager &fam)
{
	SmallVector<CallInst *, 8> marks;

	for (Instruction &at : instructions (f))
		if (auto *call = dyn_cast<CallInst> (&at))
			if (call->getIntrinsicID () == Intrinsic::fake_use)
				marks.push_back (call);

	if (marks.empty ())
		return PreservedAnalyses::all ();

	for (CallInst *mark : marks) {
		IRBuilder<> b (mark);

		for (Value *arg : mark->args ()) {
			CallInst *asm_read = b.CreateCall (
				InlineAsm::get (FunctionType::get (b.getVoidTy (), { arg->getType () }, false),
			                        "", "r", /*hasSideEffects=*/true),
				{ arg });

			asm_read->setMemoryEffects (MemoryEffects::inaccessibleMemOnly ());
		}

		mark->eraseFromParent ();
	}

	PreservedAnalyses preserved;

	// Every read is written where its own mark stood, and no branch moves.
	preserved.preserveSet<CFGAnalyses> ();
	return preserved;
}

} // namespace mono

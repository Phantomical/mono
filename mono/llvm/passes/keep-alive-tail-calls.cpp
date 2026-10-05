#include "keep-alive-tail-calls.hpp"

#include <llvm/ADT/SmallVector.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Intrinsics.h>

using namespace llvm;

namespace mono {

PreservedAnalyses
KeepAliveTailCallsPass::run (Function &f, FunctionAnalysisManager &)
{
	bool marked = false;
	SmallVector<CallInst *, 8> tail_calls;

	for (Instruction &at : instructions (f)) {
		auto *call = dyn_cast<CallInst> (&at);

		if (call == nullptr)
			continue;
		if (call->getIntrinsicID () == Intrinsic::fake_use)
			marked = true;
		else if (call->getTailCallKind () == CallInst::TCK_Tail)
			tail_calls.push_back (call);
	}

	if (!marked || tail_calls.empty ())
		return PreservedAnalyses::all ();

	for (CallInst *call : tail_calls)
		call->setTailCallKind (CallInst::TCK_None);

	PreservedAnalyses preserved;

	preserved.preserveSet<CFGAnalyses> ();
	return preserved;
}

} // namespace mono

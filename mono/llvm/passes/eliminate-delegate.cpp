#include "eliminate-delegate.hpp"

#include "analysis/constant-values.hpp"
#include "builtins.hpp"

#include <llvm/Analysis/BlockFrequencyInfo.h>

using namespace llvm;

namespace mono {

PreservedAnalyses
EliminateDelegateInvokesPass::run (Function &f, FunctionAnalysisManager &fam)
{
	BlockFrequencyInfo &counts = fam.getResult<BlockFrequencyAnalysis> (f);
	const ConstantValues &values = fam.getResult<MonoMemoryValues> (f);

	return eliminate_delegate_invokes (f, counts, values) ? PreservedAnalyses::none ()
	                                                      : PreservedAnalyses::all ();
}

} // namespace mono

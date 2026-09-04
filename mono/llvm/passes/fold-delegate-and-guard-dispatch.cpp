#include "fold-delegate-and-guard-dispatch.hpp"

#include "analysis/constant-values.hpp"
#include "devirtualize.hpp"
#include "fold-delegate.hpp"

#include <llvm/Analysis/BlockFrequencyInfo.h>

using namespace llvm;

namespace mono {

PreservedAnalyses
FoldDelegateAndGuardDispatchPass::run (Function &f, FunctionAnalysisManager &fam)
{
	BlockFrequencyInfo &counts = fam.getResult<BlockFrequencyAnalysis> (f);
	const ConstantValues &values = fam.getResult<MonoMemoryValues> (f);

	bool changed = fold_delegate_invokes (f, counts, values);
	changed |= guard_dispatch_sites (f, counts, values);

	return changed ? PreservedAnalyses::none () : PreservedAnalyses::all ();
}

} // namespace mono

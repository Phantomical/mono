#include "eliminate-delegate-and-guard-dispatch.hpp"

#include "analysis/constant-values.hpp"
#include "devirtualize.hpp"
#include "eliminate-delegate.hpp"

#include <llvm/Analysis/BlockFrequencyInfo.h>

using namespace llvm;

namespace mono {

PreservedAnalyses
EliminateDelegateAndGuardDispatchPass::run (Function &f, FunctionAnalysisManager &fam)
{
	BlockFrequencyInfo &counts = fam.getResult<BlockFrequencyAnalysis> (f);
	const ConstantValues &values = fam.getResult<MonoMemoryValues> (f);

	bool changed = eliminate_delegate_invokes (f, counts, values);
	changed |= guard_dispatch_sites (f, counts, values);

	return changed ? PreservedAnalyses::none () : PreservedAnalyses::all ();
}

} // namespace mono

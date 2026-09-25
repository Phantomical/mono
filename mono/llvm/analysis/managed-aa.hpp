/**
 * \file
 * \brief Alias analysis for managed objects and statics blocks.
 */

#ifndef MONO_LLVM_ANALYSIS_MANAGED_AA_HPP
#define MONO_LLVM_ANALYSIS_MANAGED_AA_HPP

#include <llvm/Analysis/AliasAnalysis.h>
#include <llvm/IR/PassManager.h>

typedef struct _MonoClass MonoClass;

namespace mono {

class ManagedAAResult : public llvm::AAResultBase {
public:
	/// Stateless, so it outlives every change to the function.
	bool invalidate (llvm::Function &, const llvm::PreservedAnalyses &,
	                 llvm::FunctionAnalysisManager::Invalidator &)
	{
		return false;
	}

	llvm::AliasResult alias (const llvm::MemoryLocation &a, const llvm::MemoryLocation &b,
	                         llvm::AAQueryInfo &info, const llvm::Instruction *at);
};

class ManagedAA : public llvm::AnalysisInfoMixin<ManagedAA> {
	friend llvm::AnalysisInfoMixin<ManagedAA>;
	static llvm::AnalysisKey Key;

public:
	using Result = ManagedAAResult;

	Result run (llvm::Function &, llvm::FunctionAnalysisManager &) { return Result (); }
};

/// Whether no object can be an instance of both classes.
bool classes_share_no_instance (MonoClass *a, MonoClass *b);

} // namespace mono

#endif

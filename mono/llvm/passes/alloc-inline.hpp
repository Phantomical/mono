/**
 * \file
 * \brief Inlining marked allocator calls after lowering.
 */

#ifndef MONO_LLVM_PASSES_ALLOC_INLINE_HPP
#define MONO_LLVM_PASSES_ALLOC_INLINE_HPP

#include <llvm/IR/PassManager.h>

#include <utility>

namespace llvm {
class TargetMachine;
} // namespace llvm

namespace mono {

class OneFileFS;

/// Whether -mono-inline-allocator was passed.
bool allocation_inlining_enabled ();

/// Weighs and inlines calls whose callees carry alloc_wrapper_attribute.
class AllocationInlinerPass : public llvm::PassInfoMixin<AllocationInlinerPass> {
public:
	AllocationInlinerPass (llvm::TargetMachine &target, llvm::ModulePassManager materialize,
	                      llvm::FunctionPassManager simplify, OneFileFS &profile_fs)
	    : target_ (&target), materialize_ (std::move (materialize)),
	      simplify_ (std::move (simplify)), profile_fs_ (&profile_fs)
	{
	}

	llvm::PreservedAnalyses run (llvm::Module &m, llvm::ModuleAnalysisManager &mam);

private:
	llvm::TargetMachine *target_;
	llvm::ModulePassManager materialize_;
	llvm::FunctionPassManager simplify_;
	OneFileFS *profile_fs_;
};

} // namespace mono

#endif

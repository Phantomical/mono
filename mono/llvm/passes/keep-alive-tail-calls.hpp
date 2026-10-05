/**
 * \file
 * \brief Prevent tail calls from discarding values kept live by fake uses.
 */

#ifndef MONO_LLVM_PASSES_KEEP_ALIVE_TAIL_CALLS_HPP
#define MONO_LLVM_PASSES_KEEP_ALIVE_TAIL_CALLS_HPP

#include <llvm/IR/PassManager.h>

namespace mono {

/// Codegen can turn `call; fake.use; ret` into a jump and discard the frame
/// while the callee runs. Clear `tail` on calls in functions with fake uses.
/// `musttail` calls keep their marker. Tier 1 lowers fake uses to inline asm
/// reads, which already prevent this.
class KeepAliveTailCallsPass : public llvm::PassInfoMixin<KeepAliveTailCallsPass> {
public:
	llvm::PreservedAnalyses run (llvm::Function &f, llvm::FunctionAnalysisManager &fam);
};

} // namespace mono

#endif

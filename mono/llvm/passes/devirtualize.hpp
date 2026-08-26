/**
 * \file
 * \brief Answering a `mono.vtable.func` call whose operands are settled.
 *
 * The two operands of a dispatch site name the receiver's vtable and the slot.
 * Where the optimizer has made both constant - which the store at an allocation
 * is what produces - the entry the site reads is a method this compile can name,
 * and the call becomes a direct one.
 */

#ifndef MONO_LLVM_PASSES_DEVIRTUALIZE_HPP
#define MONO_LLVM_PASSES_DEVIRTUALIZE_HPP

#include <llvm/IR/PassManager.h>

namespace mono {

/// Replaces a `mono.vtable.func` call whose class and slot are settled with the
/// entry it stands for.
///
/// Runs at the peephole extension point, so it sits behind each round of the
/// simplification that makes a vtable operand constant and in front of the one
/// that reads the direct call it leaves.
///
/// What it needs of the running compile it reads from current_compile ()
/// (compile-state.hpp), and it asks mono for the rest. Outside a compile it
/// leaves every site alone.
class DevirtualizePass : public llvm::PassInfoMixin<DevirtualizePass> {
public:
	llvm::PreservedAnalyses run (llvm::Function &f, llvm::FunctionAnalysisManager &fam);
};

} // namespace mono

#endif

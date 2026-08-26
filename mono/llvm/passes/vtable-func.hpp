/**
 * \file
 * \brief The declaration a virtual call reads its callee through.
 *
 * A dispatch site written as arithmetic on a pointer says nothing a later pass
 * can act on. Written as a call it keeps the two facts that settle the callee -
 * the vtable and the slot - as operands, which is the form constant propagation
 * delivers once the receiver's class is known.
 */

#ifndef MONO_LLVM_PASSES_VTABLE_FUNC_HPP
#define MONO_LLVM_PASSES_VTABLE_FUNC_HPP

#include <llvm/ADT/StringRef.h>
#include <llvm/IR/PassManager.h>

namespace llvm {
class Function;
class Module;
} // namespace llvm

namespace mono {

/// `ptr @mono.vtable.func (ptr vtable, i32 index)` answers the entry in slot
/// index of vtable. index counts words into `MonoVTable.vtable`, so it is the
/// method's own vtable index. Only the translator writes a call to it.
constexpr llvm::StringRef vtable_func_name = "mono.vtable.func";

/// The declaration in m, created on first use and carrying its attributes.
llvm::Function *vtable_func_decl (llvm::Module &m);

/// Rewrites every `mono.vtable.func` call into the load it stands for, and
/// erases the declaration.
///
/// Codegen has no lowering for one, so both tiers run this behind everything
/// that reads the call.
class LowerVTableFuncPass : public llvm::PassInfoMixin<LowerVTableFuncPass> {
public:
	llvm::PreservedAnalyses run (llvm::Module &m, llvm::ModuleAnalysisManager &mam);
};

} // namespace mono

#endif

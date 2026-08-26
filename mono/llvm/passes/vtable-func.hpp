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

/// `ptr @mono.imt.func (ptr vtable, i32 slot, ptr key)` answers the entry in
/// interface method table slot of vtable. The table sits in the words before
/// the MonoVTable, and slot is the index into it that key hashes to.
///
/// The key is what the site passes the thunk in the IMT register, and it is
/// carried here as well because a slot is a hash bucket: several interface
/// methods reach one, so the slot alone does not say which method the site
/// asked for. The lowering drops it. Only the translator writes a call to this.
constexpr llvm::StringRef imt_func_name = "mono.imt.func";

/// `ptr @mono.vtable.gfunc (ptr vtable, i32 index, ptr key)` answers the entry
/// in slot index of vtable for a virtual generic method.
///
/// The slot itself can never hold one instantiation's code, so what stands
/// there is a trampoline that reads the asked-for method out of the IMT
/// register. key is that method, and it is what makes the site resolvable: the
/// slot alone serves every instantiation, while the class and the key together
/// name one. Written as a call rather than as the load so both stay operands
/// for DevirtualizePass to read.
constexpr llvm::StringRef vtable_gfunc_name = "mono.vtable.gfunc";

/// The declarations in m, created on first use and carrying their attributes.
llvm::Function *vtable_func_decl (llvm::Module &m);
llvm::Function *imt_func_decl (llvm::Module &m);
llvm::Function *vtable_gfunc_decl (llvm::Module &m);

/// Rewrites every `mono.vtable.func` and `mono.imt.func` call into the load it
/// stands for, and erases the declarations.
///
/// Codegen has no lowering for either, so both tiers run this behind everything
/// that reads the calls.
class LowerVTableFuncPass : public llvm::PassInfoMixin<LowerVTableFuncPass> {
public:
	llvm::PreservedAnalyses run (llvm::Module &m, llvm::ModuleAnalysisManager &mam);
};

} // namespace mono

#endif

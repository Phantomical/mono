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

namespace llvm {
class Function;
class LoadInst;
class Module;
class Value;
} // namespace llvm

namespace mono {

/// `ptr @mono.vtable.func (ptr vtable, i32 index)` returns the entry in slot
/// index of vtable. index counts words into `MonoVTable.vtable`, so it is the
/// method's own vtable index. Only the translator writes a call to it.
constexpr llvm::StringRef vtable_func_name = "mono.vtable.func";

/// `ptr @mono.imt.func (ptr vtable, i32 slot, ptr key)` returns the entry in
/// interface method table slot of vtable. The table sits in the words before
/// the MonoVTable, and slot is the index into it that key hashes to.
///
/// The key is what the site passes the thunk in the IMT register, and it is
/// carried here as well because a slot is a hash bucket: several interface
/// methods reach one, so the slot alone does not say which method the site
/// asked for. The lowering drops it. Only the translator writes a call to this.
constexpr llvm::StringRef imt_func_name = "mono.imt.func";

/// `ptr @mono.vtable.gfunc (ptr vtable, i32 index, ptr key)` returns the entry
/// in slot index of vtable for a virtual generic method.
///
/// The slot itself can never hold one instantiation's code, so what stands
/// there is a trampoline that reads the asked-for method out of the IMT
/// register. key is that method, and it is what makes the site resolvable: the
/// slot alone serves every instantiation, while the class and the key together
/// name one. Written as a call rather than as the load so both stay operands
/// for fold_dispatch_sites () to read.
constexpr llvm::StringRef vtable_gfunc_name = "mono.vtable.gfunc";

/// `ptr @mono.vtable.klass (ptr vtable)` returns the class the vtable stands
/// for, `ptr @mono.vtable.type (ptr vtable)` that class's `System.Type` object,
/// and `i8 @mono.vtable.rank (ptr vtable)` its rank.
///
/// Each is a call rather than the load it stands for so that the vtable stays
/// an operand, which is what `fold_vtable_fields ()` reads once the IR settles
/// which vtable a site names. Only the translator writes a call to one.
constexpr llvm::StringRef vtable_klass_name = "mono.vtable.klass";
constexpr llvm::StringRef vtable_type_name = "mono.vtable.type";
constexpr llvm::StringRef vtable_rank_name = "mono.vtable.rank";

/// The declarations named above, created in m on first use and carrying their
/// attributes.
llvm::Function *vtable_func_decl (llvm::Module &m);
llvm::Function *imt_func_decl (llvm::Module &m);
llvm::Function *vtable_gfunc_decl (llvm::Module &m);
llvm::Function *vtable_klass_decl (llvm::Module &m);
llvm::Function *vtable_type_decl (llvm::Module &m);
llvm::Function *vtable_rank_decl (llvm::Module &m);

/// Rewrites every call to the declarations named above into the load it stands
/// for, erases the declarations, and says whether it changed anything.
bool lower_vtable_reads (llvm::Module &m);

/*
 * A read off an object is an ordinary load, where each read off a vtable above
 * is a declaration. The word it reads is one the module itself writes, at the
 * store `emit_object_alloc ()` puts behind an allocation. A load keeps that
 * store live for the memory passes and lets LLVM forward it, which answers a
 * fresh allocation before any pass here runs.
 */

/// Marks \p load as the read of an object's vtable word.
///
/// `!invariant.load` says the word holds one value wherever it can be read.
/// SGen writes a forwarding pointer over it when it moves an object. The stack
/// scan is conservative here, so an object a compiled frame holds is pinned
/// rather than moved.
///
/// That metadata grants no dereferenceability, so the read stays under the null
/// check on the object. `fold_object_vtables ()` needs that check to dominate
/// the read before it can take a sealed slot's declared class for the class the
/// object is.
llvm::LoadInst *mark_object_vtable_read (llvm::LoadInst *load);

/// The object whose vtable \p v reads, or null where \p v is not such a read.
///
/// What identifies a read is the declaration it feeds. So a read whose every use
/// is folded already is one this no longer answers for, and nothing is left to
/// fold there.
llvm::Value *object_vtable_read (llvm::Value *v);

} // namespace mono

#endif

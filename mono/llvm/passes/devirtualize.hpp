/**
 * \file
 * \brief Folding a `mono.vtable.func` call whose operands are settled.
 *
 * The two operands of a dispatch site name the receiver's vtable and the slot.
 * Where the optimizer has made both constant - which the store at an allocation
 * is what produces - the entry the site reads is a method this compile can name,
 * and the call becomes a direct one.
 */

#ifndef MONO_LLVM_PASSES_DEVIRTUALIZE_HPP
#define MONO_LLVM_PASSES_DEVIRTUALIZE_HPP

#include <cstdint>

namespace llvm {
class Function;
}

typedef struct _MonoClass MonoClass;
typedef struct _MonoMethod MonoMethod;

namespace mono {

/// The method in slot \p index of \p klass's vtable that a caller can name
/// directly, or null where it cannot name what stands there.
///
/// Null covers a slot with no method, one whose method is abstract, generic or
/// implemented outside IL, and one whose entry needs a context. A synchronized
/// method comes back as its wrapper, which is what the runtime puts in the slot.
MonoMethod *slot_target (MonoClass *klass, int32_t index);

/// Replaces each dispatch site in \p f whose class and slot are settled with
/// the entry it stands for. Says whether it changed anything.
///
/// What it needs of the running compile it reads from current_compile ()
/// (compile-state.hpp), and it asks mono for the rest. Outside a compile it
/// leaves every site alone.
bool fold_dispatch_sites (llvm::Function &f);

} // namespace mono

#endif

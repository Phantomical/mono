/**
 * \file
 * \brief Folding a vtable field read against the class its symbol names.
 *
 * A site reads the class, the `System.Type` object or the rank off a vtable it
 * is handed. Where the optimizer has settled that operand to a class's own
 * symbol, the value is one the translator already wrote beside that symbol.
 */

#ifndef MONO_LLVM_PASSES_FOLD_VTABLE_HPP
#define MONO_LLVM_PASSES_FOLD_VTABLE_HPP

namespace llvm {
class Function;
}

namespace mono {

class ConstantValues;

/// Replaces each object vtable read in \p f whose class the IR settles with
/// that class's own vtable symbol. Says whether it changed anything.
///
/// A read stands under the null check on its object, and the declaration is not
/// speculatable, so nothing moves one above that check. That is what lets a
/// sealed slot's declared class stand for the class the object is. The null
/// such a slot also admits cannot reach the read.
bool fold_object_vtables (llvm::Function &f, const ConstantValues &values);

/// Replaces each vtable field read in \p f whose vtable is a marked symbol with
/// the value that symbol carries. Says whether it changed anything.
bool fold_vtable_fields (llvm::Function &f, const ConstantValues &values);

} // namespace mono

#endif

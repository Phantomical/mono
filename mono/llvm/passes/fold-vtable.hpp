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

/// Replaces each vtable field read in \p f whose vtable is a marked symbol with
/// the value that symbol carries. Says whether it changed anything.
bool fold_vtable_fields (llvm::Function &f);

} // namespace mono

#endif

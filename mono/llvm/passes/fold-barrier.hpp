/**
 * \file
 * \brief Folding a write barrier against a destination the IR settles to the
 * frame.
 */

#ifndef MONO_LLVM_PASSES_FOLD_BARRIER_HPP
#define MONO_LLVM_PASSES_FOLD_BARRIER_HPP

namespace llvm {
class Function;
}

namespace mono {

/// Erases each write barrier in \p f whose destination the IR settles to an
/// alloca. Says whether it changed anything.
///
/// Both collectors scan a thread's frames conservatively, so a reference a frame
/// holds is found without a remembered-set entry.
bool fold_stack_barriers (llvm::Function &f);

} // namespace mono

#endif

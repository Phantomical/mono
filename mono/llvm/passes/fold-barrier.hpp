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

/// Rewrites each value copy in \p f that the IR settles as safe in the open into
/// a memcpy or a memmove with the cards behind it. Says whether it changed
/// anything.
///
/// A copy the optimizer can read is what lets SROA scalarize a value type and
/// what lets the dead-allocation walk erase the object behind it. The site the
/// translator wrote hides all of that inside one call, because an open copy is
/// wrong where a copied reference lands somewhere no conservative scan reaches.
/// `gc_value_copy_name` (`passes/gc-barrier.hpp`) states that rule, and this is
/// the fold that reads the IR against it.
bool open_value_copies (llvm::Function &f);

} // namespace mono

#endif

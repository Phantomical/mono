/**
 * \file
 * \brief What the interpreter entry thunk asks the engine for.
 */

#ifndef MONO_LLVM_INTERP_ENTRY_HPP
#define MONO_LLVM_INTERP_ENTRY_HPP

#include "arch/arch.hpp"

typedef struct _MonoMethod MonoMethod;

namespace mono {

/// Returns how to enter method in the calling thread's domain. The layout is
/// null when that domain is not prepared to interpret it.
arch::InterpEntryPoint interp_entry_for (MonoMethod *method);

} // namespace mono

#endif /* MONO_LLVM_INTERP_ENTRY_HPP */

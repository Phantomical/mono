/**
 * \file
 * \brief What the interpreter entry thunk asks the engine for.
 */

#ifndef MONO_LLVM_INTERP_ENTRY_HPP
#define MONO_LLVM_INTERP_ENTRY_HPP

#include "arch/arch.hpp"

typedef struct _MonoMethod MonoMethod;

namespace mono {

/// How a method is entered in the calling thread's domain, or null when nothing
/// there is prepared to interpret it.
const arch::InterpEntryPoint *interp_entry_for (MonoMethod *method);

} // namespace mono

#endif /* MONO_LLVM_INTERP_ENTRY_HPP */

/**
 * \file
 * \brief What the interpreter entry thunk asks the engine for.
 */

#ifndef MONO_LLVM_INTERP_ENTRY_HPP
#define MONO_LLVM_INTERP_ENTRY_HPP

#include "arch/arch.hpp"

#include <llvm/Support/Error.h>

namespace mono {

class MonoDomainMethod;

/// Returns how to enter the method whose thunk published carries.
///
/// The calling thread's domain is preferred over published's own, so a call
/// that crossed a domain runs where that domain's interpreter state is. The
/// error says why the interpreter refused the body.
llvm::Expected<arch::InterpEntryPoint> interp_entry_for (MonoDomainMethod *published);

} // namespace mono

#endif /* MONO_LLVM_INTERP_ENTRY_HPP */

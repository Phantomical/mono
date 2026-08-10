/**
 * \file
 * \brief What a method whose metadata would not load is compiled into.
 */

#ifndef MONO_LLVM_RUNTIME_THROWER_HPP
#define MONO_LLVM_RUNTIME_THROWER_HPP

#include "translate.hpp"

#include <llvm/ADT/STLFunctionalExtras.h>
#include <llvm/Support/Error.h>

#include <cstdint>

#include "mono/utils/mono-error.h"

typedef struct _MonoDomain MonoDomain;

typedef struct _MonoMethod MonoMethod;

namespace mono {

class MonoJit;

/// Note that a piece of code, and the record registered for it, belong to a
/// method - see TranslationTarget::remember.
using RememberFn = llvm::function_ref<void (const CompiledMethod &, MonoJitInfo *)>;

/// Whether a metadata failure is one ECMA-335 raises where the thing is used
/// rather than where it is named.
///
/// A method calling one that is missing gets to run until the call, and its
/// caller gets to catch what the call throws. Refusing to compile it instead
/// would raise at the wrong place, and to the wrong catch.
bool raised_where_used (uint16_t code);

/// Compile a body for a method that raises the given failure and nothing else.
/// Consumes the failure.
///
/// One body answers for every one of the method's doors: it reads no argument
/// and never returns, so whichever entry a caller came for, these few
/// instructions answer for it.
llvm::Expected<Compiled> compile_thrower (MonoJit &jit, MonoDomain *domain,
                                          MonoMethod *method, MonoError *failure,
                                          RememberFn remember);

/// Decide what a failed translation of a method means.
///
/// A metadata failure the program is owed as an exception becomes a stand-in
/// body that raises it; anything else is handed straight back, unchanged.
llvm::Expected<Compiled> recover (MonoJit &jit, MonoDomain *domain, MonoMethod *method,
                                  llvm::Error failure, RememberFn remember);

} // namespace mono

#endif

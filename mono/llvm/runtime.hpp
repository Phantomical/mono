/**
 * \file
 * \brief The entry point mono's C runtime compiles methods through.
 *
 * This is the whole surface the rest of mono sees of the LLVM-only backend: the
 * runtime asks for a method's code and gets back an address to call. Everything
 * behind it - translation, the ORC engine, stubs - stays on the C++ side.
 */

#ifndef MONO_LLVM_RUNTIME_HPP
#define MONO_LLVM_RUNTIME_HPP

#include <mono/utils/mono-publib.h>
#include <mono/utils/mono-error.h>

MONO_BEGIN_DECLS

typedef struct _MonoMethod MonoMethod;

/// Whether METHOD should be compiled by the LLVM-only backend.
///
/// MONO_LLVM_JIT in the environment decides: unset compiles nothing here, `1`
/// compiles everything, and any other value names the methods to take, matched
/// as a substring of the full method name. The last is how a single method is
/// put through this backend while the runtime around it still boots on the
/// classic JIT - which is the only way to run anything at all until wrappers
/// translate.
mono_bool mono_llvm_jit_wants_method (MonoMethod *method);

/// Compile METHOD and return the address to call it at.
///
/// The address is the method's stub, which is stable for the life of the process
/// however many times the method is later recompiled.
///
/// Returns NULL with ERROR set when the method cannot be compiled - IL this
/// backend has no translation for reports an ExecutionEngineException, and
/// malformed IL an InvalidProgramException, exactly as the caller would get from
/// any other failing compile.
void *mono_llvm_jit_compile_method (MonoMethod *method, MonoError *error);

MONO_END_DECLS

#endif

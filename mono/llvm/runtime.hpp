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
typedef struct _MonoDomain MonoDomain;

/// Compile METHOD for TARGET_DOMAIN and return the address to call it at.
///
/// The address is the method's stub, which is stable for the life of the process
/// however many times the method is later recompiled.
///
/// TARGET_DOMAIN is the domain whose linker the code goes into - the caller's
/// choice, because it carries mini's sharing policy: an icall wrapper compiles
/// for the root domain, since global caches hold its address for the life of
/// the process, and everything else for the domain that asked.
///
/// Returns NULL with ERROR set when the method cannot be compiled - IL this
/// backend has no translation for reports an ExecutionEngineException, and
/// malformed IL an InvalidProgramException, exactly as the caller would get from
/// any other failing compile.
void *mono_llvm_jit_compile_method (MonoMethod *method, MonoDomain *target_domain,
                                    MonoError *error);

/// Release everything the backend holds for DOMAIN: its code, its stubs, the
/// linker they live in. Called on the domain's way out, after the runtime has
/// proved nothing can execute in it any more; a domain the backend never
/// compiled for is a quiet no-op.
void mono_llvm_jit_free_domain (MonoDomain *domain);

/// Queue OPT for LLVM's own command-line option registry - the same options
/// `opt` and `llc` take, e.g. "-print-after-all". A leading dash is optional.
///
/// This is what `--llvm-opt=<opt>` on the command line does. The options take
/// effect when the backend starts, so they have to be set before the first
/// method is compiled; one LLVM rejects fails the backend's startup.
void mono_llvm_jit_add_option (const char *opt);

MONO_END_DECLS

#endif

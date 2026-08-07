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
typedef struct _MonoJitInfo MonoJitInfo;

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

/// Stop compiling in the background, waiting for whatever is already under way.
///
/// Called at the top of runtime shutdown, and it has to be: a background
/// compile reads metadata and allocates, and everything it touches is torn down
/// from there on - the domain's string table goes first, its assemblies not
/// long after. Nothing is queued again afterwards.
void mono_llvm_jit_stop_compiling (void);

/// Release everything the backend holds for DOMAIN: its code, its stubs, the
/// linker they live in. Called on the domain's way out, after the runtime has
/// proved nothing can execute in it any more; a domain the backend never
/// compiled for is a quiet no-op.
void mono_llvm_jit_free_domain (MonoDomain *domain);

/// Release everything the backend holds for METHOD: its code in every domain it
/// was compiled into, the jit-info records covering that code, and the caches
/// keyed by it.
///
/// Called when the runtime frees a dynamic method - the only kind it ever frees
/// - after it has proved nothing can be executing in the method any more. A
/// method this backend never compiled is a quiet no-op.
///
/// Freeing the method hands its MonoMethod back to the allocator, so this is
/// what keeps the next method to land on that address from being handed this
/// one's code.
void mono_llvm_jit_free_method (MonoMethod *method);

/// Where METHOD's body starts in DOMAIN, or NULL when this backend has not
/// compiled it there.
///
/// This is the address the method's own jit info covers - not the stub the
/// runtime calls it through, and not the interop thunk in front of it - so it
/// is what to hand mono_jit_info_table_find () to get back at that record. The
/// runtime has no method-keyed map of its own to answer this from: a method
/// reached as a callee is compiled without the runtime ever asking for it.
void *mono_llvm_jit_find_body (MonoDomain *domain, MonoMethod *method);

/// Call VISIT with the jit info of each live body this backend compiled METHOD
/// into in DOMAIN, oldest first.
///
/// A method keeps every body it has ever been compiled into: recompiling
/// redirects the stubs at the new one, which is what every later call reaches,
/// but a thread already running in an older body carries on there. Anything
/// that has to hold for the method wherever it is executing - a breakpoint - has
/// to be applied to all of them.
void mono_llvm_jit_foreach_body (MonoDomain *domain, MonoMethod *method,
                                 void (*visit) (MonoJitInfo *ji, void *user_data),
                                 void *user_data);

/// Where to enter METHOD when the receiver is still boxed - the address a call
/// off a value type's vtable or IMT is given, which steps the receiver past the
/// object header and carries on into the method exactly as its ordinary entry
/// would.
///
/// Returns NULL for a method this backend generated no such entry for: one not
/// implemented in IL is entered through code the backend never emitted, and
/// there is nothing to step the receiver in.
///
/// The address is a stub, so a slot filled from it keeps reaching the method
/// when a later tier replaces what stands behind it.
void *mono_llvm_jit_unbox_entry (MonoMethod *method);

/// Queue OPT for LLVM's own command-line option registry - the same options
/// `opt` and `llc` take, e.g. "-print-after-all". A leading dash is optional.
///
/// This is what `--llvm-opt=<opt>` on the command line does. The options take
/// effect when the backend starts, so they have to be set before the first
/// method is compiled; one LLVM rejects fails the backend's startup.
void mono_llvm_jit_add_option (const char *opt);

/// Enter the methods FILTER selects by interpreting them rather than by
/// compiling them: FILTER is matched as a substring of the printed name, and
/// an empty string takes every method the interpreter will accept. A method it
/// will not accept is compiled as usual.
///
/// This is what `--interp-tier0[=<filter>]` on the command line does. It needs
/// the interpreter to have been started, which that option also arranges.
void mono_llvm_jit_interpret_methods (const char *filter);

MONO_END_DECLS

#endif

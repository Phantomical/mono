/**
 * \file
 * \brief The interface mono's C runtime compiles methods through.
 *
 * This is the whole surface the rest of mono sees of the LLVM-only backend.
 * The runtime asks for a method's code and gets back an address to call.
 */

#ifndef MONO_LLVM_RUNTIME_HPP
#define MONO_LLVM_RUNTIME_HPP

#include <mono/utils/mono-publib.h>
#include <mono/utils/mono-error.h>

MONO_BEGIN_DECLS

typedef struct _MonoMethod MonoMethod;
typedef struct _MonoDomain MonoDomain;
typedef struct _MonoJitInfo MonoJitInfo;

/// Compiles a method and returns the address to call it at.
///
/// The code goes into the given domain's linker. The address is a stub, and
/// it stays the same for the life of the process however often the method is
/// recompiled. Callers can cache it.
///
/// Returns NULL and sets the error if the method cannot be compiled.
void *mono_llvm_jit_compile_method (MonoMethod *method, MonoDomain *domain, MonoError *error);

/// Stops background compiling and waits for whatever is already running.
///
/// Call this at the top of runtime shutdown. A background compile reads
/// metadata that shutdown then frees. Nothing queues after this returns.
void mono_llvm_jit_stop_compiling (void);

/// Stops background compiling for one domain and waits for its in-flight
/// compile.
///
/// Call this at the start of an unload, while the domain is still whole. By
/// the time the runtime frees it, a compile reading it reads freed memory.
///
/// Call this with no lock held. The compile it waits for can take the loader
/// lock.
void mono_llvm_jit_stop_compiling_for_domain (MonoDomain *domain);

/// Releases everything the backend holds for a domain.
///
/// Call this once the runtime has proved that nothing can execute in the
/// domain any more. A domain the backend never compiled for is a no-op.
void mono_llvm_jit_free_domain (MonoDomain *domain);

/// Releases everything the backend holds for a method, in every domain it was
/// compiled into.
///
/// Call this when the runtime frees a dynamic method, once it has proved that
/// nothing can execute in the method any more. A method the backend never
/// compiled is a no-op.
///
/// Freeing a method returns its MonoMethod to the allocator, so skipping this
/// hands the next method at that address this one's code.
void mono_llvm_jit_free_method (MonoMethod *method);

/// Returns where a method's body starts in a domain, or NULL if the backend
/// did not compile it there.
///
/// This is the address the method's own jit info covers, not the stub the
/// runtime calls through and not the interop thunk in front of it. Pass it to
/// mono_jit_info_table_find () to get that record.
void *mono_llvm_jit_find_body (MonoDomain *domain, MonoMethod *method);

/// Calls visit with the jit info of every live body of a method, oldest
/// first.
///
/// A method keeps the bodies it was compiled into before, and a thread can
/// still be running in one of them. Anything that has to hold wherever the
/// method executes, such as a breakpoint, belongs on all of them.
void mono_llvm_jit_foreach_body (MonoDomain *domain, MonoMethod *method,
                                 void (*visit) (MonoJitInfo *ji, void *user_data), void *user_data);

/// Returns where to enter a method with a boxed receiver, or NULL if the
/// backend generated no such entry.
///
/// This is what a call off a value type's vtable or IMT needs: it steps the
/// receiver past the object header before running the method. The address is
/// a stub, so a slot filled from it survives a later tier.
void *mono_llvm_jit_unbox_entry (MonoMethod *method);

/// Queues an option for LLVM's own command-line registry, the same options
/// `opt` and `llc` take. A leading dash is optional.
///
/// Set these before the first compile. An option LLVM rejects fails the
/// backend's startup.
void mono_llvm_jit_add_option (const char *opt);

/// Whether any method runs at tier 0, which is what decides whether the
/// interpreter starts at all.
mono_bool mono_llvm_jit_tier0_enabled (void);

/// How many calls a method takes at tier 0 before it should be asked for as
/// tier 1, or zero if it never promotes.
int32_t mono_llvm_jit_tier0_calls (MonoMethod *method);

/// Asks for a method to be compiled, replacing whatever tier runs it now.
///
/// Returns immediately. There is no way to wait for the compile or to ask
/// what became of it, and it can simply not happen: a domain on its way out
/// refuses the work and nothing retries it.
void mono_llvm_jit_request_promotion (MonoMethod *method, MonoDomain *domain);

/// Put METHOD's IL through the verifier, if a verifier mode was asked for.
///
/// The engine that enters a method first is the one that has to call this. A
/// method the interpreter reaches on its own is never asked for through
/// mono_llvm_jit_compile_method (), so nothing else gets the chance to decide
/// whether its body may run at all. A passing verdict is cached, so a method
/// both engines enter is verified once.
///
/// Returns FALSE with ERROR set to the exception the verdict names -
/// VerificationException, MethodAccessException and so on. Returns TRUE when no
/// verifier mode was asked for, which is the default.
mono_bool mono_llvm_jit_verify_method (MonoMethod *method, MonoError *error);

MONO_END_DECLS

#endif

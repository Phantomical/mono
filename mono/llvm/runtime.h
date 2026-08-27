/**
 * \file
 * \brief The interface mono's C runtime compiles methods through.
 *
 * The runtime asks for a method's code and gets back an address to call.
 */

#ifndef MONO_LLVM_RUNTIME_H
#define MONO_LLVM_RUNTIME_H

#include <mono/utils/mono-publib.h>
#include <mono/utils/mono-error.h>

MONO_BEGIN_DECLS

typedef struct _MonoMethod MonoMethod;
typedef struct _MonoDomain MonoDomain;
typedef struct _MonoJitInfo MonoJitInfo;
typedef struct _MonoMethodSignature MonoMethodSignature;

/// Call this from runtime startup, before any method can be entered. The
/// engine is otherwise built by whichever thread asks for the first compile or
/// promotion, which can be a mutator inside the interpreter.
///
/// The domains and their linkers are still built on demand, so this does not
/// consume the options queued by mono_llvm_jit_add_option ().
void mono_llvm_jit_init (void);

/// Fills in the jit icall table entries for the helpers this backend and its
/// interpreter entry provide. Call it from register_icalls ().
void mono_llvm_jit_register_icalls (void);

/// The code goes into the given domain's linker. The address is a stub, and
/// it stays the same for the life of the process however often the method is
/// recompiled. Callers can cache it.
///
/// Returns NULL and sets the error if the method cannot be compiled.
void *mono_llvm_jit_compile_method (MonoMethod *method, MonoDomain *domain, MonoError *error);

/// Returns the function pointer for this method, without compiling it.
///
/// This allocates a stub if not already created. If you need the method to be
/// compiled immediately use mono_llvm_jit_compile_method (), otherwise the
/// method will be compiled when it is first called.
///
/// Returns NULL and sets the error if the method cannot be published.
void *mono_llvm_jit_stub_for (MonoMethod *method, MonoDomain *domain, MonoError *error);

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

/// Calls visit with the jit info of every live body a method has in a
/// domain, oldest first.
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

/// How many calls a method takes at tier 0 before it is asked for as tier 1,
/// or zero if it never promotes.
int32_t mono_llvm_jit_tier0_calls (MonoMethod *method);

/// Asks for a method to be compiled at the given tier, replacing whatever tier
/// runs it now. The tier is a MonoTier.
///
/// Returns immediately. There is no way to wait for the compile or to ask what
/// became of it.
///
/// Returns FALSE when the request was refused, which a domain on its way out
/// and a runtime in shutdown both do. Nothing retries a refused request, so a
/// caller that spent a call count on it has to start counting again or the
/// method stays where it is for good.
mono_bool mono_llvm_jit_request_promotion (MonoMethod *method, MonoDomain *domain,
                                           uint8_t tier);

/// Makes the next call through a lazy re-entry trampoline compile its method
/// again, rather than continue into the code an earlier call left there.
///
/// Redirect the entry at the trampoline after this rather than before. A call
/// that arrives between the two reaches the code this takes away.
///
/// A record has a trampoline for each of the two steps a first call takes, and
/// either one can be holding an answer. Rearm both.
void mono_llvm_jit_rearm_trampoline (MonoDomain *domain, void *trampoline);

/// Compiles a method at a tier and points its entry at the result, on the
/// calling thread. The tier is a MonoTier.
///
/// This is what a test uses to reach a tier without waiting: the request above
/// is queued, so a test that spins on a call count races the compile worker.
/// This one has landed by the time it returns.
///
/// Returns FALSE when the compile was refused or failed, and the method then
/// stays at the tier already running it.
mono_bool mono_llvm_jit_promote_now (MonoMethod *method, MonoDomain *domain, uint8_t tier);

/// Whichever engine enters a method first is the one that has to call this. A
/// method the interpreter reaches on its own is never asked for through
/// mono_llvm_jit_compile_method (), so only an explicit call to this decides
/// whether its body can run. A passing verdict is cached, so a method both
/// engines enter is verified once.
///
/// Returns FALSE and sets the error to the exception the verdict names, such
/// as VerificationException or MethodAccessException. Returns TRUE when no
/// verifier mode was asked for, which is the default.
mono_bool mono_llvm_jit_verify_method (MonoMethod *method, MonoError *error);

/*
 * Calling a compiled body from a caller with no compiled code of its own.
 *
 * The interpreter reaches compiled code this way. The plan states where this
 * backend's convention puts each argument of one signature, and the call reads
 * it, so a signature costs a plan rather than a compiled wrapper.
 */

/// Plans how a call of \p sig is passed. Returns NULL for a signature that
/// passes or returns a value type by value, which this refuses rather than
/// state as a plan, and such a call has to be made another way.
///
/// The caller owns the plan. It holds no metadata and never changes, so a
/// caller that reaches several methods of one signature can share one.
void *mono_llvm_jit_dyn_call_prepare (MonoMethodSignature *sig);

/// Returns the bytes of scratch a call under \p plan needs. The scratch holds
/// no pointer the collector has to see, so a stack buffer is the right place
/// for it.
int mono_llvm_jit_dyn_call_frame_size (void *plan);

/// Calls \p target under \p plan, with each entry of \p args pointing at one
/// argument's value, the receiver first, and \p ret at room for the return.
///
/// The call runs on the calling thread, so the caller owes it an LMF: an
/// exception leaving \p target unwinds past this without reading its frame.
void mono_llvm_jit_dyn_call (void *plan, void *target, void **args, void *ret, void *frame);

MONO_END_DECLS

#endif

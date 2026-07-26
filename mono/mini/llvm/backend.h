/**
 * \file
 * backend.h - minimal extern "C" boundary for the mono/mini/llvm backend.
 *
 * This is the ONLY C-linkage header under mono/mini/llvm/. It re-declares,
 * with C linkage, the backend entry points that the rest of mono's C code
 * links against. During the step-2 build bring-up these are satisfied by
 * stub.cpp; step 3 replaces the stub with the real LLVM 18 translator/JIT.
 *
 * The canonical signatures (and the LLVMModuleFlags enum) live in mono/mini's
 * mini-llvm.h. We include it inside G_BEGIN_DECLS so the C++ stub sees those
 * declarations with C linkage - mini-llvm.h itself lacks the guard because it
 * is only ever consumed by mono's C sources today, where the linkage is
 * already C.
 */

#ifndef __MONO_MINI_LLVM_BACKEND_H__
#define __MONO_MINI_LLVM_BACKEND_H__

#include "mini.h"
#include "aot-runtime.h"

#include <llvm-c/Core.h>
#include <llvm-c/ExecutionEngine.h>

G_BEGIN_DECLS

#include "mini-llvm.h"

/*
 * ---- translator/runtime boundary ----
 * mono_llvm_set_unhandled_exception_handler is implemented by engine.cpp and
 * registered as a JIT icall from mini-runtime.c.
 */
void mono_llvm_set_unhandled_exception_handler (void);

/*
 * ---- engine (ORCv2 in-process JIT) boundary ----
 * Implemented by engine.cpp. These mirror the legacy mono/mini/llvm-jit.h
 * surface so step 3b's translator (mini-llvm.c) links against the new engine
 * unchanged. Today they are called only by that still-stubbed translator, so
 * they are unreferenced in the running mono binary (hence --llvm still fails
 * gracefully in stub.cpp's mono_llvm_emit_method).
 */
typedef void *MonoEERef;

void      mono_llvm_jit_init (void);
MonoEERef mono_llvm_create_ee (LLVMExecutionEngineRef *ee);
void      mono_llvm_dispose_ee (MonoEERef *mono_ee);
void      mono_llvm_optimize_method (LLVMValueRef method);
/*
 * Compile `method` and return its executable address.
 *
 * code_size_out (may be NULL) receives the machine-code size of the emitted
 * method, taken from the object's ELF symbol table (st_size) - authoritative and
 * per-function, unlike the code-section allocation (a module may hold several
 * functions) or an .eh_frame FDE (absent entirely for a nounwind leaf). It feeds
 * cfg->code_len, which sizes the method's MonoJitInfo.
 *
 * Under the forked LLVM that size came out of the mono-format EH table via
 * decode_llvm_eh_info(); stock LLVM emits no such table.
 *
 * It is returned through this call rather than a "last compile" accessor
 * deliberately: the size is discovered by the object layer on whichever thread
 * materializes the module, which stops being the calling thread as soon as the
 * JIT is given compile threads.
 *
 * dwarf_eh_frame_out / dwarf_eh_frame_size_out (both may be NULL) receive the
 * stock DWARF .eh_frame SECTION emitted for this module - not a per-function
 * FDE, so the caller must locate the FDE whose initial_location matches the
 * method (a module can hold more than one function). It is transcoded into
 * cfg->encoded_unwind_ops; see llvm/ehframe.cpp.
 *
 * stackmaps_out / stackmaps_size_out (both may be NULL) receive the loaded
 * `.llvm_stackmaps` SECTION, non-empty only for gshared methods (the translator
 * plants a llvm.experimental.stackmap recording the home slot of this/mrgctx).
 * Task #15 parses it into cfg->llvm_this_reg/offset so a stack walk can rebuild
 * the frame's generic context.
 *
 * mono_lsda_out / mono_lsda_size_out (both may be NULL) receive the loaded
 * `.mono_lsda` SECTION - mono's own target-neutral clause table (magic 'MLSD',
 * code-relative offsets), written by MonoLSDAStreamer from the EH-gather side
 * channel, non-empty only for an EH-bearing method whose catch clauses resolved.
 * C4/C6 parse it into the method's MonoJitExceptionInfo[]; the gate still declines
 * every EH method today, so this too is {NULL,0} for every method that currently
 * reaches here.
 */
gpointer  mono_llvm_compile_method (MonoEERef mono_ee, MonoCompile *cfg, LLVMValueRef method, int nvars, LLVMValueRef *callee_vars, gpointer *callee_addrs, gpointer *eh_frame, guint32 *code_size_out, gpointer *dwarf_eh_frame_out, guint32 *dwarf_eh_frame_size_out, gpointer *stackmaps_out, guint32 *stackmaps_size_out, gpointer *mono_lsda_out, guint32 *mono_lsda_size_out);

/*
 * Transcode the stock DWARF .eh_frame LLVM emits into mono's unwind ops.
 *
 * Finds the FDE describing [code_start, code_start + code_len) inside the
 * .eh_frame section at EH_FRAME and translates its CFI program, together with
 * the initial rules from its CIE, into a GSList of MonoUnwindOp*. The result is
 * ordered by code offset and contains only opcodes mono_unwind_ops_encode_full()
 * can encode and mono_unwind_frame() can execute. See llvm/ehframe.cpp for why
 * this transcodes rather than copying the CFI bytes.
 *
 * Returns TRUE and stores the list in *out_ops. Returns FALSE if the section is
 * malformed, no FDE matches, or the CFI uses something that cannot be
 * represented - in which case the caller must decline the method rather than
 * publish a frame with no unwind information.
 *
 * Exposed on this boundary (rather than kept internal to the module) so the
 * mono/unit-tests transcoder test can drive it with synthetic CIE/FDE buffers:
 * the restore, remember_state, restore_state and undefined paths cannot be
 * reached from managed code on x86-64.
 */
gboolean  mono_llvm_eh_frame_to_unwind_ops (guint8 *eh_frame, guint32 eh_frame_size, gpointer code_start, guint32 code_len, GSList **out_ops);

/*
 * Explicit runtime-helper registration (ORCv2 absoluteSymbols), replacing the
 * legacy engine's -rdynamic/process-symbol search. Step 3b registers mono's
 * icall helper targets through this before compiling methods that call them.
 */
void      mono_llvm_jit_register_symbol (const char *name, gpointer addr);

/*
 * Reverse lookup: the name ADDR was registered under via
 * mono_llvm_jit_register_symbol (), or NULL if ADDR has never been
 * registered. Used by the disassembler (helpers.c) to annotate tier-1 call
 * targets with a symbolic name instead of a bare address. The returned
 * pointer, if non-NULL, is stable for the life of the process and must not
 * be freed.
 */
const char *mono_llvm_jit_resolve_symbol_name (gpointer addr);

/*
 * Reclaim every JIT'd body compiled for DOMAIN: drop its symbols from the
 * ExecutionSession, deregister its .eh_frame with the host unwinder and UNMAP
 * its code and data. Returns the number of per-method JITDylibs removed.
 *
 * WHY THIS IS SAFE, AND WHY ONLY HERE. It is called from
 * mini_free_jit_domain_info (), mono_domain_free ()'s free_domain_hook. A few
 * dozen lines later that same function reaches
 * mono_mem_manager_free_singleton (), which calls
 * mono_code_manager_destroy (domain->memory_manager->code_mp) and thereby
 * unmaps every byte of CLASSIC JIT code the domain ever produced. So the
 * runtime already asserts, at this exact point and with no safepoint machinery,
 * that no thread can be executing this domain's code, that no reachable
 * MonoJitInfo, trampoline, vtable slot or delegate still targets it, and that
 * address reuse of those pages is acceptable. Reclaiming the tier-1 bodies
 * alongside the tier-0 bodies they replaced adds no new assumption: it is the
 * same domain, the same quiescence argument (mono_domain_try_unload () aborts
 * and waits, with an infinite timeout, for every thread holding an appdomain
 * ref) and the same set of consumers.
 *
 * Two things do NOT follow from that and are handled explicitly:
 *   - .eh_frame FDEs are registered in libgcc's PROCESS-GLOBAL registry, not in
 *     anything domain-scoped, so they must be deregistered before the pages go.
 *     ExecutionSession::removeJITDylibs () drives that ordering, and
 *     ~MonoJitMemoryManager asserts it held.
 *   - mono_dont_free_domains (the debugger's "keep domains alive" mode) makes
 *     mono_domain_free () return before the hook, so this is never reached and
 *     nothing is unmapped - matching the runtime's own choice to invalidate
 *     rather than free in that mode.
 *
 * Reclaiming per METHOD rather than per domain is NOT offered, and that is a
 * deliberate omission: outside a domain unload the runtime never establishes
 * that a compiled body is dead. mini_tiered_promote () orphans the tier-0 body
 * rather than freeing it, precisely because a published body can still be the
 * target of a patched call site or a live stack frame with nothing tracking it.
 */
guint32   mono_llvm_jit_release_domain (MonoDomain *domain);

/*
 * Tiered compilation (tiered.cpp). Tier 0 is the classic JIT. With a
 * call-count threshold set, a method is queued for tier 1 once its tier-0
 * entry count crosses that threshold and promoted by a dedicated background
 * compile thread, never on the thread that queued it. At threshold 0 there is
 * no queue and no background thread: mono_llvm_tiered_promote_sync () below
 * promotes the method synchronously, on whichever thread just published its
 * tier-0 body. All of these are no-ops unless MONO_TIERED is set.
 *
 * mini.c brackets mini_method_compile with _compile_begin/_compile_end, calls
 * _promote_sync after publishing a threshold-0 tier-0 body, and implements
 * mini_tiered_promote (declared in mini.h), which both that call and the
 * worker call back into.
 */
gboolean  mono_llvm_tiered_enabled (void);
void      mono_llvm_tiered_set_ready (void);
void      mono_llvm_tiered_compile_begin (void);
void      mono_llvm_tiered_compile_end (void);
/*
 * MONO_TIERED_CALL_THRESHOLD == 0 only: synchronously compile METHOD to tier 1
 * on the calling thread - the thread that just published its tier-0 body -
 * and return its tier-1 code pointer so the caller can start running it
 * immediately, or NULL to keep running the tier-0 body it already has (the
 * feature is off, promotion declined or failed, or - the recursion guard -
 * this thread is already inside another synchronous promotion). See the
 * fuller comment on the definition in tiered.cpp.
 */
gpointer  mono_llvm_tiered_promote_sync (MonoMethod *method, MonoDomain *domain, guint32 opt);
gboolean  mono_llvm_tiered_in_promotion (void);
void      mono_llvm_tiered_promote_begin (void);
void      mono_llvm_tiered_promote_end (void);
gboolean  mono_llvm_tiered_promotion_suspend (void);
void      mono_llvm_tiered_promotion_restore (gboolean old);
/*
 * Drop every queued and recorded method belonging to DOMAIN. Called from
 * mini_free_jit_domain_info (), the JIT's free_domain_hook, which runs partway
 * through mono_domain_free (): the domain's assemblies have already been closed,
 * so a MonoMethod from a dynamic assembly it owned is gone, while
 * domain->jit_code_hash and the MonoDomain itself are freed shortly afterwards.
 * Purging here means the background worker can never pick up any of them.
 */
void      mono_llvm_tiered_domain_unload (MonoDomain *domain);
/*
 * Ask the background compile worker (if one was ever started) to exit, and
 * wait - bounded, not indefinitely - for it to actually do so before
 * returning. Called once, from mini_cleanup (), during runtime teardown. The
 * worker is a background, DONT_MANAGE thread, so mono_thread_manage () never
 * waits for it; this function is what stands in for that, so that a compile
 * still in flight finishes (or the wait times out) before teardown starts
 * freeing domain and LLVM state it touches. On timeout it returns anyway -
 * see the fuller comment on the definition in tiered.cpp for the residual
 * risk that leaves.
 */
void      mono_llvm_tiered_shutdown (void);
/*
 * Hold the background compile worker off any tier-1 compile, waiting out one
 * already in flight, so the caller can mutate runtime state a compile reads.
 * The --regression harness brackets its between-opt-combination wipe of
 * domain->jit_trampoline_hash / domain->jit_code_hash with these; nothing else
 * replaces those tables underneath a running compile. Nests, and is a no-op
 * only when tiering is off. See tiered.cpp for the full contract.
 */
void      mono_llvm_tiered_quiesce (void);
void      mono_llvm_tiered_resume (void);

G_END_DECLS

#endif /* __MONO_MINI_LLVM_BACKEND_H__ */

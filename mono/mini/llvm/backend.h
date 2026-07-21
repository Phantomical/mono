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
 * cfg->encoded_unwind_ops; see llvm/ehframe.hpp.
 */
gpointer  mono_llvm_compile_method (MonoEERef mono_ee, MonoCompile *cfg, LLVMValueRef method, int nvars, LLVMValueRef *callee_vars, gpointer *callee_addrs, gpointer *eh_frame, guint32 *code_size_out, gpointer *dwarf_eh_frame_out, guint32 *dwarf_eh_frame_size_out);

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
void      mono_llvm_jit_register_symbol (const char *name, void *addr);

/*
 * Engine self-test. Builds hand-crafted LLVM modules, JITs them through the real
 * engine path, calls the results and checks them. Returns 0 on success, non-zero
 * on failure. Driven by mono/unit-tests/test-llvm-engine.c.
 */
int       mono_llvm_engine_run_selftest (void);

/*
 * Tiered compilation (tiered.cpp). Tier 0 is the classic JIT; a method is
 * queued for tier 1 on a successful tier-0 compile and promoted once the
 * compile nesting unwinds to zero, so LLVM codegen never runs on a deep
 * JIT nest. All of these are no-ops unless MONO_TIERED is set.
 *
 * mini.c brackets mini_method_compile with _compile_begin/_compile_end, calls
 * _enqueue after publishing a tier-0 body, and implements mini_tiered_promote
 * (declared in mini.h) which the drain calls back into.
 */
gboolean  mono_llvm_tiered_enabled (void);
void      mono_llvm_tiered_set_ready (void);
void      mono_llvm_tiered_compile_begin (void);
void      mono_llvm_tiered_compile_end (void);
void      mono_llvm_tiered_enqueue (MonoMethod *method, MonoDomain *domain, guint32 opt);
gboolean  mono_llvm_tiered_in_promotion (void);
void      mono_llvm_tiered_promote_begin (void);
void      mono_llvm_tiered_promote_end (void);
gboolean  mono_llvm_tiered_promotion_suspend (void);
void      mono_llvm_tiered_promotion_restore (gboolean old);

G_END_DECLS

#endif /* __MONO_MINI_LLVM_BACKEND_H__ */

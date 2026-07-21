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
 */
gpointer  mono_llvm_compile_method (MonoEERef mono_ee, MonoCompile *cfg, LLVMValueRef method, int nvars, LLVMValueRef *callee_vars, gpointer *callee_addrs, gpointer *eh_frame, guint32 *code_size_out);

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

G_END_DECLS

#endif /* __MONO_MINI_LLVM_BACKEND_H__ */

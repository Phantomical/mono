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

G_BEGIN_DECLS

#include "mini-llvm.h"

/*
 * Declared in llvm-jit.h, which pulls in the llvm-c headers. Redeclared here so
 * this boundary header stays free of any LLVM include.
 */
void mono_llvm_set_unhandled_exception_handler (void);

G_END_DECLS

#endif /* __MONO_MINI_LLVM_BACKEND_H__ */

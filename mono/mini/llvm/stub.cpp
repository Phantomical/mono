/**
 * \file
 * stub.cpp - placeholder mono/mini/llvm backend (step-2 build bring-up).
 *
 * Satisfies the linker for the mono_llvm_* entry points that mono's non-LLVM
 * code references, so the whole runtime links against unmodified system
 * LLVM 18. No IL->IR translation happens here yet: the compile-a-method entry
 * points abort with a clear message (step 3 ports the real translator).
 *
 * It is compiled with the LLVM 18 C++ flags (-fno-rtti, -std=c++17, ...) even
 * though it includes no LLVM headers, so its ABI already matches libLLVM-18
 * for when the real backend lands here.
 */

#include <config.h>
#include <glib.h>

#include "backend.h"

#define LLVM18_STUB_MSG \
	"mono/mini/llvm: the LLVM 18 backend is not implemented yet (step-2 build " \
	"stub). Run without --llvm; the step-3 translator port replaces this."

#define LLVM18_AOT_MSG \
	"mono/mini/llvm: AOT with LLVM 18 is not supported by the step-2 build stub."

extern "C" {

/* ---- lifecycle: safe no-ops (some of these run without --llvm) ---- */

void
mono_llvm_init (gboolean enable_jit)
{
	/*
	 * Reached only under --llvm (JIT) or AOT. Let init succeed so the failure
	 * surfaces at the real translator boundary (method emission), proving the
	 * whole plumbing is wired end to end.
	 */
}

void
mono_llvm_cleanup (void)
{
}

void
mono_llvm_free_domain_info (MonoDomain *domain)
{
	/* Called on every domain teardown, even without --llvm: must be a no-op. */
}

void
mono_llvm_set_unhandled_exception_handler (void)
{
	/*
	 * Registered as a JIT icall at startup whenever ENABLE_LLVM is defined;
	 * never actually invoked unless LLVM-compiled code runs.
	 */
}

MonoCPUFeatures
mono_llvm_get_cpu_features (void)
{
	/*
	 * Called during normal (non-llvm) JIT for SIMD feature detection, so it
	 * must return cleanly. Report no LLVM-derived features; the arch-specific
	 * detection path still applies on top of this.
	 */
	return (MonoCPUFeatures) 0;
}

/* ---- compile-a-method entry points: bail with a clear message ---- */

void
mono_llvm_check_method_supported (MonoCompile *cfg)
{
	g_error ("%s", LLVM18_STUB_MSG);
}

void
mono_llvm_create_vars (MonoCompile *cfg)
{
	g_error ("%s", LLVM18_STUB_MSG);
}

void
mono_llvm_emit_call (MonoCompile *cfg, MonoCallInst *call)
{
	g_error ("%s", LLVM18_STUB_MSG);
}

void
mono_llvm_emit_method (MonoCompile *cfg)
{
	g_error ("%s", LLVM18_STUB_MSG);
}

/* ---- AOT entry points: out of scope for this milestone ---- */

void
mono_llvm_create_aot_module (MonoAssembly *assembly, const char *global_prefix, int initial_got_size, LLVMModuleFlags flags)
{
	g_error ("%s", LLVM18_AOT_MSG);
}

void
mono_llvm_emit_aot_module (const char *filename, const char *cu_name)
{
	g_error ("%s", LLVM18_AOT_MSG);
}

void
mono_llvm_emit_aot_file_info (MonoAotFileInfo *info, gboolean has_jitted_code)
{
	g_error ("%s", LLVM18_AOT_MSG);
}

gpointer
mono_llvm_emit_aot_data (const char *symbol, guint8 *data, int data_len)
{
	g_error ("%s", LLVM18_AOT_MSG);
	return NULL;
}

gpointer
mono_llvm_emit_aot_data_aligned (const char *symbol, guint8 *data, int data_len, int align)
{
	g_error ("%s", LLVM18_AOT_MSG);
	return NULL;
}

void
mono_llvm_fixup_aot_module (void)
{
	g_error ("%s", LLVM18_AOT_MSG);
}

} /* extern "C" */

/**
 * \file
 */

#ifndef __MONO_MINI_WINDOWS_H__
#define __MONO_MINI_WINDOWS_H__

#include <config.h>
#include <glib.h>

#ifdef HOST_WIN32
#include "windows.h"
#include "mini.h"
#include "mono/utils/mono-context.h"

/* The LLVM backend is C++, and it publishes the unwind tables it emits
 * through the declarations below. */
MONO_BEGIN_DECLS

gboolean
mono_setup_thread_context(DWORD thread_id, MonoContext *mono_context);

typedef enum {
	MONO_WIN32_TLS_CALLBACK_TYPE_NONE,
	MONO_WIN32_TLS_CALLBACK_TYPE_DLL,
	MONO_WIN32_TLS_CALLBACK_TYPE_LIB
} MonoWin32TLSCallbackType;

gboolean
mono_win32_handle_tls_callback_type (MonoWin32TLSCallbackType);

BOOL
mono_win32_runtime_tls_callback (HMODULE module_handle, DWORD reason, LPVOID reserved, MonoWin32TLSCallbackType callback_type);

#ifdef MONO_ARCH_HAVE_UNWIND_TABLE
// Implies HOST_WIN32

typedef struct {
	SRWLOCK lock;
	PVOID handle;
	gsize begin_range;
	gsize end_range;
	PRUNTIME_FUNCTION rt_funcs;
	DWORD rt_funcs_current_count;
	DWORD rt_funcs_max_count;
} DynamicFunctionTableEntry;


#ifdef ENABLE_CHECKED_BUILD
#define ENABLE_CHECKED_BUILD_UNWINDINFO
#endif

PRUNTIME_FUNCTION
mono_arch_unwindinfo_insert_rt_func_in_table(const gpointer code, gsize code_size, const gpointer unwinddata);

guint
mono_arch_unwindinfo_init_method_unwind_info(gpointer cfg);

RUNTIME_FUNCTION
mono_arch_unwindinfo_init(gpointer code, gsize code_offset, gsize code_size, gsize begin_range, gsize end_range, const gpointer unwinddata);

#endif

MONO_END_DECLS

#endif /* HOST_WIN32 */
#endif /* __MONO_MINI_WINDOWS_H__ */

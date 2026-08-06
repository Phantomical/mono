/**
 * \file
 * The AOT loader's entry points.
 *
 * This runtime has no AOT compiler and loads no AOT images, so every one of
 * these answers "there is no AOT code here". The ones that abort are those a
 * caller only reaches once it already holds AOT code, which cannot happen.
 *
 * Licensed under the MIT license. See LICENSE file in the project root for full license information.
 */

#include "config.h"

#include <mono/metadata/class.h>
#include <mono/metadata/object.h>

#include "mini.h"
#include "aot-runtime.h"
#include "mini-runtime.h"

void
mono_aot_init (void)
{
}

void
mono_aot_cleanup (void)
{
}

guint32
mono_aot_find_method_index (MonoMethod *method)
{
	g_assert_not_reached ();
	return 0;
}

gboolean
mono_aot_init_llvm_method (gpointer aot_module, gpointer method_info, MonoClass *init_class, MonoError *error)
{
	g_assert_not_reached ();
	return FALSE;
}

gpointer
mono_aot_get_method (MonoDomain *domain,
					 MonoMethod *method, MonoError *error)
{
	error_init (error);
	return NULL;
}

gboolean
mono_aot_is_got_entry (guint8 *code, guint8 *addr)
{
	return FALSE;
}

gboolean
mono_aot_get_cached_class_info (MonoClass *klass, MonoCachedClassInfo *res)
{
	return FALSE;
}

gboolean
mono_aot_get_class_from_name (MonoImage *image, const char *name_space, const char *name, MonoClass **klass)
{
	return FALSE;
}

MonoJitInfo *
mono_aot_find_jit_info (MonoDomain *domain, MonoImage *image, gpointer addr)
{
	return NULL;
}

gpointer
mono_aot_get_method_from_token (MonoDomain *domain, MonoImage *image, guint32 token, MonoError *error)
{
	error_init (error);
	return NULL;
}

guint8*
mono_aot_get_plt_entry (host_mgreg_t *regs, guint8 *code)
{
	return NULL;
}

gpointer
mono_aot_plt_resolve (gpointer aot_module, host_mgreg_t *regs, guint8 *code, MonoError *error)
{
	return NULL;
}

void
mono_aot_patch_plt_entry (gpointer aot_module, guint8 *code, guint8 *plt_entry, gpointer *got, host_mgreg_t *regs, guint8 *addr)
{
}

gpointer
mono_aot_get_method_from_vt_slot (MonoDomain *domain, MonoVTable *vtable, int slot, MonoError *error)
{
	error_init (error);

	return NULL;
}

guint32
mono_aot_get_plt_info_offset (gpointer aot_module, guint8 *plt_entry, host_mgreg_t *regs, guint8 *code)
{
	g_assert_not_reached ();

	return 0;
}

gpointer
mono_aot_create_specific_trampoline (gpointer arg1, MonoTrampolineType tramp_type, MonoDomain *domain, guint32 *code_len)
{
	g_assert_not_reached ();
	return NULL;
}

gpointer
mono_aot_get_static_rgctx_trampoline (gpointer ctx, gpointer addr)
{
	g_assert_not_reached ();
	return NULL;
}

gpointer
mono_aot_get_trampoline_full (const char *name, MonoTrampInfo **out_tinfo)
{
	g_assert_not_reached ();
	return NULL;
}

gpointer
mono_aot_get_trampoline (const char *name)
{
	g_assert_not_reached ();
	return NULL;
}

gpointer
mono_aot_get_unbox_arbitrary_trampoline (gpointer addr)
{
	g_assert_not_reached ();
	return NULL;
}

gpointer
mono_aot_get_unbox_trampoline (MonoMethod *method, gpointer addr)
{
	g_assert_not_reached ();
	return NULL;
}

gpointer
mono_aot_get_lazy_fetch_trampoline (guint32 slot)
{
	g_assert_not_reached ();
	return NULL;
}

gpointer
mono_aot_get_imt_trampoline (MonoVTable *vtable, MonoDomain *domain, MonoIMTCheckItem **imt_entries, int count, gpointer fail_tramp)
{
	g_assert_not_reached ();
	return NULL;
}	

gpointer
mono_aot_get_gsharedvt_arg_trampoline (gpointer arg, gpointer addr)
{
	g_assert_not_reached ();
	return NULL;
}

#ifdef MONO_ARCH_HAVE_FTNPTR_ARG_TRAMPOLINE
gpointer
mono_aot_get_ftnptr_arg_trampoline (gpointer arg, gpointer addr)
{
	g_assert_not_reached ();
	return NULL;
}
#endif

void
mono_aot_set_make_unreadable (gboolean unreadable)
{
}

gboolean
mono_aot_is_pagefault (void *ptr)
{
	return FALSE;
}

void
mono_aot_handle_pagefault (void *ptr)
{
}

guint8*
mono_aot_get_unwind_info (MonoJitInfo *ji, guint32 *unwind_info_len)
{
	g_assert_not_reached ();
	return NULL;
}

GHashTable *
mono_aot_get_weak_field_indexes (MonoImage *image)
{
	return NULL;
}

MonoAotMethodFlags
mono_aot_get_method_flags (guint8 *code)
{
	return MONO_AOT_METHOD_FLAG_NONE;
}

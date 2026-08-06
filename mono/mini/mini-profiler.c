/*
 * Licensed to the .NET Foundation under one or more agreements.
 * The .NET Foundation licenses this file to you under the MIT license.
 */

#include <config.h>

#include <mono/metadata/abi-details.h>
#include <mono/metadata/mono-debug.h>

#include "interp/interp.h"
#include "mini.h"
#include "trace.h"

void
mini_profiler_context_enable (void)
{
	if (!mono_debug_enabled ())
		mono_debug_init (MONO_DEBUG_FORMAT_MONO);
}

static gpointer
memdup_with_type (gpointer data, MonoType *t)
{
	int dummy;

	return g_memdup (data, mono_type_size (t, &dummy));
}

static guint8 *
get_int_reg (MonoContext *ctx, guint32 reg)
{
	return (guint8 *)(gsize)mono_arch_context_get_int_reg (ctx, reg);
}

static gpointer
get_variable_buffer (MonoDebugMethodJitInfo *jit, MonoDebugVarInfo *var, MonoContext *ctx)
{
	guint32 flags = var->index & MONO_DEBUG_VAR_ADDRESS_MODE_FLAGS;
	guint32 reg = var->index & ~MONO_DEBUG_VAR_ADDRESS_MODE_FLAGS;

	switch (flags) {
	case MONO_DEBUG_VAR_ADDRESS_MODE_REGISTER: {
		/*
		 * This is kind of a special case: All other address modes ultimately
		 * produce an address to where the actual value is located, but this
		 * address mode gets us the value itself as an host_mgreg_t value.
		 */
		host_mgreg_t value = (host_mgreg_t) get_int_reg (ctx, reg);

		return memdup_with_type (&value, var->type);
	}
	case MONO_DEBUG_VAR_ADDRESS_MODE_REGOFFSET:
		return memdup_with_type (get_int_reg (ctx, reg) + (gint32) var->offset, var->type);
	case MONO_DEBUG_VAR_ADDRESS_MODE_REGOFFSET_INDIR:
	case MONO_DEBUG_VAR_ADDRESS_MODE_VTADDR:
		return memdup_with_type (*(guint8 **) (get_int_reg (ctx, reg) + (gint32) var->offset), var->type);
	case MONO_DEBUG_VAR_ADDRESS_MODE_GSHAREDVT_LOCAL: {
		guint32 idx = reg;

		MonoDebugVarInfo *info_var = jit->gsharedvt_info_var;

		flags = info_var->index & MONO_DEBUG_VAR_ADDRESS_MODE_FLAGS;
		reg = info_var->index & ~MONO_DEBUG_VAR_ADDRESS_MODE_FLAGS;

		MonoGSharedVtMethodRuntimeInfo *info;

		switch (flags) {
		case MONO_DEBUG_VAR_ADDRESS_MODE_REGISTER:
			info = (MonoGSharedVtMethodRuntimeInfo *) get_int_reg (ctx, reg);
			break;
		case MONO_DEBUG_VAR_ADDRESS_MODE_REGOFFSET:
			info = *(MonoGSharedVtMethodRuntimeInfo **) (get_int_reg (ctx, reg) + (gint32) info_var->offset);
			break;
		default:
			g_assert_not_reached ();
		}

		MonoDebugVarInfo *locals_var = jit->gsharedvt_locals_var;

		flags = locals_var->index & MONO_DEBUG_VAR_ADDRESS_MODE_FLAGS;
		reg = locals_var->index & ~MONO_DEBUG_VAR_ADDRESS_MODE_FLAGS;

		guint8 *locals;

		switch (flags) {
		case MONO_DEBUG_VAR_ADDRESS_MODE_REGISTER:
			locals = get_int_reg (ctx, reg);
			break;
		case MONO_DEBUG_VAR_ADDRESS_MODE_REGOFFSET:
			locals = *(guint8 **) (get_int_reg (ctx, reg) + (gint32) info_var->offset);
			break;
		default:
			g_assert_not_reached ();
		}

		return memdup_with_type (locals + (gsize) info->entries [idx], var->type);
	}
	default:
		g_assert_not_reached ();
		return NULL;
	}
}

gpointer
mini_profiler_context_get_this (MonoProfilerCallContext *ctx)
{
	if (!mono_method_signature_internal (ctx->method)->hasthis)
		return NULL;

	if (ctx->interp_frame)
		return memdup_with_type (mini_get_interp_callbacks ()->frame_get_this (ctx->interp_frame), m_class_get_this_arg (ctx->method->klass));
	else
		return memdup_with_type (ctx->args [0], m_class_get_this_arg (ctx->method->klass));
}

gpointer
mini_profiler_context_get_argument (MonoProfilerCallContext *ctx, guint32 pos)
{
	MonoMethodSignature *sig = mono_method_signature_internal (ctx->method);

	if (pos >= sig->param_count)
		return NULL;

	if (ctx->interp_frame)
		return memdup_with_type (mini_get_interp_callbacks ()->frame_get_arg (ctx->interp_frame, pos), sig->params [pos]);

	return memdup_with_type (ctx->args [sig->hasthis + pos], sig->params [pos]);
}

gpointer
mini_profiler_context_get_local (MonoProfilerCallContext *ctx, guint32 pos)
{
	ERROR_DECL (error);
	MonoMethodHeader *header = mono_method_get_header_checked (ctx->method, error);
	mono_error_assert_ok (error); // Must be a valid method at this point.

	if (pos >= header->num_locals) {
		mono_metadata_free_mh (header);
		return NULL;
	}

	MonoType *t = header->locals [pos];

	mono_metadata_free_mh (header);

	if (ctx->interp_frame)
		return memdup_with_type (mini_get_interp_callbacks ()->frame_get_local (ctx->interp_frame, pos), t);

	MonoDebugMethodJitInfo *info = mono_debug_find_method (ctx->method, mono_domain_get ());

	/*
	 * A method registered without variable location info has a line table and
	 * nothing else, so where its locals live is not something anyone knows.
	 */
	if (!info || !info->has_var_info || pos >= info->num_locals)
		return NULL;

	return get_variable_buffer (info, &info->locals [pos], &ctx->context);
}

gpointer
mini_profiler_context_get_result (MonoProfilerCallContext *ctx)
{
	MonoType *ret = mono_method_signature_internal (ctx->method)->ret;

	if (!ctx->return_value)
		return NULL;

	return memdup_with_type (ctx->return_value, ret);
}

void
mini_profiler_context_free_buffer (void *buffer)
{
	g_free (buffer);
}

/**
 * \file
 * Mini-specific debugging stuff.
 *
 * Author:
 *   Martin Baulig (martin@ximian.com)
 *
 * (C) 2003 Ximian, Inc.
 */

#include "mini.h"
#include "mini-runtime.h"
#include "jit.h"
#include "config.h"
#include <mono/metadata/verify.h>
#include <mono/metadata/mono-config.h>
#include <mono/metadata/mono-debug.h>
#include <mono/metadata/appdomain.h>
#include <mono/metadata/threads-types.h>

#include <mono/metadata/debug-internals.h>

typedef struct {
	guint32 index;
	MonoMethodDesc *desc;
} MiniDebugBreakpointInfo;

static void
print_var_info (MonoDebugVarInfo *info, int idx, const char *name, const char *type)
{
	switch (info->index & MONO_DEBUG_VAR_ADDRESS_MODE_FLAGS) {
	case MONO_DEBUG_VAR_ADDRESS_MODE_REGISTER:
		g_print ("%s %s (%d) in register %s\n", type, name, idx, mono_arch_regname (info->index & (~MONO_DEBUG_VAR_ADDRESS_MODE_FLAGS)));
		break;
	case MONO_DEBUG_VAR_ADDRESS_MODE_REGOFFSET:
		g_print ("%s %s (%d) in memory: base register %s + %d\n", type, name, idx, mono_arch_regname (info->index & (~MONO_DEBUG_VAR_ADDRESS_MODE_FLAGS)), info->offset);
		break;
	case MONO_DEBUG_VAR_ADDRESS_MODE_REGOFFSET_INDIR:
		g_print ("%s %s (%d) in indir memory: base register %s + %d\n", type, name, idx, mono_arch_regname (info->index & (~MONO_DEBUG_VAR_ADDRESS_MODE_FLAGS)), info->offset);
		break;
	case MONO_DEBUG_VAR_ADDRESS_MODE_GSHAREDVT_LOCAL:
		g_print ("%s %s (%d) gsharedvt local.\n", type, name, idx);
		break;
	case MONO_DEBUG_VAR_ADDRESS_MODE_VTADDR:
		g_print ("%s %s (%d) vt address: base register %s + %d\n", type, name, idx, mono_arch_regname (info->index & (~MONO_DEBUG_VAR_ADDRESS_MODE_FLAGS)), info->offset);
		break;
	case MONO_DEBUG_VAR_ADDRESS_MODE_TWO_REGISTERS:
	default:
		g_assert_not_reached ();
	}
}

/**
 * mono_debug_print_locals:
 *
 * Prints to stdout the information about the local variables in
 * a method (if \p only_arguments is false) or about the arguments.
 * The information includes the storage info (where the variable 
 * lives, in a register or in memory).
 * The method is found by looking up what method has been emitted at
 * the instruction address \p ip.
 * This is for use inside a debugger.
 */
void
mono_debug_print_vars (gpointer ip, gboolean only_arguments)
{
	MonoDomain *domain = mono_domain_get ();
	MonoJitInfo *ji = mono_jit_info_table_find (domain, ip);
	MonoDebugMethodJitInfo *jit;
	int i;

	if (!ji)
		return;

	jit = mono_debug_find_method (jinfo_get_method (ji), domain);
	if (!jit)
		return;

	if (only_arguments) {
		char **names;
		names = g_new (char *, jit->num_params);
		mono_method_get_param_names (jinfo_get_method (ji), (const char **) names);
		if (jit->this_var)
			print_var_info (jit->this_var, 0, "this", "Arg");
		for (i = 0; i < jit->num_params; ++i) {
			print_var_info (&jit->params [i], i, names [i]? names [i]: "unknown name", "Arg");
		}
		g_free (names);
	} else {
		for (i = 0; i < jit->num_locals; ++i) {
			print_var_info (&jit->locals [i], i, "", "Local");
		}
	}
	mono_debug_free_method_jit_info (jit);
}

/*
 * The old Debugger breakpoint interface.
 *
 * This interface is used to insert breakpoints on methods which are not yet JITed.
 * The debugging code keeps a list of all such breakpoints and automatically inserts the
 * breakpoint when the method is JITed.
 */

static GPtrArray *breakpoints;

static int
mono_debugger_insert_breakpoint_full (MonoMethodDesc *desc)
{
	static int last_breakpoint_id = 0;
	MiniDebugBreakpointInfo *info;

	info = g_new0 (MiniDebugBreakpointInfo, 1);
	info->desc = desc;
	info->index = ++last_breakpoint_id;

	if (!breakpoints)
		breakpoints = g_ptr_array_new ();

	g_ptr_array_add (breakpoints, info);

	return info->index;
}

/*FIXME This is part of the public API by accident, remove it from there when possible. */
int
mono_debugger_insert_breakpoint (const gchar *method_name, gboolean include_namespace)
{
	MonoMethodDesc *desc;

	desc = mono_method_desc_new (method_name, include_namespace);
	if (!desc)
		return 0;

	return mono_debugger_insert_breakpoint_full (desc);
}

/*FIXME This is part of the public API by accident, remove it from there when possible. */
int
mono_debugger_method_has_breakpoint (MonoMethod *method)
{
	int i;

	if (!breakpoints)
		return 0;

	for (i = 0; i < breakpoints->len; i++) {
		MiniDebugBreakpointInfo *info = (MiniDebugBreakpointInfo *)g_ptr_array_index (breakpoints, i);

		if (!mono_method_desc_full_match (info->desc, method))
			continue;

		return info->index;
	}

	return 0;
}

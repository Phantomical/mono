/**
 * \file
 * Mini-specific debugging stuff.
 *
 * Author:
 *   Martin Baulig (martin@ximian.com)
 *
 * (C) 2003 Ximian, Inc.
 */

#include "compile.h"
#include "mini-runtime.h"
#include "jit.h"
#include <mono/metadata/verify.h>
#include <mono/metadata/mono-config.h>
#include <mono/metadata/mono-debug.h>
#include <mono/metadata/appdomain.h>
#include <mono/metadata/threads-types.h>

#include <mono/metadata/debug-internals.h>

#include <mono/utils/valgrind.h>

typedef struct
{
	MonoDebugMethodJitInfo *jit;
	GArray *line_numbers;
	guint32 has_line_numbers;
	guint32 breakpoint_id;
} MiniDebugMethodInfo;

static void
record_line_number (MiniDebugMethodInfo *info, guint32 address, guint32 offset)
{
	MonoDebugLineNumberEntry lne;

	lne.native_offset = address;
	lne.il_offset = offset;

	g_array_append_val (info->line_numbers, lne);
}

void
mono_debug_init_method (MonoCompile *cfg, MonoBasicBlock *start_block, guint32 breakpoint_id)
{
	MiniDebugMethodInfo *info;

	if (!mono_debug_enabled ())
		return;

	info = g_new0 (MiniDebugMethodInfo, 1);
	info->breakpoint_id = breakpoint_id;

	cfg->debug_info = info;
}

void
mono_debug_open_method (MonoCompile *cfg)
{
	MiniDebugMethodInfo *info;
	MonoDebugMethodJitInfo *jit;
	MonoMethodHeader *header;

	info = (MiniDebugMethodInfo *) cfg->debug_info;
	if (!info)
		return;

	mono_class_init_internal (cfg->method->klass);

	header = cfg->header;
	g_assert (header);

	info->jit = jit = g_new0 (MonoDebugMethodJitInfo, 1);
	info->line_numbers = g_array_new (FALSE, TRUE, sizeof (MonoDebugLineNumberEntry));
	jit->num_locals = header->num_locals;
	jit->locals = g_new0 (MonoDebugVarInfo, jit->num_locals);
}

static void
write_variable (MonoInst *inst, MonoDebugVarInfo *var)
{
	var->type = inst->inst_vtype;

	if (inst->opcode == OP_REGVAR)
		var->index = inst->dreg | MONO_DEBUG_VAR_ADDRESS_MODE_REGISTER;
	else if (inst->flags & MONO_INST_IS_DEAD)
		var->index = MONO_DEBUG_VAR_ADDRESS_MODE_DEAD;
	else if (inst->opcode == OP_REGOFFSET) {
		/* the debug interface needs fixing to allow 0(%base) address */
		var->index = inst->inst_basereg | MONO_DEBUG_VAR_ADDRESS_MODE_REGOFFSET;
		var->offset = inst->inst_offset;
	} else if (inst->opcode == OP_GSHAREDVT_ARG_REGOFFSET) {
		var->index = inst->inst_basereg | MONO_DEBUG_VAR_ADDRESS_MODE_REGOFFSET_INDIR;
		var->offset = inst->inst_offset;
	} else if (inst->opcode == OP_GSHAREDVT_LOCAL) {
		var->index = inst->inst_imm | MONO_DEBUG_VAR_ADDRESS_MODE_GSHAREDVT_LOCAL;
	} else if (inst->opcode == OP_VTARG_ADDR) {
		MonoInst *vtaddr;

		vtaddr = inst->inst_left;
		g_assert (vtaddr->opcode == OP_REGOFFSET);
		var->offset = vtaddr->inst_offset;
		var->index  = vtaddr->inst_basereg | MONO_DEBUG_VAR_ADDRESS_MODE_VTADDR;
	} else {
		g_assert_not_reached ();
	}
}

/*
 * mono_debug_add_vg_method:
 *
 *  Register symbol information for the method with valgrind
 */
static void
mono_debug_add_vg_method (MonoMethod *method, MonoDebugMethodJitInfo *jit)
{
#ifdef VALGRIND_ADD_LINE_INFO
	ERROR_DECL (error);
	MonoMethodHeader *header;
	MonoDebugMethodInfo *minfo;
	int i;
	char *filename = NULL;
	guint32 address, line_number;
	const char *full_name;
	guint32 *addresses;
	guint32 *lines;

	if (!RUNNING_ON_VALGRIND)
		return;

	header = mono_method_get_header_checked (method, error);
	mono_error_assert_ok (error); /* FIXME don't swallow the error */

	full_name = mono_method_full_name (method, TRUE);

	addresses = g_new0 (guint32, header->code_size + 1);
	lines = g_new0 (guint32, header->code_size + 1);

	/*
	 * Very simple code to convert the addr->offset mappings that mono has
	 * into [addr-addr] ->line number mappings.
	 */

	minfo = mono_debug_lookup_method (method);
	if (minfo) {
		/* Create offset->line number mapping */
		for (i = 0; i < header->code_size; ++i) {
			MonoDebugSourceLocation *location;

			location = mono_debug_method_lookup_location (minfo, i);
			if (!location)
				continue;

			lines [i] = location.row;
			if (!filename)
				filename = location.source_file;

			mono_debug_free_source_location (location);
		}
	}

	/* Create address->offset mapping */
	for (i = 0; i < jit->num_line_numbers; ++i) {
		MonoDebugLineNumberEntry *lne = jit->line_numbers [i];

		g_assert (lne->offset <= header->code_size);

		if ((addresses [lne->offset] == 0) || (lne->address < addresses [lne->offset]))
			addresses [lne->offset] = lne->address;
	}
	/* Fill out missing addresses */
	address = 0;
	for (i = 0; i < header->code_size; ++i) {
		if (addresses [i] == 0)
			addresses [i] = address;
		else
			address = addresses [i];
	}

	address = 0;
	line_number = 0;
	i = 0;
	while (i < header->code_size) {
		if (lines [i] == line_number)
			i ++;
		else {
			if (line_number > 0) {
				if (addresses [i] - 1 >= address) {
					VALGRIND_ADD_LINE_INFO (jit->code_start + address, jit->code_start + addresses [i] - 1, filename, line_number);
				}
			}
			address = addresses [i];
			line_number = lines [i];
		}
	}

	if (line_number > 0) {
		VALGRIND_ADD_LINE_INFO (jit->code_start + address, jit->code_start + jit->code_size - 1, filename, line_number);
	}

	VALGRIND_ADD_SYMBOL (jit->code_start, jit->code_size, full_name);

	g_free (addresses);
	g_free (lines);
	mono_metadata_free_mh (header);
#endif /* VALGRIND_ADD_LINE_INFO */
}

void
mono_debug_close_method (MonoCompile *cfg)
{
	MiniDebugMethodInfo *info;
	MonoDebugMethodJitInfo *jit;
	MonoMethodHeader *header;
	MonoMethodSignature *sig;
	MonoMethod *method;
	int i;

	info = (MiniDebugMethodInfo *) cfg->debug_info;
	if (!info || !info->jit) {
		if (info)
			g_free (info);
		return;
	}

	method = cfg->method;
	header = cfg->header;
	sig = mono_method_signature_internal (method);

	jit = info->jit;
	jit->code_start = cfg->native_code;
	jit->epilogue_begin = cfg->epilog_begin;
	jit->code_size = cfg->code_len;
	jit->has_var_info = mini_debug_options.mdb_optimizations || MONO_CFG_PROFILE_CALL_CONTEXT (cfg);

	if (jit->epilogue_begin)
		   record_line_number (info, jit->epilogue_begin, header->code_size);

	if (jit->has_var_info) {
		jit->num_params = sig->param_count;
		jit->params = g_new0 (MonoDebugVarInfo, jit->num_params);

		for (i = 0; i < jit->num_locals; i++)
			write_variable (cfg->locals [i], &jit->locals [i]);

		if (sig->hasthis) {
			jit->this_var = g_new0 (MonoDebugVarInfo, 1);
			write_variable (cfg->args [0], jit->this_var);
		}

		for (i = 0; i < jit->num_params; i++)
			write_variable (cfg->args [i + sig->hasthis], &jit->params [i]);

		if (cfg->gsharedvt_info_var) {
			jit->gsharedvt_info_var = g_new0 (MonoDebugVarInfo, 1);
			jit->gsharedvt_locals_var = g_new0 (MonoDebugVarInfo, 1);
			write_variable (cfg->gsharedvt_info_var, jit->gsharedvt_info_var);
			write_variable (cfg->gsharedvt_locals_var, jit->gsharedvt_locals_var);
		}
	}

	jit->num_line_numbers = info->line_numbers->len;
	jit->line_numbers = g_new0 (MonoDebugLineNumberEntry, jit->num_line_numbers);

	for (i = 0; i < jit->num_line_numbers; i++)
		jit->line_numbers [i] = g_array_index (info->line_numbers, MonoDebugLineNumberEntry, i);

	mono_debug_add_method (cfg->method_to_register, jit, cfg->domain);

	mono_debug_add_vg_method (method, jit);

	mono_debug_free_method_jit_info (jit);
	mono_debug_free_method (cfg);
}

void
mono_debug_free_method (MonoCompile *cfg)
{
	MiniDebugMethodInfo *info;

	info = (MiniDebugMethodInfo *) cfg->debug_info;
	if (info) {
		if (info->line_numbers)
			g_array_free (info->line_numbers, TRUE);
		g_free (info);
		cfg->debug_info = NULL;
	}
}

void
mono_debug_record_line_number (MonoCompile *cfg, MonoInst *ins, guint32 address)
{
	MiniDebugMethodInfo *info;
	MonoMethodHeader *header;
	guint32 offset;

	info = (MiniDebugMethodInfo *) cfg->debug_info;
	if (!info || !info->jit || !ins->cil_code)
		return;

	header = cfg->header;
	g_assert (header);

	if ((ins->cil_code < header->code) ||
	    (ins->cil_code > header->code + header->code_size))
		return;

	offset = ins->cil_code - header->code;
	if (!info->has_line_numbers) {
		info->jit->prologue_end = address;
		info->has_line_numbers = TRUE;
	}

	record_line_number (info, address, offset);
}

void
mono_debug_open_block (MonoCompile *cfg, MonoBasicBlock *bb, guint32 address)
{
	MiniDebugMethodInfo *info;
	MonoMethodHeader *header;
	guint32 offset;

	info = (MiniDebugMethodInfo *) cfg->debug_info;
	if (!info || !info->jit || !bb->cil_code)
		return;

	header = cfg->header;
	g_assert (header);

	if ((bb->cil_code < header->code) ||
	    (bb->cil_code > header->code + header->code_size))
		return;

	offset = bb->cil_code - header->code;
	if (!info->has_line_numbers) {
		info->jit->prologue_end = address;
		info->has_line_numbers = TRUE;
	}

	record_line_number (info, address, offset);
}

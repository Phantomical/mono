/**
 * \file
 * The new Mono code generator.
 *
 * Authors:
 *   Paolo Molaro (lupus@ximian.com)
 *   Dietmar Maurer (dietmar@ximian.com)
 *
 * Copyright 2002-2003 Ximian, Inc.
 * Copyright 2003-2010 Novell, Inc.
 * Copyright 2011 Xamarin, Inc (http://www.xamarin.com)
 * Licensed under the MIT license. See LICENSE file in the project root for full license information.
 */

#include <config.h>
#include <string.h>

#include <mono/utils/mono-counters.h>
#include <mono/utils/mono-time.h>
#include <mono/utils/unlocked.h>

#include "mini.h"
#include "mini-runtime.h"
#include "llvm-runtime.h"
#include "trace.h"

MonoCallSpec *mono_jit_trace_calls;
MonoMethodDesc *mono_inject_async_exc_method;
int mono_inject_async_exc_pos;
MonoMethodDesc *mono_break_at_bb_method;
int mono_break_at_bb_bb_num;
gboolean mono_do_x86_stack_align = TRUE;

#define EMUL_HIT_SHIFT 3
#define EMUL_HIT_MASK ((1 << EMUL_HIT_SHIFT) - 1)
/* small hit bitmap cache */
static mono_byte emul_opcode_hit_cache [(OP_LAST>>EMUL_HIT_SHIFT) + 1] = {0};
static short emul_opcode_num = 0;
static short emul_opcode_alloced = 0;
static short *emul_opcode_opcodes;
static MonoJitICallInfo **emul_opcode_map;

MonoJitICallInfo *
mono_find_jit_opcode_emulation (int opcode)
{
	g_assert (opcode >= 0 && opcode <= OP_LAST);
	if (emul_opcode_hit_cache [opcode >> (EMUL_HIT_SHIFT + 3)] & (1 << (opcode & EMUL_HIT_MASK))) {
		int i;
		for (i = 0; i < emul_opcode_num; ++i) {
			if (emul_opcode_opcodes [i] == opcode)
				return emul_opcode_map [i];
		}
	}
	return NULL;
}

void
mini_register_opcode_emulation (int opcode, MonoJitICallInfo *info, const char *name, MonoMethodSignature *sig, gpointer func, const char *symbol, gboolean no_wrapper)
{
	g_assert (info);
	g_assert (!sig->hasthis);
	g_assert (sig->param_count < 3);

	mono_register_jit_icall_info (info, func, name, sig, no_wrapper, symbol);

	if (emul_opcode_num >= emul_opcode_alloced) {
		int incr = emul_opcode_alloced? emul_opcode_alloced/2: 16;
		emul_opcode_alloced += incr;
		emul_opcode_map = (MonoJitICallInfo **)g_realloc (emul_opcode_map, sizeof (emul_opcode_map [0]) * emul_opcode_alloced);
		emul_opcode_opcodes = (short *)g_realloc (emul_opcode_opcodes, sizeof (emul_opcode_opcodes [0]) * emul_opcode_alloced);
	}
	emul_opcode_map [emul_opcode_num] = info;
	emul_opcode_opcodes [emul_opcode_num] = opcode;
	emul_opcode_num++;
	emul_opcode_hit_cache [opcode >> (EMUL_HIT_SHIFT + 3)] |= (1 << (opcode & EMUL_HIT_MASK));
}

gint64 mono_time_track_start ()
{
	return mono_100ns_ticks ();
}

/*
 * mono_time_track_end:
 *
 *   Uses UnlockedAddDouble () to update \param time.
 */
void mono_time_track_end (gint64 *time, gint64 start)
{
	UnlockedAdd64 (time, mono_100ns_ticks () - start);
}

/*
 * mini_get_underlying_type:
 *
 *   Return the type the JIT will use during compilation.
 * Handles: byref, enums, native types, bool/char, ref types, generic sharing.
 * For gsharedvt types, it will return the original VAR/MVAR.
 */
MonoType*
mini_get_underlying_type (MonoType *type)
{
	return mini_type_get_underlying_type (type);
}

void
mini_jit_init (void)
{
	mono_counters_register ("Allocated seq points size", MONO_COUNTER_JIT | MONO_COUNTER_INT, &mono_jit_stats.allocated_seq_points_size);
}

void
mini_jit_cleanup (void)
{
	g_free (emul_opcode_map);
	g_free (emul_opcode_opcodes);
}

#if !defined(ENABLE_LLVM_RUNTIME) && !defined(ENABLE_LLVM)

void
mono_llvm_cpp_throw_exception (void)
{
	g_assert_not_reached ();
}

void
mono_llvm_cpp_catch_exception (MonoLLVMInvokeCallback cb, gpointer arg, gboolean *out_thrown)
{
	g_assert_not_reached ();
}

#endif

/*
 * mono_target_pagesize:
 *
 *   query pagesize used to determine if an implicit NRE can be used
 */
int
mono_target_pagesize (void)
{
	/* We could query the system's pagesize via mono_pagesize (), however there
	 * are pitfalls: sysconf (3) is called on some posix like systems, and per
	 * POSIX.1-2008 this function doesn't have to be async-safe. Since this
	 * function can be called from a signal handler, we simplify things by
	 * using 4k on all targets. Implicit null-checks with an offset larger than
	 * 4k are _very_ uncommon, so we don't mind emitting an explicit null-check
	 * for those cases.
	 */
	return 4 * 1024;
}

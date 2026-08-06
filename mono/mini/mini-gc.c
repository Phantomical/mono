/**
 * \file
 * GC interface for the mono JIT
 *
 * Author:
 *   Zoltan Varga (vargaz@gmail.com)
 *
 * Copyright 2009 Novell, Inc (http://www.novell.com)
 * Copyright 2011 Xamarin, Inc (http://www.xamarin.com)
 * Licensed under the MIT license. See LICENSE file in the project root for full license information.
 */

#include "config.h"
#include "mini-gc.h"
#include "mini-runtime.h"
#include <mono/metadata/gc-internals.h>

static gboolean
get_provenance (StackFrameInfo *frame, MonoContext *ctx, gpointer data)
{
	MonoJitInfo *ji = frame->ji;
	MonoMethod *method;
	if (!ji)
		return FALSE;
	method = jinfo_get_method (ji);
	if (method->wrapper_type != MONO_WRAPPER_NONE)
		return FALSE;
	*(gpointer *)data = method;
	return TRUE;
}

static gpointer
get_provenance_func (void)
{
	gpointer provenance = NULL;
	mono_walk_stack (get_provenance, MONO_UNWIND_DEFAULT, (gpointer)&provenance);
	return provenance;
}

void
mini_gc_init (void)
{
	MonoGCCallbacks cb;

	memset (&cb, 0, sizeof (cb));
	cb.get_provenance_func = get_provenance_func;
	if (mono_use_interpreter)
		cb.interp_mark_func = mini_get_interp_callbacks ()->mark_stack;
	mono_gc_set_gc_callbacks (&cb);
}

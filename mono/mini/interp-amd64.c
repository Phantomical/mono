/**
 * \file
 * The amd64 side of the interpreter's pinvoke path: moving arguments and
 * return values between an interpreter frame and a native CallContext, in
 * both directions.
 *
 * Authors:
 *   Paolo Molaro (lupus@ximian.com)
 *   Dietmar Maurer (dietmar@ximian.com)
 *   Patrik Torstensson
 *   Zoltan Varga (vargaz@gmail.com)
 *   Johan Lorensson (lateralusx.github@gmail.com)
 *
 * (C) 2003 Ximian, Inc.
 * Copyright 2003-2011 Novell, Inc (http://www.novell.com)
 * Copyright 2011 Xamarin, Inc (http://www.xamarin.com)
 * Licensed under the MIT license. See LICENSE file in the project root for full license information.
 */
#include "mini.h"
#include <string.h>

#include "mono/interp/interp.h"

#include "mini-amd64.h"
#include "mini-runtime.h"

static int
arg_need_temp (ArgInfo *ainfo)
{
	// Value types using one register doesn't need temp.
	if (ainfo->storage == ArgValuetypeInReg && ainfo->nregs > 1)
		return ainfo->nregs * sizeof (host_mgreg_t);
	return 0;
}

static gpointer
arg_get_storage (CallContext *ccontext, ArgInfo *ainfo)
{
	switch (ainfo->storage) {
		case ArgInIReg:
			return &ccontext->gregs [ainfo->reg];
		case ArgInFloatSSEReg:
		case ArgInDoubleSSEReg:
			return &ccontext->fregs [ainfo->reg];
		case ArgOnStack:
		case ArgValuetypeAddrOnStack:
			return ccontext->stack + ainfo->offset;
		case ArgValuetypeInReg:
			// Empty struct
			if (ainfo->nregs == 0)
				return NULL;
			// Value type using one register can be stored
			// directly in its context gregs/fregs slot.
			g_assert (ainfo->nregs == 1);
			switch (ainfo->pair_storage [0]) {
				case ArgInIReg:
					return &ccontext->gregs [ainfo->pair_regs [0]];
				case ArgInFloatSSEReg:
				case ArgInDoubleSSEReg:
					return &ccontext->fregs [ainfo->pair_regs [0]];
				default:
					g_assert_not_reached ();
			}
		case ArgValuetypeAddrInIReg:
			g_assert (ainfo->pair_storage [0] == ArgInIReg && ainfo->pair_storage [1] == ArgNone);
			return &ccontext->gregs [ainfo->pair_regs [0]];
		default:
			g_error ("Arg storage type not yet supported");
	}
}

static void
arg_get_val (CallContext *ccontext, ArgInfo *ainfo, gpointer dest)
{
	g_assert (arg_need_temp (ainfo));

	host_mgreg_t *dest_cast = (host_mgreg_t*)dest;
	/* Reconstruct the value type */
	for (int k = 0; k < ainfo->nregs; k++) {
		int storage_type = ainfo->pair_storage [k];
		int reg_storage = ainfo->pair_regs [k];
		switch (storage_type) {
			case ArgInIReg:
				*dest_cast = ccontext->gregs [reg_storage];
				break;
			case ArgInFloatSSEReg:
			case ArgInDoubleSSEReg:
				*(double*)dest_cast = ccontext->fregs [reg_storage];
				break;
			default:
				g_assert_not_reached ();
		}
		dest_cast++;
	}
}

static void
arg_set_val (CallContext *ccontext, ArgInfo *ainfo, gpointer src)
{
	g_assert (arg_need_temp (ainfo));

	host_mgreg_t *src_cast = (host_mgreg_t*)src;
	for (int k = 0; k < ainfo->nregs; k++) {
		int storage_type = ainfo->pair_storage [k];
		int reg_storage = ainfo->pair_regs [k];
		switch (storage_type) {
			case ArgInIReg:
				ccontext->gregs [reg_storage] = *src_cast;
				break;
			case ArgInFloatSSEReg:
			case ArgInDoubleSSEReg:
				ccontext->fregs [reg_storage] = *(double*)src_cast;
				break;
			default:
				g_assert_not_reached ();
		}
		src_cast++;
	}
}

void
mono_arch_set_native_call_context_args (CallContext *ccontext, gpointer frame, MonoMethodSignature *sig)
{
	CallInfo *cinfo = mono_arch_get_call_info (NULL, sig);
	const MonoEECallbacks *interp_cb = mini_get_interp_callbacks ();
	gpointer storage;
	ArgInfo *ainfo;

	memset (ccontext, 0, sizeof (CallContext));

	ccontext->stack_size = ALIGN_TO (cinfo->stack_usage, MONO_ARCH_FRAME_ALIGNMENT);
	if (ccontext->stack_size)
		ccontext->stack = (guint8*)g_calloc (1, ccontext->stack_size);

	if (sig->ret->type != MONO_TYPE_VOID) {
		ainfo = &cinfo->ret;
		if (ainfo->storage == ArgValuetypeAddrInIReg) {
			storage = interp_cb->frame_arg_to_storage ((MonoInterpFrameHandle)frame, sig, -1);
			ccontext->gregs [cinfo->ret.reg] = (host_mgreg_t)storage;
		}
	}

	g_assert (!sig->hasthis);

	for (int i = 0; i < sig->param_count; i++) {
		ainfo = &cinfo->args [i];

		if (ainfo->storage == ArgValuetypeAddrInIReg || ainfo->storage == ArgValuetypeAddrOnStack) {
			storage = arg_get_storage (ccontext, ainfo);
			*(gpointer *)storage = interp_cb->frame_arg_to_storage (frame, sig, i);
			continue;
		}

		int temp_size = arg_need_temp (ainfo);

		if (temp_size)
			storage = alloca (temp_size); // FIXME? alloca in a loop
		else
			storage = arg_get_storage (ccontext, ainfo);

		interp_cb->frame_arg_to_data ((MonoInterpFrameHandle)frame, sig, i, storage);
		if (temp_size)
			arg_set_val (ccontext, ainfo, storage);
	}

	g_free (cinfo);
}

void
mono_arch_set_native_call_context_ret (CallContext *ccontext, gpointer frame, MonoMethodSignature *sig, gpointer retp)
{
	const MonoEECallbacks *interp_cb;
	CallInfo *cinfo;
	gpointer storage;
	ArgInfo *ainfo;

	if (sig->ret->type == MONO_TYPE_VOID)
		return;

	interp_cb = mini_get_interp_callbacks ();
	cinfo = mono_arch_get_call_info (NULL, sig);
	ainfo = &cinfo->ret;

	if (retp) {
		g_assert (cinfo->ret.storage == ArgValuetypeAddrInIReg);
		interp_cb->frame_arg_to_data ((MonoInterpFrameHandle)frame, sig, -1, retp);
#ifdef TARGET_WIN32
		// Windows x64 ABI ainfo implementation includes info on how to return value type address.
		// back to caller.
		storage = arg_get_storage (ccontext, ainfo);
		*(gpointer *)storage = retp;
#endif
	} else {
		g_assert (cinfo->ret.storage != ArgValuetypeAddrInIReg);
		int temp_size = arg_need_temp (ainfo);

		if (temp_size)
			storage = alloca (temp_size);
		else
			storage = arg_get_storage (ccontext, ainfo);
		memset (ccontext, 0, sizeof (CallContext)); // FIXME
		interp_cb->frame_arg_to_data ((MonoInterpFrameHandle)frame, sig, -1, storage);
		if (temp_size)
			arg_set_val (ccontext, ainfo, storage);
	}

	g_free (cinfo);
}

gpointer
mono_arch_get_native_call_context_args (CallContext *ccontext, gpointer frame, MonoMethodSignature *sig)
{
	const MonoEECallbacks *interp_cb = mini_get_interp_callbacks ();
	CallInfo *cinfo = mono_arch_get_call_info (NULL, sig);
	gpointer storage;
	ArgInfo *ainfo;

	for (int i = 0; i < sig->param_count + sig->hasthis; i++) {
		ainfo = &cinfo->args [i];

		if (ainfo->storage == ArgValuetypeAddrInIReg || ainfo->storage == ArgValuetypeAddrOnStack) {
			storage = arg_get_storage (ccontext, ainfo);
			interp_cb->data_to_frame_arg ((MonoInterpFrameHandle)frame, sig, i, *(gpointer *)storage);
			continue;
		}

		int temp_size = arg_need_temp (ainfo);

		if (temp_size) {
			storage = alloca (temp_size); // FIXME? alloca in a loop
			arg_get_val (ccontext, ainfo, storage);
		} else {
			storage = arg_get_storage (ccontext, ainfo);
		}

		interp_cb->data_to_frame_arg ((MonoInterpFrameHandle)frame, sig, i, storage);
	}

	storage = NULL;
	if (sig->ret->type != MONO_TYPE_VOID) {
		ainfo = &cinfo->ret;
		if (ainfo->storage == ArgValuetypeAddrInIReg)
			storage = (gpointer) ccontext->gregs [cinfo->ret.reg];
	}
	g_free (cinfo);
	return storage;
}

void
mono_arch_get_native_call_context_ret (CallContext *ccontext, gpointer frame, MonoMethodSignature *sig)
{
	const MonoEECallbacks *interp_cb;
	CallInfo *cinfo;
	ArgInfo *ainfo;
	gpointer storage;

	/* No return value */
	if (sig->ret->type == MONO_TYPE_VOID)
		return;

	interp_cb = mini_get_interp_callbacks ();
	cinfo = mono_arch_get_call_info (NULL, sig);
	ainfo = &cinfo->ret;

	/* The return values were stored directly at address passed in reg */
	if (cinfo->ret.storage != ArgValuetypeAddrInIReg) {
		int temp_size = arg_need_temp (ainfo);

		if (temp_size) {
			storage = alloca (temp_size);
			arg_get_val (ccontext, ainfo, storage);
		} else {
			storage = arg_get_storage (ccontext, ainfo);
		}
		interp_cb->data_to_frame_arg ((MonoInterpFrameHandle)frame, sig, -1, storage);
	}

	g_free (cinfo);
}

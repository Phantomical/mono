/**
 * \file
 * \brief Calling out of the interpreter into native code.
 */

#include "config.h"

#include "callbacks.hpp"
#include "internals.hpp"
#include "frame.hpp"
#include "stackval.hpp"
#include "call.hpp"
#include "lmf.hpp"
#include "pinvoke.hpp"

#include <mono/metadata/marshal.h>
#include <mono/mini/aot-runtime.h>
#include <mono/mini/mini-runtime.h>

namespace mono::interp {

#ifndef MONO_ARCH_HAVE_INTERP_PINVOKE_TRAMP
static InterpMethodArguments *
build_args_from_sig (MonoMethodSignature *sig, InterpFrame *frame)
{
	InterpMethodArguments *margs = g_malloc0 (sizeof (InterpMethodArguments));

#ifdef TARGET_ARM
	g_assert (mono_arm_eabi_supported ());
	int i8_align = mono_arm_i8_align ();
#endif

#ifdef TARGET_WASM
	margs->sig = sig;
#endif

	if (sig->hasthis)
		margs->ilen++;

	for (int i = 0; i < sig->param_count; i++) {
		guint32 ptype = sig->params[i]->byref ? MONO_TYPE_PTR : sig->params[i]->type;
		switch (ptype) {
		case MONO_TYPE_BOOLEAN:
		case MONO_TYPE_CHAR:
		case MONO_TYPE_I1:
		case MONO_TYPE_U1:
		case MONO_TYPE_I2:
		case MONO_TYPE_U2:
		case MONO_TYPE_I4:
		case MONO_TYPE_U4:
		case MONO_TYPE_I:
		case MONO_TYPE_U:
		case MONO_TYPE_PTR:
		case MONO_TYPE_FNPTR:
		case MONO_TYPE_SZARRAY:
		case MONO_TYPE_CLASS:
		case MONO_TYPE_OBJECT:
		case MONO_TYPE_STRING:
		case MONO_TYPE_VALUETYPE:
		case MONO_TYPE_GENERICINST:
#if SIZEOF_VOID_P == 8
		case MONO_TYPE_I8:
		case MONO_TYPE_U8:
#endif
			margs->ilen++;
			break;
#if SIZEOF_VOID_P == 4
		case MONO_TYPE_I8:
		case MONO_TYPE_U8:
#ifdef TARGET_ARM
			/* pairs begin at even registers */
			if (i8_align == 8 && margs->ilen & 1)
				margs->ilen++;
#endif
			margs->ilen += 2;
			break;
#endif
		case MONO_TYPE_R4:
		case MONO_TYPE_R8:
			margs->flen++;
			break;
		default:
			g_error ("build_args_from_sig: not implemented yet (1): 0x%x\n", ptype);
		}
	}

	if (margs->ilen > 0)
		margs->iargs = g_malloc0 (sizeof (gpointer) * margs->ilen);

	if (margs->flen > 0)
		margs->fargs = g_malloc0 (sizeof (double) * margs->flen);

	if (margs->ilen > INTERP_ICALL_TRAMP_IARGS)
		g_error ("build_args_from_sig: TODO, allocate gregs: %d\n", margs->ilen);

	if (margs->flen > INTERP_ICALL_TRAMP_FARGS)
		g_error ("build_args_from_sig: TODO, allocate fregs: %d\n", margs->flen);

	size_t int_i = 0;
	size_t int_f = 0;

	if (sig->hasthis) {
		margs->iargs[0] = frame->stack[0].data.p;
		int_i++;
		g_error ("FIXME if hasthis, we incorrectly access the args below");
	}

	for (int i = 0; i < sig->param_count; i++) {
		guint32 offset = get_arg_offset (frame->imethod, sig, i);
		stackval *sp_arg = STACK_ADD_BYTES (frame->stack, offset);
		MonoType *type = sig->params[i];
		guint32 ptype;
retry:
		ptype = type->byref ? MONO_TYPE_PTR : type->type;
		switch (ptype) {
		case MONO_TYPE_BOOLEAN:
		case MONO_TYPE_CHAR:
		case MONO_TYPE_I1:
		case MONO_TYPE_U1:
		case MONO_TYPE_I2:
		case MONO_TYPE_U2:
		case MONO_TYPE_I4:
		case MONO_TYPE_U4:
		case MONO_TYPE_I:
		case MONO_TYPE_U:
		case MONO_TYPE_PTR:
		case MONO_TYPE_FNPTR:
		case MONO_TYPE_SZARRAY:
		case MONO_TYPE_CLASS:
		case MONO_TYPE_OBJECT:
		case MONO_TYPE_STRING:
#if SIZEOF_VOID_P == 8
		case MONO_TYPE_I8:
		case MONO_TYPE_U8:
#endif
			margs->iargs[int_i] = sp_arg->data.p;
			int_i++;
			break;
		case MONO_TYPE_VALUETYPE:
			if (m_class_is_enumtype (type->data.klass)) {
				type = mono_class_enum_basetype_internal (type->data.klass);
				goto retry;
			}
			margs->iargs[int_i] = sp_arg;
			int_i++;
			break;
		case MONO_TYPE_GENERICINST: {
			MonoClass *container_class = type->data.generic_class->container_class;
			type = m_class_get_byval_arg (container_class);
			goto retry;
		}
#if SIZEOF_VOID_P == 4
		case MONO_TYPE_I8:
		case MONO_TYPE_U8: {
#ifdef TARGET_ARM
			/* pairs begin at even registers */
			if (i8_align == 8 && int_i & 1)
				int_i++;
#endif
			margs->iargs[int_i] = (gpointer) sp_arg->data.pair.lo;
			int_i++;
			margs->iargs[int_i] = (gpointer) sp_arg->data.pair.hi;
			int_i++;
			break;
		}
#endif
		case MONO_TYPE_R4:
		case MONO_TYPE_R8:
			if (ptype == MONO_TYPE_R4)
				*static_cast<float *> (&(margs->fargs[int_f])) = sp_arg->data.f_r4;
			else
				margs->fargs[int_f] = sp_arg->data.f;
			int_f++;
			break;
		default:
			g_error ("build_args_from_sig: not implemented yet (2): 0x%x\n", ptype);
		}
	}

	switch (sig->ret->type) {
	case MONO_TYPE_BOOLEAN:
	case MONO_TYPE_CHAR:
	case MONO_TYPE_I1:
	case MONO_TYPE_U1:
	case MONO_TYPE_I2:
	case MONO_TYPE_U2:
	case MONO_TYPE_I4:
	case MONO_TYPE_U4:
	case MONO_TYPE_I:
	case MONO_TYPE_U:
	case MONO_TYPE_PTR:
	case MONO_TYPE_FNPTR:
	case MONO_TYPE_SZARRAY:
	case MONO_TYPE_CLASS:
	case MONO_TYPE_OBJECT:
	case MONO_TYPE_STRING:
	case MONO_TYPE_I8:
	case MONO_TYPE_U8:
	case MONO_TYPE_VALUETYPE:
	case MONO_TYPE_GENERICINST:
		margs->retval = &frame->stack->data.p;
		margs->is_float_ret = 0;
		break;
	case MONO_TYPE_R4:
	case MONO_TYPE_R8:
		margs->retval = &frame->stack->data.p;
		margs->is_float_ret = 1;
		break;
	case MONO_TYPE_VOID:
		margs->retval = NULL;
		break;
	default:
		g_error ("build_args_from_sig: ret type not implemented yet: 0x%x\n", sig->ret->type);
	}

	return margs;
}
#endif

static MonoPIFunc
get_interp_to_native_trampoline (void)
{
	static MonoPIFunc trampoline = NULL;

	if (!trampoline) {
		if (mono_ee_features.use_aot_trampolines) {
			trampoline = (MonoPIFunc) mono_aot_get_trampoline ("interp_to_native_trampoline");
		} else {
			MonoTrampInfo *info;
			trampoline = (MonoPIFunc) mono_arch_get_interp_to_native_trampoline (&info);
			mono_tramp_info_register (info, NULL);
		}
		mono_memory_barrier ();
	}
	return trampoline;
}

void
interp_to_native_trampoline (gpointer addr, gpointer ccontext)
{
	get_interp_to_native_trampoline () (addr, ccontext);
}

MONO_NO_OPTIMIZATION MONO_NEVER_INLINE gpointer
ves_pinvoke_method (InterpMethod *imethod, MonoMethodSignature *sig, MonoFuncV addr,
                    ThreadContext *context, InterpFrame *parent_frame, stackval *sp,
                    gboolean save_last_error, gpointer *cache)
{
	InterpFrame frame = {0};
	frame.parent = parent_frame;
	frame.imethod = imethod;
	frame.stack = sp;
	frame_stamp_ordinal (context, &frame);
	/* A pinvoke reached by calli from outside a managed-to-native wrapper has
	 * no method behind it -- the xdomain-invoke wrappers do this -- and the
	 * frame then has nothing whose code it could root. */
	if (imethod)
		frame_root_code_owner (&frame);

	MonoLMFExt ext;
	gpointer args;

	/*
	 * When there's a calli in a pinvoke wrapper, we're in GC Safe mode.
	 * When we're called for some other calli, we may be in GC Unsafe mode.
	 *
	 * On any code path where we call anything other than the entry_func,
	 * we need to switch back to GC Unsafe before calling the runtime.
	 */
	MONO_REQ_GC_NEUTRAL_MODE;

#ifdef HOST_WASM
	/*
	 * Use a per-signature entry function.
	 * Cache it in imethod->data_items.
	 * This is GC safe.
	 */
	MonoPIFunc entry_func = *cache;
	if (!entry_func) {
		entry_func = (MonoPIFunc) mono_wasm_get_interp_to_native_trampoline (sig);
		mono_memory_barrier ();
		*cache = entry_func;
	}
#else
	static MonoPIFunc entry_func = NULL;
	if (!entry_func) {
		MONO_ENTER_GC_UNSAFE;
#ifdef MONO_ARCH_HAS_NO_PROPER_MONOCTX
		ERROR_DECL (error);
		entry_func = (MonoPIFunc) mono_jit_compile_method_jit_only (
			mini_get_interp_lmf_wrapper ("mono_interp_to_native_trampoline",
		                                 (gpointer) mono_interp_to_native_trampoline),
			error);
		mono_error_assert_ok (error);
#else
		entry_func = get_interp_to_native_trampoline ();
#endif
		mono_memory_barrier ();
		MONO_EXIT_GC_UNSAFE;
	}
#endif

#ifdef ENABLE_NETCORE
	if (save_last_error) {
		mono_marshal_clear_last_error ();
	}
#endif

#ifdef MONO_ARCH_HAVE_INTERP_PINVOKE_TRAMP
	CallContext ccontext;
	MONO_ENTER_GC_UNSAFE;
	mono_arch_set_native_call_context_args (&ccontext, &frame, sig);
	MONO_EXIT_GC_UNSAFE;
	args = &ccontext;
#else
	InterpMethodArguments *margs = build_args_from_sig (sig, &frame);
	args = margs;
#endif

	INTERP_PUSH_LMF_WITH_CTX (&frame, ext, exit_pinvoke);
	entry_func ((gpointer) addr, args);
	if (save_last_error)
		mono_marshal_set_last_error ();
	interp_pop_lmf (&ext);

#ifdef MONO_ARCH_HAVE_INTERP_PINVOKE_TRAMP
	if (!context->has_resume_state) {
		MONO_ENTER_GC_UNSAFE;
		mono_arch_get_native_call_context_ret (&ccontext, &frame, sig);
		MONO_EXIT_GC_UNSAFE;
	}

	g_free (ccontext.stack);
#else
	// Only the vt address has been returned, we need to copy the entire content on interp stack
	if (!context->has_resume_state && MONO_TYPE_ISSTRUCT (sig->ret))
		stackval_from_data (sig->ret, frame.stack, static_cast<char *> (frame.stack->data.p),
		                    sig->pinvoke);

	g_free (margs->iargs);
	g_free (margs->fargs);
	g_free (margs);
#endif
	goto exit_pinvoke; // prevent unused label warning in some configurations
exit_pinvoke:
	return NULL;
}

} // namespace mono::interp

#include "config.h"

/**
 * \file
 * \brief Calling a method body the compiled tier has already built.
 */

#include "interp-call.hpp"
#include "interp-internals.hpp"
#include "interp-jit-call.hpp"
#include "interp-lmf.hpp"
#include "interp-stackval.hpp"

#include <mono/llvm/runtime.h>
#include <mono/metadata/class-internals.h>
#include <mono/metadata/marshal.h>
#include <mono/metadata/object-internals.h>
#include <mono/mini/llvm-runtime.h>
#include <mono/mini/llvmonly-runtime.h>
#include <mono/mini/mini-runtime.h>
#include <mono/utils/mono-error-internals.h>

#include <cstring>

namespace mono::interp {

enum {
	/* Pass stackval->data.p */
	JIT_ARG_BYVAL,
	/* Pass &stackval->data.p */
	JIT_ARG_BYREF
};

enum {
	JIT_RET_VOID,
	JIT_RET_SCALAR,
	JIT_RET_VTYPE
};

typedef struct {
	int pindex;
	gpointer jit_wrapper;
	gpointer *args;
	MonoFtnDesc ftndesc;
} JitCallCbData;

/* Callback called by mono_llvm_cpp_catch_exception () */
static void
jit_call_cb (gpointer arg)
{
	JitCallCbData *cb_data = (JitCallCbData *) arg;
	gpointer jit_wrapper = cb_data->jit_wrapper;
	int pindex = cb_data->pindex;
	gpointer *args = cb_data->args;
	MonoFtnDesc *ftndesc = &cb_data->ftndesc;

	switch (pindex) {
	case 0: {
		typedef void (*T) (gpointer);
		T func = (T) jit_wrapper;

		func (ftndesc);
		break;
	}
	case 1: {
		typedef void (*T) (gpointer, gpointer);
		T func = (T) jit_wrapper;

		func (args[0], ftndesc);
		break;
	}
	case 2: {
		typedef void (*T) (gpointer, gpointer, gpointer);
		T func = (T) jit_wrapper;

		func (args[0], args[1], ftndesc);
		break;
	}
	case 3: {
		typedef void (*T) (gpointer, gpointer, gpointer, gpointer);
		T func = (T) jit_wrapper;

		func (args[0], args[1], args[2], ftndesc);
		break;
	}
	case 4: {
		typedef void (*T) (gpointer, gpointer, gpointer, gpointer, gpointer);
		T func = (T) jit_wrapper;

		func (args[0], args[1], args[2], args[3], ftndesc);
		break;
	}
	case 5: {
		typedef void (*T) (gpointer, gpointer, gpointer, gpointer, gpointer, gpointer);
		T func = (T) jit_wrapper;

		func (args[0], args[1], args[2], args[3], args[4], ftndesc);
		break;
	}
	case 6: {
		typedef void (*T) (gpointer, gpointer, gpointer, gpointer, gpointer, gpointer, gpointer);
		T func = (T) jit_wrapper;

		func (args[0], args[1], args[2], args[3], args[4], args[5], ftndesc);
		break;
	}
	case 7: {
		typedef void (*T) (gpointer, gpointer, gpointer, gpointer, gpointer, gpointer, gpointer,
		                   gpointer);
		T func = (T) jit_wrapper;

		func (args[0], args[1], args[2], args[3], args[4], args[5], args[6], ftndesc);
		break;
	}
	case 8: {
		typedef void (*T) (gpointer, gpointer, gpointer, gpointer, gpointer, gpointer, gpointer,
		                   gpointer, gpointer);
		T func = (T) jit_wrapper;

		func (args[0], args[1], args[2], args[3], args[4], args[5], args[6], args[7], ftndesc);
		break;
	}
	default:
		g_assert_not_reached ();
		break;
	}
}

typedef struct _JitCallInfo JitCallInfo;
struct _JitCallInfo {
	gpointer addr;
	gpointer extra_arg;
	gpointer wrapper;
	MonoMethodSignature *sig;
	guint8 *arginfo;
	gint32 res_size;
	int ret_mt;
};

static MONO_NEVER_INLINE void
init_jit_call_info (InterpMethod *rmethod, MonoError *error)
{
	MonoMethodSignature *sig;
	JitCallInfo *cinfo;

	//printf ("jit_call: %s\n", mono_method_full_name (rmethod->method, 1));

	MonoMethod *method = rmethod->method;

	// FIXME: Memory management
	cinfo = g_new0 (JitCallInfo, 1);

	sig = mono_method_signature_internal (method);
	g_assert (sig);

	MonoMethod *wrapper = mini_get_gsharedvt_out_sig_wrapper (sig);
	//printf ("J: %s %s\n", mono_method_full_name (method, 1), mono_method_full_name (wrapper, 1));

	gpointer jit_wrapper = mono_jit_compile_method_jit_only (wrapper, error);
	mono_error_assert_ok (error);

	gpointer addr = mono_jit_compile_method_jit_only (method, error);
	return_if_nok (error);
	g_assert (addr);

	if (mono_llvm_only)
		cinfo->addr =
			mini_llvmonly_add_method_wrappers (method, addr, FALSE, FALSE, &cinfo->extra_arg);
	else
		cinfo->addr = addr;
	cinfo->sig = sig;
	cinfo->wrapper = jit_wrapper;

	if (sig->ret->type != MONO_TYPE_VOID) {
		int mt = mint_type (sig->ret);
		if (mt == MINT_TYPE_VT) {
			MonoClass *klass = mono_class_from_mono_type_internal (sig->ret);
			/*
			 * We cache this size here, instead of the instruction stream of the
			 * calling instruction, to save space for common callvirt instructions
			 * that could end up doing a jit call.
			 */
			gint32 size = mono_class_value_size (klass, NULL);
			cinfo->res_size = ALIGN_TO (size, MINT_VT_ALIGNMENT);
		} else {
			cinfo->res_size = MINT_STACK_SLOT_SIZE;
		}
		cinfo->ret_mt = mt;
	} else {
		cinfo->ret_mt = -1;
	}

	if (sig->param_count) {
		cinfo->arginfo = g_new0 (guint8, sig->param_count);

		for (int i = 0; i < rmethod->param_count; ++i) {
			MonoType *t = rmethod->param_types[i];
			int mt = mint_type (t);
			if (sig->params[i]->byref) {
				cinfo->arginfo[i] = JIT_ARG_BYVAL;
			} else if (mt == MINT_TYPE_O) {
				cinfo->arginfo[i] = JIT_ARG_BYREF;
			} else {
				/* stackval->data is an union */
				cinfo->arginfo[i] = JIT_ARG_BYREF;
			}
		}
	}

	mono_memory_barrier ();
	rmethod->jit_call_info = cinfo;
}

MONO_NEVER_INLINE void
do_jit_call (stackval *sp, InterpFrame *frame, InterpMethod *rmethod, MonoError *error)
{
	MonoLMFExt ext;
	JitCallInfo *cinfo;

	//printf ("jit_call: %s\n", mono_method_full_name (rmethod->method, 1));

	/*
	 * Call JITted code through a gsharedvt_out wrapper. These wrappers receive every argument
	 * by ref and return a return value using an explicit return value argument.
	 */
	if (G_UNLIKELY (!rmethod->jit_call_info)) {
		init_jit_call_info (rmethod, error);
		mono_error_assert_ok (error);
	}
	cinfo = (JitCallInfo *) rmethod->jit_call_info;

	/*
	 * Convert the arguments on the interpeter stack to the format expected by the gsharedvt_out wrapper.
	 */
	gpointer args[32];
	int pindex = 0;
	int stack_index = 0;
	if (rmethod->hasthis) {
		args[pindex++] = sp[0].data.p;
		stack_index++;
	}
	/* return address */
	if (cinfo->ret_mt != -1)
		args[pindex++] = sp;
	for (int i = 0; i < rmethod->param_count; ++i) {
		stackval *sval = STACK_ADD_BYTES (sp, get_arg_offset_fast (rmethod, stack_index + i));
		if (cinfo->arginfo[i] == JIT_ARG_BYVAL)
			args[pindex++] = sval->data.p;
		else
			/* data is an union, so can use 'p' for all types */
			args[pindex++] = sval;
	}

	/* Every field is written below and nothing reads the padding, so the
	 * struct is not zeroed first: gcc turns a 40-byte memset into a rep stos
	 * whose startup alone is a tenth of the call. */
	JitCallCbData cb_data;
	cb_data.jit_wrapper = cinfo->wrapper;
	cb_data.pindex = pindex;
	cb_data.args = args;
	cb_data.ftndesc.addr = cinfo->addr;
	cb_data.ftndesc.arg = cinfo->extra_arg;

	interp_push_lmf (&ext, frame);
	gboolean thrown = FALSE;
	if (mono_aot_mode == MONO_AOT_MODE_LLVMONLY_INTERP) {
		/* Catch the exception thrown by the native code using a try-catch */
		mono_llvm_cpp_catch_exception (jit_call_cb, &cb_data, &thrown);
	} else {
		jit_call_cb (&cb_data);
	}
	interp_pop_lmf (&ext);
	if (thrown) {
		MonoObject *obj = mono_llvm_load_exception ();
		g_assert (obj);
		mono_error_set_exception_instance (error, (MonoException *) obj);
		return;
	}
	if (cinfo->ret_mt != -1) {
		//  Sign/zero extend if necessary
		switch (cinfo->ret_mt) {
		case MINT_TYPE_I1:
			sp->data.i = *(gint8 *) sp;
			break;
		case MINT_TYPE_U1:
			sp->data.i = *(guint8 *) sp;
			break;
		case MINT_TYPE_I2:
			sp->data.i = *(gint16 *) sp;
			break;
		case MINT_TYPE_U2:
			sp->data.i = *(guint16 *) sp;
			break;
		case MINT_TYPE_I4:
		case MINT_TYPE_I8:
		case MINT_TYPE_R4:
		case MINT_TYPE_R8:
		case MINT_TYPE_VT:
		case MINT_TYPE_O:
			/* The result was written to sp */
			break;
		default:
			g_assert_not_reached ();
		}
	}
}

} // namespace mono::interp

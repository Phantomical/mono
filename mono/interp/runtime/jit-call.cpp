#include "config.h"

/**
 * \file
 * \brief Calling a method body the compiled tier has already built.
 */

#include "call.hpp"
#include "internals.hpp"
#include "jit-call.hpp"
#include "lmf.hpp"
#include "stackval.hpp"

#include <mono/llvm/runtime.h>
#include <mono/metadata/class-internals.h>
#include <mono/metadata/marshal.h>
#include <mono/metadata/object-internals.h>
#include <mono/mini/llvm-runtime.h>
#include <mono/mini/llvmonly-runtime.h>
#include <mono/mini/mini-runtime.h>
#include <mono/utils/mono-error-internals.h>

#include <cstring>
#include <optional>

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

struct JitCallCbData {
	int pindex;
	gpointer jit_wrapper;
	gpointer *args;
	MonoFtnDesc ftndesc;
};

/* Callback called by mono_llvm_cpp_catch_exception () */
static void
jit_call_cb (gpointer arg)
{
	JitCallCbData *cb_data = static_cast<JitCallCbData *> (arg);
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

struct JitCallInfo {
	gpointer addr = nullptr;
	gpointer extra_arg = nullptr;
	gpointer wrapper = nullptr;
	MonoMethodSignature *sig = nullptr;
	guint8 *arginfo = nullptr;
	gint32 res_size = 0;
	/// Empty when the call returns void, and so writes nothing back to sp.
	std::optional<MintType> ret_mt;
};

static MONO_NEVER_INLINE void
init_jit_call_info (InterpMethod *rmethod, MonoError *error)
{
	MonoMethodSignature *sig;
	JitCallInfo *cinfo;

	//printf ("jit_call: %s\n", mono_method_full_name (rmethod->method, 1));

	MonoMethod *method = rmethod->method;

	// FIXME: Memory management
	cinfo = new JitCallInfo{};

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
		MintType mt = mint_type (sig->ret);
		if (mt == MintType::VT) {
			MonoClass *klass = mono_class_from_mono_type_internal (sig->ret);
			/*
			 * We keep this size here, and not in the calling instruction. callvirt
			 * instructions are common, and any of them can end in a jit call. The
			 * size in the instruction stream makes all of them larger.
			 */
			gint32 size = mono_class_value_size (klass, NULL);
			cinfo->res_size = ALIGN_TO (size, MINT_VT_ALIGNMENT);
		} else {
			cinfo->res_size = MINT_STACK_SLOT_SIZE;
		}
		cinfo->ret_mt = mt;
	} else {
		cinfo->ret_mt = std::nullopt;
	}

	if (sig->param_count) {
		cinfo->arginfo = g_new0 (guint8, sig->param_count);

		for (int i = 0; i < rmethod->param_count; ++i) {
			MonoType *t = rmethod->param_types[i];
			MintType mt = mint_type (t);
			if (sig->params[i]->byref) {
				cinfo->arginfo[i] = JIT_ARG_BYVAL;
			} else if (mt == MintType::O) {
				cinfo->arginfo[i] = JIT_ARG_BYREF;
			} else {
				/* stackval->data is a union */
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
	 * Call JITted code through a gsharedvt_out wrapper: it takes every argument
	 * by reference and writes its result through an explicit return-value
	 * argument.
	 */
	if (G_UNLIKELY (!rmethod->jit_call_info)) {
		init_jit_call_info (rmethod, error);
		mono_error_assert_ok (error);
	}
	cinfo = static_cast<JitCallInfo *> (rmethod->jit_call_info);

	/*
	 * Convert the arguments on the interpreter stack to the format the
	 * gsharedvt_out wrapper expects.
	 */
	gpointer args[32];
	int pindex = 0;
	int stack_index = 0;
	if (rmethod->hasthis) {
		args[pindex++] = sp[0].data.p;
		stack_index++;
	}
	/* return address */
	if (cinfo->ret_mt)
		args[pindex++] = sp;
	for (int i = 0; i < rmethod->param_count; ++i) {
		stackval *sval = STACK_ADD_BYTES (sp, get_arg_offset_fast (rmethod, stack_index + i));
		if (cinfo->arginfo[i] == JIT_ARG_BYVAL)
			args[pindex++] = sval->data.p;
		else
			/* data is a union, so can use 'p' for all types */
			args[pindex++] = sval;
	}

	/* Every field here is written below, and the padding is never read, so we
	 * do not zero the struct first. gcc turns a 40-byte memset into a rep
	 * stos, and its startup alone costs a tenth of the call. */
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
		mono_error_set_exception_instance (error, reinterpret_cast<MonoException *> (obj));
		return;
	}
	if (cinfo->ret_mt) {
		//  Sign/zero extend if necessary
		switch (*cinfo->ret_mt) {
		case MintType::I1:
			sp->data.i = *reinterpret_cast<gint8 *> (sp);
			break;
		case MintType::U1:
			sp->data.i = *reinterpret_cast<guint8 *> (sp);
			break;
		case MintType::I2:
			sp->data.i = *reinterpret_cast<gint16 *> (sp);
			break;
		case MintType::U2:
			sp->data.i = *reinterpret_cast<guint16 *> (sp);
			break;
		case MintType::I4:
		case MintType::I8:
		case MintType::R4:
		case MintType::R8:
		case MintType::VT:
		case MintType::O:
			/* The result was written to sp */
			break;
		default:
			g_assert_not_reached ();
		}
	}
}

} // namespace mono::interp

#include "glib.h"
#include "mintops.h"
#include "mono/llvm/runtime.h"
#include "mono/metadata/appdomain.h"
#include "mono/metadata/class-internals.h"
#include "mono/metadata/metadata.h"
#include "mono/metadata/object-forward.h"
#include "mono/metadata/object-internals.h"
#include "mono/metadata/object.h"
#include "mono/metadata/profiler.h"
#include "mono/metadata/tabledefs.h"
#include "mono/mini/interp/interp-internals.h"
#include "mono/mini/interp/interp-internals.hpp"
#include "mono/mini/interp/interp.hpp"
#include "mono/mini/interp/frame-data.hpp"
#include "mono/mini/interp/transform.h"
#include "mono/mini/llvm-runtime.h"
#include "mono/mini/llvmonly-runtime.h"
#include "mono/utils/atomic.h"
#include "mono/utils/mono-compiler.h"
#include "mono/utils/mono-error-internals.h"
#include <cstring>

namespace mono::interp {

MONO_INTERP_ENTRY (exec_exit_frame, exit_frame);

MONO_ALWAYS_INLINE InterpState::OpFunc
InterpState::exit_frame ()
{
	g_assert_checked (frame->imethod);

	if (frame->parent && frame->parent->state.ip) {
		// return to the main loop after a non-recursive interpreter call
		g_assert_checked (frame->stack);
		// A suspended parent means call () made this frame, so it is the top of the
		// frame stack and nothing above it is live.
		g_assert_checked ((guchar *) frame >= context->frame_stack_start
		                  && (guchar *) (frame + 1) <= context->frame_stack_pointer);
		context->frame_stack_pointer = (guchar *) frame;
		frame = frame->parent;
		context->current_frame = frame;
		context->stack_pointer = (guchar *) frame->stack + frame->imethod->alloca_size;
		LOAD_INTERP_STATE (frame);
		CHECK_RESUME_STATE (context);

		MONO_INTERP_DISPATCH ();
	}

	return &exec_exit;
}

MONO_INTERP_OP_IMPL (MINT_RET)
{
	frame->stack[0] = LOCAL_VAR (ip[1], stackval);
	return &exec_exit_frame;
}

MONO_INTERP_OP_IMPL (MINT_RET_VOID)
{
	return &exec_exit_frame;
}

MONO_INTERP_OP_IMPL (MINT_RET_VT)
{
	std::memmove (frame->stack, &LOCAL_VAR (ip[1], char), ip[2]);
	return &exec_exit_frame;
}

MONO_INTERP_OP_IMPL (MINT_RET_LOCALLOC)
{
	frame->stack[0] = LOCAL_VAR (ip[1], stackval);
	frame_data_allocator_pop (&context->data_stack, frame);
	return &exec_exit_frame;
}

MONO_INTERP_OP_IMPL (MINT_RET_VOID_LOCALLOC)
{
	frame_data_allocator_pop (&context->data_stack, frame);
	return &exec_exit_frame;
}

MONO_INTERP_OP_IMPL (MINT_RET_VT_LOCALLOC)
{
	std::memmove (frame->stack, &LOCAL_VAR (ip[1], char), ip[2]);
	frame_data_allocator_pop (&context->data_stack, frame);
	return &exec_exit_frame;
}

MONO_INTERP_OP_IMPL (MINT_JMP)
{
	auto new_method = (InterpMethod *) frame->imethod->data_items[ip[1]];

	if (frame->imethod->prof_flags & MONO_PROFILER_CALL_INSTRUMENTATION_TAIL_CALL)
		MONO_PROFILER_RAISE (method_tail_call, (frame->imethod->method, new_method->method));

	if (G_UNLIKELY (!new_method->transformed)) {
		error_init_reuse (error);

		mono_interp_transform_method (new_method, context, error);
		if (G_UNLIKELY (!is_ok (error)))
			THROW_EX (mono_error_convert_to_exception (error), ip);

		EXCEPTION_CHECKPOINT;
	}

	// It's possible for the caller stack frame to be smaller than the callee stack frame
	// (at the interp level).
	context->stack_pointer = (guchar *) frame->stack + new_method->alloca_size;
	frame->imethod = new_method;
	frame_root_code_owner (frame);
	ip = frame->imethod->code;

	MONO_INTERP_DISPATCH ();
}

static InterpMethod *
get_virtual_method (InterpMethod *imethod, MonoVTable *vtable)
{
	MonoMethod *m = imethod->method;
	MonoDomain *domain = imethod->domain;
	InterpMethod *ret = NULL;

#ifndef DISABLE_REMOTING
	if (mono_class_is_transparent_proxy (vtable->klass)) {
		ERROR_DECL (error);
		MonoMethod *remoting_invoke_method = mono_marshal_get_remoting_invoke_with_check (m, error);
		mono_error_assert_ok (error);
		ret = mono_interp_get_imethod (domain, remoting_invoke_method, error);
		mono_error_assert_ok (error);
		return ret;
	}
#endif

	if ((m->flags & METHOD_ATTRIBUTE_FINAL) || !(m->flags & METHOD_ATTRIBUTE_VIRTUAL)) {
		if (m->iflags & METHOD_IMPL_ATTRIBUTE_SYNCHRONIZED) {
			ERROR_DECL (error);
			ret =
				mono_interp_get_imethod (domain, mono_marshal_get_synchronized_wrapper (m), error);
			mono_interp_error_cleanup (error); /* FIXME: don't swallow the error */
		} else {
			ret = imethod;
		}
		return ret;
	}

	mono_class_setup_vtable (vtable->klass);

	int slot = mono_method_get_vtable_slot (m);
	if (mono_class_is_interface (m->klass)) {
		g_assert (vtable->klass != m->klass);
		/* TODO: interface offset lookup is slow, go through IMT instead */
		gboolean non_exact_match;
		slot +=
			mono_class_interface_offset_with_variance (vtable->klass, m->klass, &non_exact_match);
	}

	MonoMethod *virtual_method = m_class_get_vtable (vtable->klass)[slot];
	if (m->is_inflated && mono_method_get_context (m)->method_inst) {
		MonoGenericContext context = {NULL, NULL};

		if (mono_class_is_ginst (virtual_method->klass))
			context.class_inst =
				mono_class_get_generic_class (virtual_method->klass)->context.class_inst;
		else if (mono_class_is_gtd (virtual_method->klass))
			context.class_inst =
				mono_class_get_generic_container (virtual_method->klass)->context.class_inst;
		context.method_inst = mono_method_get_context (m)->method_inst;

		ERROR_DECL (error);
		virtual_method =
			mono_class_inflate_generic_method_checked (virtual_method, &context, error);
		mono_error_cleanup (error); /* FIXME: don't swallow the error */
	}

	if (virtual_method->iflags & METHOD_IMPL_ATTRIBUTE_INTERNAL_CALL) {
		virtual_method = mono_marshal_get_native_wrapper (virtual_method, FALSE, FALSE);
	}

	if (virtual_method->iflags & METHOD_IMPL_ATTRIBUTE_SYNCHRONIZED) {
		virtual_method = mono_marshal_get_synchronized_wrapper (virtual_method);
	}

	ERROR_DECL (error);
	InterpMethod *virtual_imethod = mono_interp_get_imethod (domain, virtual_method, error);
	mono_error_cleanup (error); /* FIXME: don't swallow the error */
	return virtual_imethod;
}

static InterpMethod *
interp_get_native_func_wrapper (InterpMethod *imethod, MonoMethodSignature *csignature,
                                guchar *code, MonoError *error)
{
	/* Pinvoke call is missing the wrapper. See mono_get_native_calli_wrapper */
	MonoMarshalSpec **mspecs = g_newa0 (MonoMarshalSpec *, csignature->param_count + 1);

	MonoMethodPInvoke iinfo;
	memset (&iinfo, 0, sizeof (iinfo));

	MonoMethod *m = mono_marshal_get_native_func_wrapper (
		m_class_get_image (imethod->method->klass), csignature, &iinfo, mspecs, code);

	for (int i = csignature->param_count; i >= 0; i--)
		if (mspecs[i])
			mono_metadata_free_marshal_spec (mspecs[i]);

	InterpMethod *cmethod = mono_interp_get_imethod (imethod->domain, m, error);
	return cmethod;
}

/*
 * Settle how calls to imethod are made and record the answer on it. A call goes
 * natively, through do_jit_call (), when the method already has code or when its
 * entry address is in native hands. The method is interpreted otherwise.
 *
 * IMETHOD_CODE_COMPILED is a permanent answer - a body is never taken back and
 * the address it is entered at is fixed - while IMETHOD_CODE_INTERP is only true
 * until something compiles the method, which interp_method_compiled () reports.
 */
static MONO_NEVER_INLINE InterpMethodCodeType
resolve_code_type (InterpMethod *imethod)
{
	MonoMethod *method = imethod->method;
	MonoMethodSignature *sig = mono_method_signature_internal (method);
	InterpMethodCodeType code_type = IMETHOD_CODE_INTERP;
	gboolean marshallable = mono_interp_jit_call_marshallable (method, sig);

	/*
	 * A patcher turns inlining off before it writes over an entry address,
	 * because an inlined call site keeps a copy of the original body and the
	 * jump never reaches it. The two facts together tell a patched method from
	 * one whose address was only passed on.
	 */
	gboolean patched =
		method->native_entry_escaped && (method->iflags & METHOD_IMPL_ATTRIBUTE_NOINLINING) != 0;

	if (patched && !marshallable) {
		char *name = mono_method_full_name (method, TRUE);

		g_printerr ("[interp] the entry address of %s is in native hands, but no "
		            "jit call fits its signature. A patch written over that "
		            "address does not take effect.\n",
		            name);
		g_free (name);
	}

	/*
	 * The only chance to arm a method nothing ever asked the backend for: its
	 * callers reached it by interpreting, so it has no stub. A patched method
	 * stays unarmed, because a promotion redirects the stub and the redirect
	 * writes over the patch.
	 */
	if (!patched && mono_atomic_load_i32_relaxed (&imethod->tier_counter) == 0)
		interp_arm_tier_counter (imethod, mono_llvm_jit_tier0_calls (method));

	if (marshallable && (patched || mono_jit_method_is_compiled (imethod->domain, method)))
		code_type = IMETHOD_CODE_COMPILED;

	/*
	 * A compile that finished while the queries above were running has already
	 * written COMPILED, and that answer is the later one, so leave it alone.
	 */
	mono_atomic_cas_i32 ((gint32 *) &imethod->code_type, code_type, IMETHOD_CODE_UNKNOWN);
	return imethod->code_type;
}

/*
 * Transform CMETHOD, which FRAME is about to be handed to by a tail call.
 *
 * Unlike the frame do_transform_method () runs under, this one is complete and still
 * executing the method it is being taken away from, so the walk a class load or a throw
 * inside the transform can trigger has to find it rather than its parent.
 */
static MonoException *
do_transform_tail_callee (InterpFrame *frame, InterpMethod *cmethod, ThreadContext *context,
                          const guint16 *ip)
{
	MonoLMFExt ext;
	gboolean push_lmf = frame->parent != NULL;
	ERROR_DECL (error);

	frame->state.ip = ip;
	if (push_lmf)
		interp_push_lmf (&ext, frame);

	mono_interp_transform_method (cmethod, context, error);

	if (push_lmf)
		interp_pop_lmf (&ext);
	frame->state.ip = NULL;

	return mono_error_convert_to_exception (error);
}

/* Does not handle `this` argument */
static guint32
compute_arg_offset (MonoMethodSignature *sig, int index, int prev_offset)
{
	if (index == 0)
		return 0;

	if (prev_offset == -1) {
		guint32 offset = 0;
		for (int i = 0; i < index; i++) {
			int size, align;
			MonoType *type = sig->params[i];
			size = mono_type_size (type, &align);
			offset += ALIGN_TO (size, MINT_STACK_SLOT_SIZE);
		}
		return offset;
	} else {
		int size, align;
		MonoType *type = sig->params[index - 1];
		size = mono_type_size (type, &align);
		return prev_offset + ALIGN_TO (size, MINT_STACK_SLOT_SIZE);
	}
}

static guint32 *
initialize_arg_offsets (InterpMethod *imethod)
{
	if (imethod->arg_offsets)
		return imethod->arg_offsets;

	MonoMethodSignature *sig = mono_method_signature_internal (imethod->method);
	int arg_count = sig->hasthis + sig->param_count;
	g_assert (arg_count);
	guint32 *arg_offsets = (guint32 *) g_malloc ((sig->hasthis + sig->param_count) * sizeof (int));
	int index = 0, offset_addend = 0, prev_offset = 0;

	if (sig->hasthis) {
		arg_offsets[index++] = 0;
		offset_addend = MINT_STACK_SLOT_SIZE;
	}

	for (int i = 0; i < sig->param_count; i++) {
		prev_offset = compute_arg_offset (sig, i, prev_offset);
		arg_offsets[index++] = prev_offset + offset_addend;
	}

	mono_memory_write_barrier ();
	if (mono_atomic_cas_ptr ((gpointer *) &imethod->arg_offsets, arg_offsets, NULL) != NULL)
		g_free (arg_offsets);
	return imethod->arg_offsets;
}

static guint32
get_arg_offset_fast (InterpMethod *imethod, int index)
{
	guint32 *arg_offsets = imethod->arg_offsets;
	if (arg_offsets)
		return arg_offsets[index];

	arg_offsets = initialize_arg_offsets (imethod);
	g_assert (arg_offsets);
	return arg_offsets[index];
}

static guint32
get_arg_offset (InterpMethod *imethod, MonoMethodSignature *sig, int index)
{
	if (imethod && sig == mono_method_signature_internal (imethod->method))
		return get_arg_offset_fast (imethod, index);

	g_assert (!sig->hasthis);
	return compute_arg_offset (sig, index, -1);
}

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

static MONO_NEVER_INLINE void
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

MONO_INTERP_ENTRY (exec_call, call);
MONO_INTERP_ENTRY (exec_calli, calli);
MONO_INTERP_ENTRY (exec_tailcall, tailcall);

MONO_ALWAYS_INLINE InterpState::OpFunc
InterpState::call ()
{
	auto code_type = cmethod->code_type;
	if (G_UNLIKELY (code_type != IMETHOD_CODE_INTERP)) {
		if (code_type == IMETHOD_CODE_UNKNOWN)
			code_type = resolve_code_type (cmethod);

		// We don't support directly calling jit methods in another domain at the moment
		if (code_type == IMETHOD_CODE_COMPILED && cmethod->domain == mono_domain_get ()) {
			/* for calls, have ip pointing at the start of next instruction */
			frame->state.ip = ip;
			error_init_reuse (error);
			do_jit_call ((stackval *) (locals + call_args_offset), frame, cmethod, error);
			if (!is_ok (error))
				THROW_EX (mono_error_convert_to_exception (error), ip);

			CHECK_RESUME_STATE (context);
			MONO_INTERP_DISPATCH ();
		}
	}

	if (G_UNLIKELY (mono_atomic_load_i32_relaxed (&cmethod->tier_counter) > 0))
		interp_check_call_promotion (cmethod);

	SAVE_INTERP_STATE (frame);

	// Allocate the child frame. exit_frame () gives it back, so the two have to
	// stay paired: a frame reached with its parent suspended came from here.
	{
		auto child_frame = (InterpFrame *) context->frame_stack_pointer;

		if (G_UNLIKELY ((guchar *) (child_frame + 1)
		                > context->frame_stack_start + INTERP_FRAME_STACK_SIZE))
			THROW_EX (mono_get_exception_stack_overflow (), ip);

		context->frame_stack_pointer = (guchar *) (child_frame + 1);
		/* reinit_frame () roots the callee's code in the frame, so the pointer has
		 * to cover it before that reference is stored. */
		mono_compiler_barrier ();

		reinit_frame (child_frame, context, frame, cmethod, locals + call_args_offset);
		frame = child_frame;
	}

	MonoException *ex;
	if (method_entry (context, frame,
#if DEBUG_INTERP
	                  &tracing,
#endif
	                  &ex)) {
		if (ex)
			THROW_EX (ex, NULL);
		EXCEPTION_CHECKPOINT;
	}

	// check for stack overflow
	if (G_UNLIKELY ((guchar *) frame->stack + cmethod->alloca_size
	                > context->stack_start + INTERP_STACK_SIZE - INTERP_STACK_RESERVE))
		THROW_EX (mono_get_exception_stack_overflow (), NULL);

	context->stack_pointer = (guchar *) frame->stack + cmethod->alloca_size;
	/* Make sure the stack pointer is bumped before we store any references on the stack */
	mono_compiler_barrier ();

	INIT_INTERP_STATE (frame, NULL);
	context->current_frame = frame;

	MONO_INTERP_DISPATCH ();
}

MONO_ALWAYS_INLINE InterpState::OpFunc
InterpState::calli ()
{
	if (cmethod->method->flags & METHOD_ATTRIBUTE_PINVOKE_IMPL) {
		error_init_reuse (error);
		cmethod = mono_interp_get_imethod (
			frame->imethod->domain, mono_marshal_get_native_wrapper (cmethod->method, false, false),
			error);
		mono_interp_error_cleanup (error); // FIXME: don't swallow the error
	}

	if (calli_signature->hasthis) {
		auto this_arg = LOCAL_VAR (call_args_offset, MonoObject *);

		if (m_class_is_valuetype (this_arg->vtable->klass)) {
			gpointer unboxed = mono_object_unbox_internal (this_arg);
			LOCAL_VAR (call_args_offset, gpointer) = unboxed;
		}
	}

	return &exec_call;
}

MONO_ALWAYS_INLINE InterpState::OpFunc
InterpState::tailcall ()
{
	// Tailcalls always stay in the interpreter. If we didn't, then something that
	// is expected to use constant stack space can easily turn into a stack overflow
	// if one method in a mutual recursion chain is promoted to tier1+ and the other
	// is not.

	auto code_type = cmethod->code_type;
	if (G_UNLIKELY (code_type == IMETHOD_CODE_UNKNOWN))
		code_type = resolve_code_type (cmethod);

	// Should the called method be promoted?
	if (G_UNLIKELY (mono_atomic_load_i32_relaxed (&cmethod->tier_counter) > 0))
		interp_check_call_promotion (cmethod);

	// The one exception to sticking in the interpreter is self-calls. If we are
	// compiled then we can turn that into a regular call and the compiled method
    // will (usually) be able to tailcall internally.
	if (code_type == IMETHOD_CODE_COMPILED && cmethod == frame->imethod
	    && cmethod->domain == mono_domain_get ())
		return &exec_call;

	if (G_UNLIKELY (!cmethod->transformed)) {
		if (auto ex = do_transform_tail_callee (frame, cmethod, context, ip))
			THROW_EX (ex, ip);
		EXCEPTION_CHECKPOINT;
	}

	// if the tailcall would overflow the stack then switch to a regular call
	if (G_UNLIKELY ((guchar *) frame->stack + cmethod->alloca_size
	                > context->stack_start + INTERP_STACK_SIZE))
		return &exec_call;

	if (G_UNLIKELY (frame->imethod->prof_flags & MONO_PROFILER_CALL_INSTRUMENTATION_TAIL_CALL))
		MONO_PROFILER_RAISE (method_tail_call, (frame->imethod->method, cmethod->method));

	guchar *new_top = (guchar *) frame->stack + cmethod->alloca_size;

	// Need the compiler barriers so that the GC never sees the stack top write reordered around the memmove.
	// We also need stack_pointer to cover all the relevant data while copying.
	if (new_top > context->stack_pointer) {
		context->stack_pointer = new_top;
		mono_compiler_barrier ();
	}

	std::memmove (locals, locals + call_args_offset, tail_args_size);

	if (new_top < context->stack_pointer) {
		mono_compiler_barrier ();
		context->stack_pointer = new_top;
	}

	frame_data_allocator_pop (&context->data_stack, frame);

	frame->imethod = cmethod;
	frame_root_code_owner (frame);

	INIT_INTERP_STATE (frame, NULL);

	MONO_INTERP_DISPATCH ();
}

MONO_INTERP_OP_IMPL (MINT_CALL_DELEGATE)
{
	auto csignature = (MonoMethodSignature *) frame->imethod->data_items[ip[3]];
	guint16 param_count = csignature->param_count;
	call_args_offset = ip[1];
	auto del = LOCAL_VAR (call_args_offset, MonoDelegate *);
	bool is_multicast = del->method == nullptr;
	auto del_imethod = (InterpMethod *) del->interp_invoke_impl;

	if (G_UNLIKELY (!del_imethod)) {
		if (is_multicast) {
			error_init_reuse (error);
			MonoMethod *invoke = mono_get_delegate_invoke_internal (del->object.vtable->klass);
			del_imethod = mono_interp_get_imethod (
				del->object.vtable->domain, mono_marshal_get_delegate_invoke (invoke, del), error);
			del->interp_invoke_impl = del_imethod;
			mono_error_assert_ok (error);
		} else if (!del->interp_method) {
			// not created from interpreted code
			error_init_reuse (error);
			g_assert (del->method);
			del_imethod = mono_interp_get_imethod (del->object.vtable->domain, del->method, error);
			del->interp_method = del_imethod;
			del->interp_invoke_impl = del_imethod;
			mono_error_assert_ok (error);
		} else {
			del_imethod = (InterpMethod *) del->interp_method;

			if (del_imethod->method->flags & METHOD_ATTRIBUTE_PINVOKE_IMPL) {
				error_init_reuse (error);
				del_imethod = mono_interp_get_imethod (
					frame->imethod->domain,
					mono_marshal_get_native_wrapper (del_imethod->method, false, false), error);
				mono_error_assert_ok (error);
				del->interp_invoke_impl = del_imethod;
			} else if (del_imethod->method->flags & METHOD_ATTRIBUTE_VIRTUAL && !del->target) {
				// this is passed dynamically, we need to recompute the target
				// method with each call.
				del_imethod = get_virtual_method (
					del_imethod,
					LOCAL_VAR (call_args_offset + MINT_STACK_SLOT_SIZE, MonoObject *)->vtable);
			} else {
				del->interp_invoke_impl = del_imethod;
			}
		}
	}

	cmethod = del_imethod;
	if (!is_multicast) {
		if (cmethod->param_count == param_count + 1) {
			// Target method is static but the delegate has a target object. We handle
			// this separately from the case below, because, for these calls, the instance
			// is allowed to be null.
			LOCAL_VAR (ip[1], MonoObject *) = del->target;
		} else if (del->target) {
			MonoObject *this_arg = del->target;

			// replace the MonoDelegate* on the stack with 'this' pointer
			if (m_class_is_valuetype (this_arg->vtable->klass)) {
				gpointer unboxed = mono_object_unbox_internal (this_arg);
				LOCAL_VAR (ip[1], gpointer) = unboxed;
			} else {
				LOCAL_VAR (ip[1], MonoObject *) = this_arg;
			}
		} else {
			// skip the delegate pointer for static calls
			// FIXME we could avoid memmove
			std::memmove (locals + call_args_offset,
			              locals + call_args_offset + MINT_STACK_SLOT_SIZE, ip[2]);
		}
	}

	MONO_INTERP_OP_ADVANCE ();
	return &exec_call;
}

MONO_INTERP_OP_IMPL (MINT_CALLI)
{
	gpointer ftn = LOCAL_VAR (ip[2], gpointer);

	// We cache the InterpMethod* used for a calli in data_items.
	cmethod = (InterpMethod *) frame->imethod->data_items[ip[4]];

	if (G_UNLIKELY (!cmethod || cmethod->jit_entry != ftn)) {
		error_init_reuse (error);
		cmethod = imethod_for_entry (frame->imethod->domain, ftn, error);
		if (G_UNLIKELY (!is_ok (error)))
			THROW_EX (mono_error_convert_to_exception (error), ip);
		if (!cmethod)
			THROW_EX (
				mono_get_exception_execution_engine (
					"mono's interpreter does not support MINT_CALLI with a function pointer that does not map to a known MonoMethod*"),
				ip);

		if (cmethod->jit_entry == ftn)
			frame->imethod->data_items[ip[4]] = cmethod;
	}

	calli_signature = (MonoMethodSignature *) frame->imethod->data_items[ip[3]];
	call_args_offset = ip[1];

	MONO_INTERP_OP_ADVANCE ();
	return &exec_calli;
}

MONO_INTERP_OP_IMPL (MINT_CALLI_IMETHOD)
{
	cmethod = LOCAL_VAR (ip[2], InterpMethod *);
	calli_signature = (MonoMethodSignature *) frame->imethod->data_items[ip[3]];
	call_args_offset = ip[1];

	MONO_INTERP_OP_ADVANCE ();
	return &exec_calli;
}

MONO_INTERP_OP_IMPL (MINT_CALLI_NAT_FAST)
{
	auto csignature = (MonoMethodSignature *) frame->imethod->data_items[ip[2]];
	guint16 opcode = ip[3];
	bool save_last_error = ip[4];
	auto args = &LOCAL_VAR (ip[1], stackval);
	gpointer target_ip = args[csignature->param_count].data.p;

	// for calls, have ip pointing at the start of next instruction
	frame->state.ip = ip + 5;

	mono_interp_do_icall (frame, csignature, opcode, args, target_ip, save_last_error);
	EXCEPTION_CHECKPOINT_GC_UNSAFE;
	CHECK_RESUME_STATE (context);

	MONO_INTERP_OP_ADVANCE ();
	MONO_INTERP_DISPATCH ();
}

MONO_INTERP_OP_IMPL (MINT_CALLI_NAT_DYNAMIC)
{
	auto csignature = (MonoMethodSignature *) frame->imethod->data_items[ip[3]];

	call_args_offset = ip[1];
	guchar *code = LOCAL_VAR (ip[2], guchar *);

	error_init_reuse (error);
	cmethod = interp_get_native_func_wrapper (frame->imethod, csignature, code, error);
	if (G_UNLIKELY (!is_ok (error)))
		THROW_EX (mono_error_convert_to_exception (error), ip);

	MONO_INTERP_OP_ADVANCE ();
	return &exec_call;
}

MONO_INTERP_OP_IMPL (MINT_CALLI_NAT)
{
	auto csignature = (MonoMethodSignature *) frame->imethod->data_items[ip[3]];
	auto imethod = (InterpMethod *) frame->imethod->data_items[ip[4]];
	auto code = LOCAL_VAR (ip[2], guchar *);
	bool save_last_error = ip[5];
	auto cache = (gpointer *) &frame->imethod->data_items[ip[6]];

	/* for calls, have ip pointing at the start of next instruction */
	frame->state.ip = ip + 7;
	mono_interp_do_pinvoke (imethod, csignature, (MonoFuncV) code, context, frame,
	                        &LOCAL_VAR (ip[1], stackval), save_last_error, cache);

	EXCEPTION_CHECKPOINT_GC_UNSAFE;
	CHECK_RESUME_STATE (context);

	MONO_INTERP_OP_ADVANCE ();
	MONO_INTERP_DISPATCH ();
}

typedef struct {
	InterpMethod *imethod;
	InterpMethod *target_imethod;
} InterpVTableEntry;

/* memory manager lock must be held */
static GSList *
append_imethod (MonoMemoryManager *memory_manager, GSList *list, InterpMethod *imethod,
                InterpMethod *target_imethod)
{
	GSList *ret;
	InterpVTableEntry *entry;

	entry = (InterpVTableEntry *) mono_mem_manager_alloc_nolock (memory_manager,
	                                                             sizeof (InterpVTableEntry));
	entry->imethod = imethod;
	entry->target_imethod = target_imethod;
	ret = g_slist_append_mempool (memory_manager->mp, list, entry);

	return ret;
}

static InterpMethod *
get_target_imethod (GSList *list, InterpMethod *imethod)
{
	while (list != NULL) {
		InterpVTableEntry *entry = (InterpVTableEntry *) list->data;
		if (entry->imethod == imethod)
			return entry->target_imethod;
		list = list->next;
	}
	return NULL;
}

static gpointer *
get_method_table (MonoVTable *vtable, int offset)
{
	if (offset >= 0)
		return vtable->interp_vtable;
	else
		return (gpointer *) vtable;
}

static gpointer *
alloc_method_table (MonoVTable *vtable, int offset)
{
	gpointer *table;

	if (offset >= 0) {
		table = (gpointer *) m_class_alloc0 (vtable->domain, vtable->klass,
		                                     m_class_get_vtable_size (vtable->klass)
		                                         * sizeof (gpointer));
		vtable->interp_vtable = table;
	} else {
		table = (gpointer *) vtable;
	}

	return table;
}

/*
 * Says whether the receiver's vtable has a slot for this offset.
 *
 * Both tables are indexed without a bound of their own, and the interface one
 * is indexed below the vtable pointer, so a receiver of the wrong class does
 * not read a wrong method - it reads, and then writes, outside the allocation.
 * One such receiver therefore becomes a corruptor of whatever the domain
 * allocated before the vtable, and the fault appears later somewhere else.
 *
 * A class with no interfaces gets imt_table_bytes = 0 in mono_class_create_runtime_vtable (),
 * so for it there is no region below the vtable at all.
 */
static gboolean
method_table_holds_offset (MonoVTable *vtable, int offset)
{
	if (offset < 0)
		return offset >= -2 * MONO_IMT_SIZE
		       && m_class_get_interface_offsets_count (vtable->klass) != 0;

	return offset < m_class_get_vtable_size (vtable->klass);
}

static InterpMethod *
get_virtual_method_fast (InterpMethod *imethod, MonoVTable *vtable, int offset)
{
	gpointer *table;
	MonoMemoryManager *memory_manager = m_class_get_mem_manager (vtable->domain, vtable->klass);

#ifndef DISABLE_REMOTING
	/* FIXME Remoting */
	if (mono_class_is_transparent_proxy (vtable->klass))
		return get_virtual_method (imethod, vtable);
#endif

	g_assertf (method_table_holds_offset (vtable, offset),
	           "receiver of class %s.%s has no method table slot %d, called from %s:%s",
	           m_class_get_name_space (vtable->klass), m_class_get_name (vtable->klass), offset,
	           m_class_get_name (imethod->method->klass), imethod->method->name);

	table = get_method_table (vtable, offset);

	if (!table) {
		/* Lazily allocate method table */
		mono_domain_lock (vtable->domain);
		table = get_method_table (vtable, offset);
		if (!table)
			table = alloc_method_table (vtable, offset);
		mono_domain_unlock (vtable->domain);
	}

	if (!table[offset]) {
		InterpMethod *target_imethod = get_virtual_method (imethod, vtable);
		/* Lazily initialize the method table slot */
		mono_mem_manager_lock (memory_manager);
		if (!table[offset]) {
			if (imethod->method->is_inflated || offset < 0)
				table[offset] = append_imethod (memory_manager, NULL, imethod, target_imethod);
			else
				table[offset] = (gpointer) ((gsize) target_imethod | 0x1);
		}
		mono_mem_manager_unlock (memory_manager);
	}

	if ((gsize) table[offset] & 0x1) {
		/* Non generic virtual call. Only one method in slot */
		return (InterpMethod *) ((gsize) table[offset] & ~0x1);
	} else {
		/* Virtual generic or interface call. Multiple methods in slot */
		InterpMethod *target_imethod = get_target_imethod ((GSList *) table[offset], imethod);

		if (!target_imethod) {
			target_imethod = get_virtual_method (imethod, vtable);
			mono_mem_manager_lock (memory_manager);
			if (!get_target_imethod ((GSList *) table[offset], imethod))
				table[offset] = append_imethod (memory_manager, (GSList *) table[offset], imethod,
				                                target_imethod);
			mono_mem_manager_unlock (memory_manager);
		}
		return target_imethod;
	}
}

MONO_INTERP_OP_IMPL (MINT_CALLVIRT_FAST)
{
	cmethod = (InterpMethod *) frame->imethod->data_items[ip[2]];
	call_args_offset = ip[1];
	auto this_arg = LOCAL_VAR (call_args_offset, MonoObject *);
	auto slot = (gint16) ip[3];

	MONO_INTERP_OP_ADVANCE ();

	cmethod = get_virtual_method_fast (cmethod, this_arg->vtable, slot);
	if (m_class_is_valuetype (this_arg->vtable->klass)
	    && m_class_is_valuetype (cmethod->method->klass)) {
		/* unbox */
		gpointer unboxed = mono_object_unbox_internal (this_arg);
		LOCAL_VAR (call_args_offset, gpointer) = unboxed;
	}

	return &exec_call;
}

MONO_INTERP_OP_IMPL (MINT_TAILCALLVIRT_FAST)
{
	cmethod = (InterpMethod *) frame->imethod->data_items[ip[2]];
	call_args_offset = ip[1];
	auto this_arg = LOCAL_VAR (call_args_offset, MonoObject *);
	auto slot = (gint16) ip[3];
	tail_args_size = ip[4];

	MONO_INTERP_OP_ADVANCE ();

	cmethod = get_virtual_method_fast (cmethod, this_arg->vtable, slot);
	if (m_class_is_valuetype (this_arg->vtable->klass)
	    && m_class_is_valuetype (cmethod->method->klass)) {
		/* unbox */
		gpointer unboxed = mono_object_unbox_internal (this_arg);
		LOCAL_VAR (call_args_offset, gpointer) = unboxed;
	}

	return &exec_tailcall;
}

MONO_INTERP_OP_IMPL (MINT_CALL_VARARG)
{
	// Same as MINT_CALL, except at ip [3] we have the index for the csignature,
	// which is required by the called method to set up the arglist.
	cmethod = (InterpMethod *) frame->imethod->data_items[ip[2]];
	call_args_offset = ip[1];

	MONO_INTERP_OP_ADVANCE ();
	return &exec_call;
}

MONO_INTERP_OP_IMPL (MINT_CALLVIRT)
{
	// FIXME CALLVIRT opcodes are not used on netcore. We should kill them.
	cmethod = (InterpMethod *) frame->imethod->data_items[ip[2]];
	call_args_offset = ip[1];

	MonoObject *this_arg = LOCAL_VAR (call_args_offset, MonoObject *);

	cmethod = get_virtual_method (cmethod, this_arg->vtable);
	if (m_class_is_valuetype (this_arg->vtable->klass)
	    && m_class_is_valuetype (cmethod->method->klass)) {
		/* unbox */
		gpointer unboxed = mono_object_unbox_internal (this_arg);
		LOCAL_VAR (call_args_offset, gpointer) = unboxed;
	}

	MONO_INTERP_OP_ADVANCE ();
	return &exec_call;
}

MONO_INTERP_OP_IMPL (MINT_TAILCALL)
{
	cmethod = (InterpMethod *) frame->imethod->data_items[ip[2]];
	call_args_offset = ip[1];
	tail_args_size = ip[3];

	MONO_INTERP_OP_ADVANCE ();
	return &exec_tailcall;
}

MONO_INTERP_OP_IMPL (MINT_CALL)
{
	cmethod = (InterpMethod *) frame->imethod->data_items[ip[2]];
	call_args_offset = ip[1];

	MONO_INTERP_OP_ADVANCE ();
	return &exec_call;
}

// The transform picks this for a method the runtime implements itself rather than
// in IL.
MONO_INTERP_OP_IMPL (MINT_CALLRUN)
{
#ifndef ENABLE_NETCORE
	auto target_method = (MonoMethod *) frame->imethod->data_items[ip[2]];
	auto sig = (MonoMethodSignature *) frame->imethod->data_items[ip[3]];

	if (MonoException *ex =
	        mono_interp_ves_imethod (frame, target_method, sig, (stackval *) (locals + ip[1])))
		THROW_EX (ex, ip);
#else
	g_assert_not_reached ();
#endif

	MONO_INTERP_OP_ADVANCE ();
	MONO_INTERP_DISPATCH ();
}

MONO_INTERP_OP_IMPL (MINT_JIT_CALL)
{
	auto rmethod = (InterpMethod *) frame->imethod->data_items[ip[2]];

	error_init_reuse (error);
	/* for calls, have ip pointing at the start of next instruction */
	frame->state.ip = ip + 3;
	do_jit_call ((stackval *) (locals + ip[1]), frame, rmethod, error);
	if (!is_ok (error))
		THROW_EX (mono_error_convert_to_exception (error), ip);

	CHECK_RESUME_STATE (context);

	MONO_INTERP_OP_ADVANCE ();
	MONO_INTERP_DISPATCH ();
}

MONO_INTERP_OP_IMPL (MINT_JIT_CALL2)
{
	g_error ("MINT_JIT_CALL2 shouldn't be used");
}

/*
 * The variable arguments were pushed by the caller, so their layout is described by
 * the signature at its call site rather than by this method's own. The caller is
 * suspended at the instruction after the call, which is what the walk back to
 * MINT_CALL_VARARG counts from.
 */
MONO_INTERP_OP_IMPL (MINT_INIT_ARGLIST)
{
	const guint16 *call_ip = frame->parent->state.ip - 5;
	g_assert_checked (*call_ip == MINT_CALL_VARARG);

	int params_stack_size = call_ip[4];
	auto sig = (MonoMethodSignature *) frame->parent->imethod->data_items[call_ip[3]];

	// we are being overly conservative with the size here, for simplicity
	gpointer arglist = frame_data_allocator_alloc (&context->data_stack, frame,
	                                               params_stack_size + MINT_STACK_SLOT_SIZE);

	mono_interp_init_arglist (frame, sig, STACK_ADD_BYTES (frame->stack, ip[2]), (char *) arglist);

	// save the arglist for future access with MINT_ARGLIST
	LOCAL_VAR (ip[1], gpointer) = arglist;

	MONO_INTERP_OP_ADVANCE ();
	MONO_INTERP_DISPATCH ();
}

/*
 * An icall with no wrapper. The opcode names the arity and whether a value comes
 * back, which is what picks the prototype the target is called through.
 */
#define IMPL_ICALL(opcode)                                                             \
	MONO_INTERP_OP_IMPL (opcode)                                                       \
	{                                                                                  \
		/* for calls, have ip pointing at the start of next instruction */             \
		frame->state.ip = ip + 3;                                                      \
		mono_interp_do_icall (frame, nullptr, opcode, (stackval *) (locals + ip[1]),   \
		                      frame->imethod->data_items[ip[2]], FALSE);               \
		EXCEPTION_CHECKPOINT_GC_UNSAFE;                                                \
		CHECK_RESUME_STATE (context);                                                  \
                                                                                       \
		MONO_INTERP_OP_ADVANCE ();                                                     \
		MONO_INTERP_DISPATCH ();                                                       \
	}

IMPL_ICALL (MINT_ICALL_V_V);
IMPL_ICALL (MINT_ICALL_V_P);
IMPL_ICALL (MINT_ICALL_P_V);
IMPL_ICALL (MINT_ICALL_P_P);
IMPL_ICALL (MINT_ICALL_PP_V);
IMPL_ICALL (MINT_ICALL_PP_P);
IMPL_ICALL (MINT_ICALL_PPP_V);
IMPL_ICALL (MINT_ICALL_PPP_P);
IMPL_ICALL (MINT_ICALL_PPPP_V);
IMPL_ICALL (MINT_ICALL_PPPP_P);
IMPL_ICALL (MINT_ICALL_PPPPP_V);
IMPL_ICALL (MINT_ICALL_PPPPP_P);
IMPL_ICALL (MINT_ICALL_PPPPPP_V);
IMPL_ICALL (MINT_ICALL_PPPPPP_P);

MONO_INTERP_OP_IMPL (MINT_MONO_RETOBJ)
{
	MonoMethodSignature *sig = mono_method_signature_internal (frame->imethod->method);

	stackval_from_data (sig->ret, frame->stack, LOCAL_VAR (ip[1], gpointer), sig->pinvoke);
	frame_data_allocator_pop (&context->data_stack, frame);

	return &exec_exit_frame;
}

MONO_INTERP_OP_IMPL (MINT_LDFTN)
{
	error_init_reuse (error);
	LOCAL_VAR (ip[1], gpointer) = mono_interp_entry_for_imethod (
		(InterpMethod *) frame->imethod->data_items[ip[2]], error);
	mono_error_assert_ok (error);

	MONO_INTERP_OP_ADVANCE ();
	MONO_INTERP_DISPATCH ();
}

// The address this produces is handed to native code, so it has to be the one a
// patcher would write over rather than an entry of this engine's own.
MONO_INTERP_OP_IMPL (MINT_LDFTN_DYNAMIC)
{
	error_init_reuse (error);
	InterpMethod *m =
		mono_interp_get_imethod (mono_domain_get (), LOCAL_VAR (ip[2], MonoMethod *), error);
	mono_error_assert_ok (error);

	LOCAL_VAR (ip[1], gpointer) = mono_interp_escaping_entry_for_imethod (m, error);
	mono_error_assert_ok (error);

	MONO_INTERP_OP_ADVANCE ();
	MONO_INTERP_DISPATCH ();
}

MONO_INTERP_OP_IMPL (MINT_LDVIRTFTN)
{
	auto m = (InterpMethod *) frame->imethod->data_items[ip[3]];
	auto o = LOCAL_VAR (ip[2], MonoObject *);
	NULL_CHECK (o);

	error_init_reuse (error);
	LOCAL_VAR (ip[1], gpointer) =
		mono_interp_entry_for_imethod (get_virtual_method (m, o->vtable), error);
	mono_error_assert_ok (error);

	MONO_INTERP_OP_ADVANCE ();
	MONO_INTERP_DISPATCH ();
}

MONO_INTERP_OP_IMPL (MINT_LD_DELEGATE_METHOD_PTR)
{
	auto del = LOCAL_VAR (ip[2], MonoDelegate *);

	if (!del->interp_method) {
		/* Not created from interpreted code */
		error_init_reuse (error);
		g_assert (del->method);
		del->interp_method =
			mono_interp_get_imethod (del->object.vtable->domain, del->method, error);
		mono_error_assert_ok (error);
	}

	g_assert (del->interp_method);
	LOCAL_VAR (ip[1], gpointer) = del->interp_method;

	MONO_INTERP_OP_ADVANCE ();
	MONO_INTERP_DISPATCH ();
}

} // namespace mono::interp

#include "config.h"
#include "runtime/frame.hpp"
#include "runtime/method.hpp"
#include "runtime/stackval.hpp"

#include "mono/interp/interp.hpp"
#include "mono/metadata/handle.h"
#include "mono/metadata/marshal.h"
#include "mono/metadata/object-internals.h"
#include "mono/mini/llvm-runtime.h"
#include "mono/mini/mini.h"
#include "mono/utils/mono-compiler.h"
#include "mono/utils/mono-context.h"
#include "mono/utils/mono-threads-coop.h"
#include <cstddef>
#include <llvm/Support/Error.h>
#include <llvm/Support/raw_ostream.h>

namespace mono::interp {

const InterpState::OpFunc InterpState::optable[] = {
#define OPDEF(name, b, c, d, e, f) &InterpState::entry_##name,
#include "mintops.def"
#undef OPDEF
};

MONO_INTERP_ENTRY (exec_method, start);

MONO_ALWAYS_INLINE InterpState::OpFunc
InterpState::start ()
{
	MonoException *ex;
	if (method_entry (context, frame, &ex)) {
		if (ex)
			THROW_EX (ex, NULL);
		EXCEPTION_CHECKPOINT;
	}

	// A clause_args entry runs a handler of a frame that is already executing, and the
	// allocation that frame was given still stands.
	if (!clause_args) {
		context->stack_pointer =
			reinterpret_cast<guchar *> (frame->stack) + frame->imethod->alloca_size;
		/* Make sure the stack pointer is bumped before we store any references on the stack */
		mono_compiler_barrier ();
	}

	INIT_INTERP_STATE (frame, clause_args);
	context->current_frame = frame;

	if (clause_args && clause_args->filter_exception) {
		// Write the exception on to the first slot on the excecution stack
		LOCAL_VAR (frame->imethod->total_locals_size, MonoException *) =
			clause_args->filter_exception;
	}

#if defined(ENABLE_HYBRID_SUSPEND) || defined(ENABLE_COOP_SUSPEND)
	mono_threads_safepoint ();
#endif

	MONO_INTERP_DISPATCH ();
}

#ifdef MONO_MUSTTAIL
void
#else
void *
#endif
InterpState::exec_invalid_opcode (InterpState *state)
{
	std::string message;
	llvm::raw_string_ostream stream (message);
	stream << llvm::format ("invalid opcode: %04x at 0x%x", *state->ip,
	                        (unsigned) (state->ip - state->frame->imethod->code));
	llvm::reportFatalInternalError (message.c_str ());
}

void
InterpState::interp_throw (MonoException *ex, const guint16 *ip, bool rethrow)
{
	{
		ERROR_DECL (error);
		auto guard = LMFGuard (frame);

		// When explicitly throwing exceptions we pass the ip of the instruction that
		// throws the exception. This offsets the subtraction from interp_frame_get_ip, so
		// we don't end up in the previous instruction.
		frame->state.ip = ip + 1;

		if (mono_object_isinst_checked (reinterpret_cast<MonoObject *> (ex),
		                                mono_defaults.exception_class, error)) {
			MonoException *mono_ex = ex;
			if (!rethrow) {
				mono_ex->stack_trace = nullptr;
				mono_ex->trace_ips = nullptr;
			}
		}

		mono_error_assert_ok (error);

		MonoContext ctx;
		std::memset (&ctx, 0, sizeof (MonoContext));
		/*
		 * What the resume compares against to decide which invocations it jumps
		 * over, so it has to be this invocation's native anchor. A frame is not on
		 * this stack.
		 */
		MONO_CONTEXT_SET_SP (&ctx, mono_interp_invocation_anchor (context));

		// Call the JIT EH code. The EH code will call back to us using
		// mono_interp_set_resume_state/run_finally/run_filter.
		// Since ctx.ip is 0, this will start unwinding from the LMF frame pushed above,
		// which points to our frames.
		mono_handle_exception (&ctx, reinterpret_cast<MonoObject *> (ex));
		if (MONO_CONTEXT_GET_IP (&ctx) != 0) {
			/* We need to unwind into non-interpreter code */
			mono_restore_context (&ctx);
			g_assert_not_reached ();
		}
	}

	g_assert (context->has_resume_state);
}

MONO_INTERP_ENTRY (exec_resume, resume);

static void
clear_resume_state (ThreadContext *context)
{
	context->has_resume_state = 0;
	context->handler_frame = NULL;
	context->handler_ei = NULL;
	g_assert (context->exc_gchandle);
	mono_gchandle_free_internal (context->exc_gchandle);
	context->exc_gchandle = 0;
}

InterpState::OpFunc MONO_ALWAYS_INLINE
InterpState::resume ()
{
	g_assert (context->has_resume_state);
	g_assert (frame->imethod);

	if (frame == context->handler_frame) {
		/*
		 * When running finally blocks, we can have the same frame twice on the stack. If we have
		 * clause_args information, we need to check whether resuming should happen inside this
		 * finally block, or in some other part of the method, in which case we need to exit.
		 */
		if (clause_args && frame == clause_args->exec_frame
		    && context->handler_ip >= clause_args->end_at_ip)
			return &exec_exit;

		/* Set the current execution state to the resume state in context */
		ip = context->handler_ip;
		/* spec says stack should be empty at endfinally so it should be at the start too */
		locals = reinterpret_cast<guchar *> (frame->stack);
		g_assert (context->exc_gchandle);
		// Write the exception on to the first slot on the excecution stack
		LOCAL_VAR (frame->imethod->total_locals_size, MonoObject *) =
			mono_gchandle_get_target_internal (context->exc_gchandle);

		clear_resume_state (context);
		/* The resume site cleared the marker if it skipped frames, so put it back. */
		context->current_frame = frame;

		MONO_INTERP_DISPATCH ();
	} else if (clause_args && frame == clause_args->exec_frame) {
		/*
		 * This frame doesn't handle the resume state and it is the first frame invoked from EH.
		 * We can't just return to parent. We must first exit the EH mechanism and start resuming
		 * again from the original frame.
		 */
		return &exec_exit;
	}

	// Because we are resuming in another frame, bypassing a normal ret opcode,
	// we need to make sure to reset the localloc stack
	frame_data_allocator_pop (&context->data_stack, frame);

	return &exec_exit_frame;
}

MONO_INTERP_ENTRY (exec_exit, exit);

#ifdef MONO_MUSTTAIL
static void
real_exit (InterpState *state)
{
}
#endif

InterpState::OpFunc MONO_ALWAYS_INLINE
InterpState::exit ()
{
	// Make sure the return value stays below the stack pointer
	if (!clause_args)
		context->stack_pointer =
			reinterpret_cast<guchar *> (frame->stack) + frame->imethod->alloca_size;

	/* Our frames go away with this invocation, so hand the marker back to the one below. */
	context->current_frame = outer_current_frame;
	context->frame_stack_pointer = frame_watermark;

	/*
	 * Truncating rather than decrementing is what makes this self-healing: an entry
	 * above ours belongs to an invocation that is already gone, and this is where it
	 * stops being remembered.
	 */
	context->handle_mark_count = handle_mark_depth;

#ifdef MONO_MUSTTAIL
	return &real_exit;
#else
	return nullptr;
#endif
}

} // namespace mono::interp

/*
 * The entry points below take a caller that is not the interpreter into it. Each
 * builds the first frame of one invocation, and that frame is a local here: a root
 * frame has no parent to hold it, and the conservative scan of this stack is what
 * roots the code it names.
 */

using namespace mono::interp;

void
mono_interp_exec_method (InterpFrame *frame, ThreadContext *context, FrameClauseArgs *clause_args)
{
	HANDLE_FUNCTION_ENTER ();

	/*
	 * The error is not used to report anything back. It lives here so that an opcode
	 * that needs one does not have to declare it, and so that it outlives every frame
	 * the dispatch chain runs through.
	 */
	ERROR_DECL (error);

	/* Recorded where a resume past this frame can still find it. */
	int handle_mark_depth = mono_interp_push_handle_mark (context, &__mark, frame);

	InterpState state (frame, context, clause_args, error, mono_thread_info_current_var,
	                   handle_mark_depth);
	state.exec ();

	HANDLE_FUNCTION_RETURN ();
}

MonoObject *
mono_interp_runtime_invoke (MonoMethod *method, void *obj, void **params, MonoObject **exc,
                            MonoError *error)
{
	ThreadContext *context = mono_interp_get_context ();
	MonoMethodSignature *sig = mono_method_signature_internal (method);
	stackval *sp = reinterpret_cast<stackval *> (context->stack_pointer);
	MonoMethod *target_method = method;

	error_init (error);
	if (exc)
		*exc = NULL;

	MonoDomain *domain = mono_domain_get ();

	if (method->flags & METHOD_ATTRIBUTE_PINVOKE_IMPL)
		target_method = mono_marshal_get_native_wrapper (target_method, FALSE, FALSE);
	MonoMethod *invoke_wrapper = mono_marshal_get_runtime_invoke_full (target_method, FALSE, TRUE);

	//* <code>MonoObject *runtime_invoke (MonoObject *this_obj, void **params, MonoObject **exc, void* method)</code>

	if (sig->hasthis)
		sp[0].data.p = obj;
	else
		sp[0].data.p = NULL;
	sp[1].data.p = params;
	sp[2].data.p = exc;
	sp[3].data.p = target_method;

	InterpMethod *imethod = mono_interp_get_imethod (domain, invoke_wrapper, error);
	mono_error_assert_ok (error);

	InterpFrame frame = {};
	frame.imethod = imethod;
	frame.stack = sp;
	frame_stamp_ordinal (context, &frame);
	frame_root_code_owner (&frame);

	// The method to execute might not be transformed yet, so we don't know how much stack
	// it uses. We bump the stack_pointer here so any code triggered by method compilation
	// will not attempt to use the space that we used to push the args for this method.
	// The real top of stack for this method will be set in mono_interp_exec_method once the
	// method is transformed.
	context->stack_pointer = reinterpret_cast<guchar *> ((sp + 4));

	mono_interp_exec_method (&frame, context, NULL);

	if (context->has_resume_state) {
		/*
		 * This can happen on wasm where native frames cannot be skipped during EH.
		 * EH processing will continue when control returns to the interpreter.
		 */
		context->stack_pointer = reinterpret_cast<guchar *> (sp);
		return NULL;
	}

	// The return value is at the bottom of the stack
	MonoObject *result = frame.stack->data.o;

	/*
	 * A C local is covered by the conservative thread stack scan, so reading the value
	 * out first hands it to a scanner that keeps it before the interpreter one stops
	 * covering it. The barrier is what keeps the compiler from doing these two in the
	 * other order.
	 */
	mono_compiler_barrier ();
	context->stack_pointer = reinterpret_cast<guchar *> (sp);
	return result;
}

void
mono_interp_entry (InterpEntryData *data)
{
	gpointer orig_domain = NULL, attach_cookie;

	if ((gsize) data->rmethod & 1) {
		/* Unbox */
		data->this_arg = mono_object_unbox_internal (static_cast<MonoObject *> (data->this_arg));
		data->rmethod = static_cast<InterpMethod *> ((gpointer) ((gsize) data->rmethod & ~1));
	}
	InterpMethod *rmethod = data->rmethod;

	if (rmethod->needs_thread_attach)
		orig_domain = mono_threads_attach_coop (mono_domain_get (), &attach_cookie);

	/* After the attach, so asking for the method cannot block on a thread the
	 * collector does not know about. */
	if (G_UNLIKELY (mono_atomic_load_i32_relaxed (&rmethod->tier_counter) > 0))
		interp_check_call_promotion (rmethod);

	ThreadContext *context = mono_interp_get_context ();
	stackval *sp_args, *sp;
	sp_args = sp = reinterpret_cast<stackval *> (context->stack_pointer);

	MonoMethod *method = rmethod->method;
	MonoMethodSignature *sig = mono_method_signature_internal (method);

	// FIXME: Optimize this

	if (sig->hasthis) {
		sp_args->data.p = data->this_arg;
		sp_args++;
	}

	gpointer *params = data->many_args ? data->many_args : data->args;
	for (int i = 0; i < sig->param_count; ++i) {
		if (sig->params[i]->byref) {
			sp_args->data.p = params[i];
			sp_args++;
		} else {
			int size = stackval_from_data (sig->params[i], sp_args, params[i], FALSE);
			sp_args = STACK_ADD_BYTES (sp_args, size);
		}
	}

	InterpFrame frame = {};
	frame.imethod = rmethod;
	frame.stack = sp;
	frame_stamp_ordinal (context, &frame);
	frame_root_code_owner (&frame);

	context->stack_pointer = reinterpret_cast<guchar *> (sp_args);

	/*
	 * Building the exception a checkpoint decided to raise can call back in here,
	 * and this activation's frames are reachable through the LMF the entry pushed.
	 * The frame the outer one published is not ours to report while we run.
	 */
	InterpFrame *outer_stopped_frame = context->safepoint_frame;
	context->safepoint_frame = NULL;

	mono_interp_exec_method (&frame, context, NULL);

	g_assert (!context->has_resume_state);
	g_assert (!context->safepoint_frame);
	context->safepoint_frame = outer_stopped_frame;

	/*
	 * The detach below is a GC transition and can wait out a whole collection, so the
	 * return value has to stay inside the scanned range until it has been copied out.
	 */
	if (rmethod->needs_thread_attach)
		mono_threads_detach_coop (orig_domain, &attach_cookie);

	if (mono_llvm_only) {
		if (context->has_resume_state) {
			context->stack_pointer = reinterpret_cast<guchar *> (sp);
			mono_llvm_reraise_exception (reinterpret_cast<MonoException *> (
				mono_gchandle_get_target_internal (context->exc_gchandle)));
		}
	} else {
		g_assert (!context->has_resume_state);
	}

	// The return value is at the bottom of the stack, after the locals space
	MonoType *type = rmethod->rtype;
	if (type->type != MONO_TYPE_VOID)
		stackval_to_data (type, frame.stack, data->res, FALSE);

	context->stack_pointer = reinterpret_cast<guchar *> (sp);
}

void
mono_interp_entry_general (gpointer this_arg, gpointer res, gpointer *args, gpointer rmethod)
{
	InterpEntryData data;

	data.rmethod = static_cast<InterpMethod *> (rmethod);
	data.res = res;
	data.this_arg = this_arg;
	data.many_args = args;

	mono_interp_entry (&data);
}

void
mono_interp_entry_from_args (gpointer imethod, gpointer this_arg, gpointer res, gpointer *args)
{
	mono_interp_entry_general (this_arg, res, args, imethod);
}

#ifdef MONO_ARCH_HAVE_INTERP_ENTRY_TRAMPOLINE

void
mono_interp_entry_from_ccontext (gpointer ccontext_untyped, gpointer rmethod_untyped)
{
	CallContext *ccontext = static_cast<CallContext *> (ccontext_untyped);
	InterpMethod *rmethod = static_cast<InterpMethod *> (rmethod_untyped);
	gpointer orig_domain = NULL, attach_cookie;

	if (rmethod->needs_thread_attach)
		orig_domain = mono_threads_attach_coop (mono_domain_get (), &attach_cookie);

	ThreadContext *context = mono_interp_get_context ();
	stackval *sp = reinterpret_cast<stackval *> (context->stack_pointer);

	MonoMethod *method = rmethod->method;
	MonoMethodSignature *sig = mono_method_signature_internal (method);
	if (method->string_ctor) {
		MonoMethodSignature *newsig = (MonoMethodSignature *) g_alloca (
			MONO_SIZEOF_METHOD_SIGNATURE + ((sig->param_count + 2) * sizeof (MonoType *)));
		std::memcpy (newsig, sig, mono_metadata_signature_size (sig));
		newsig->ret = m_class_get_byval_arg (mono_defaults.string_class);
		sig = newsig;
	}

	InterpFrame frame = {};
	frame.imethod = rmethod;
	frame.stack = sp;
	frame_stamp_ordinal (context, &frame);
	frame_root_code_owner (&frame);

	/* Copy the args saved in the trampoline to the frame stack */
	gpointer retp = mono_arch_get_native_call_context_args (ccontext, &frame, sig);

	/* Allocate storage for value types */
	stackval *newsp = sp;
	/* FIXME we should reuse computation on imethod for this */
	if (sig->hasthis)
		newsp++;
	for (int i = 0; i < sig->param_count; i++) {
		MonoType *type = sig->params[i];
		int size;

		if (type->type == MONO_TYPE_GENERICINST && !MONO_TYPE_IS_REFERENCE (type)) {
			size = mono_class_value_size (mono_class_from_mono_type_internal (type), NULL);
		} else if (type->type == MONO_TYPE_VALUETYPE) {
			if (sig->pinvoke)
				size = mono_class_native_size (type->data.klass, NULL);
			else
				size = mono_class_value_size (type->data.klass, NULL);
		} else {
			size = MINT_STACK_SLOT_SIZE;
		}
		newsp = STACK_ADD_BYTES (newsp, size);
	}
	context->stack_pointer = reinterpret_cast<guchar *> (newsp);

	mono_interp_exec_method (&frame, context, NULL);

	g_assert (!context->has_resume_state);

	/*
	 * The detach below is a GC transition and can wait out a whole collection, so the
	 * return value has to stay inside the scanned range until it has been copied out.
	 */
	if (rmethod->needs_thread_attach)
		mono_threads_detach_coop (orig_domain, &attach_cookie);

	/* Write back the return value */
	/* 'frame' is still valid */
	mono_arch_set_native_call_context_ret (ccontext, &frame, sig, retp);

	context->stack_pointer = reinterpret_cast<guchar *> (sp);
}

#else

void
mono_interp_entry_from_ccontext (gpointer ccontext_untyped, gpointer rmethod_untyped)
{
	g_assert_not_reached ();
}

#endif /* MONO_ARCH_HAVE_INTERP_ENTRY_TRAMPOLINE */

gboolean
mono_interp_run_finally (StackFrameInfo *frame, int clause_index, gpointer handler_ip,
                         gpointer handler_ip_end)
{
	InterpFrame *iframe = static_cast<InterpFrame *> (frame->interp_frame);
	ThreadContext *context = mono_interp_get_context ();
	FrameClauseArgs clause_args;

	std::memset (&clause_args, 0, sizeof (FrameClauseArgs));
	clause_args.start_with_ip = static_cast<const guint16 *> (handler_ip);
	clause_args.end_at_ip = static_cast<const guint16 *> (handler_ip_end);
	clause_args.exit_clause = clause_index;
	clause_args.exec_frame = iframe;

	const guint16 *state_ip = iframe->state.ip;
	iframe->state.ip = NULL;

	InterpFrame *const next_free = iframe->next_free;
	iframe->next_free = NULL;

	// this informs MINT_ENDFINALLY to return to EH
	*reinterpret_cast<guint16 **> (
		(frame_locals (iframe) + iframe->imethod->clause_data_offsets[clause_index])) = NULL;

	mono_interp_exec_method (iframe, context, &clause_args);

	iframe->next_free = next_free;
	iframe->state.ip = state_ip;

	return context->has_resume_state ? TRUE : FALSE;
}

gboolean
mono_interp_run_filter (StackFrameInfo *frame, MonoException *ex, int clause_index,
                        gpointer handler_ip, gpointer handler_ip_end)
{
	InterpFrame *iframe = static_cast<InterpFrame *> (frame->interp_frame);
	ThreadContext *context = mono_interp_get_context ();
	/*
	 * Only MINT_ENDFILTER writes this, and an exception raised inside the filter
	 * never reaches it. ECMA-335 III.3.34 makes that case continue the search,
	 * which is what zero says here.
	 */
	stackval retval = {};
	FrameClauseArgs clause_args;

	/*
	 * A filter runs before the stack unwinds. Every frame between the throw site
	 * and the clause owner is therefore still live, and a walk taken inside the
	 * filter has to report them. Hang the clause off the innermost frame, which
	 * reaches the owner through those frames instead of over them.
	 *
	 * The innermost frame does not always reach the owner, because the
	 * interpreter did not make every throw. Fall back to the owner then.
	 */
	InterpFrame *innermost = context->current_frame;
	InterpFrame *f;

	for (f = innermost; f && f != iframe; f = f->parent)
		;

	/*
	 * Have to run the clause in a new frame which is a copy of IFRAME, since
	 * during debugging, there are two copies of the frame on the stack.
	 */
	InterpFrame child_frame = {};
	child_frame.parent = f ? innermost : iframe;
	child_frame.imethod = iframe->imethod;
	child_frame.stack = reinterpret_cast<stackval *> (context->stack_pointer);
	child_frame.retval = &retval;
	child_frame.code_owner = iframe->code_owner;
	frame_stamp_ordinal (context, &child_frame);

	/* Copy the stack frame of the original method */
	std::memcpy (child_frame.stack, iframe->stack, iframe->imethod->total_locals_size);
	context->stack_pointer += iframe->imethod->alloca_size;

	std::memset (&clause_args, 0, sizeof (FrameClauseArgs));
	clause_args.start_with_ip = static_cast<const guint16 *> (handler_ip);
	clause_args.end_at_ip = static_cast<const guint16 *> (handler_ip_end);
	clause_args.filter_exception = ex;
	clause_args.exec_frame = &child_frame;

	mono_interp_exec_method (&child_frame, context, &clause_args);

	/* Copy back the updated frame */
	std::memcpy (iframe->stack, child_frame.stack, iframe->imethod->total_locals_size);

	context->stack_pointer = reinterpret_cast<guchar *> (child_frame.stack);

	/* ENDFILTER stores the result into child_frame->retval */
	return retval.data.i ? TRUE : FALSE;
}

#ifndef __MONO_INTERP_INTERP_HPP__
#define __MONO_INTERP_INTERP_HPP__

#include "config.h"
#include "glib.h"
#include "mono/metadata/class-internals.h"
#include "mono/metadata/object-internals.h"
#include "mono/interp/runtime/internals.hpp"
#include "mono/interp/mintops.hpp"
#include "mono/metadata/metadata.h"
#include "mono/mini/mini-runtime.h"
#include "mono/utils/mono-error.h"
#include "mono/utils/mono-compiler.h"
#include <cstdint>
#include <optional>
#include <cstring>

namespace mono::interp {

class InterpState {
public:
	InterpMethod *cmethod;
	MonoError *error;
	InterpFrame *frame;
	ThreadContext *context;
	FrameClauseArgs *clause_args;

	const std::uint16_t *ip;
	unsigned char *locals;
	int call_args_offset;
	int tail_args_size;
	MonoMethodSignature *calli_signature;

	/*
	 * GC SAFETY:
	 *
	 * The interpreter runs in gc unsafe (non-preempt) mode. On wasm we cannot rely on
	 * scanning the stack or any registers. Every objref the code touches must stay alive
	 * and pinned, one of two ways:
	 * - store it on the interpreter stack, in a volatile variable so the compiler cannot
	 *   optimize the store away.
	 * - for an object that does not come from the interp stack, use tmp_handle below. It
	 *   pins one object. Clear it once the object is no longer needed. Reserve additional
	 *   handles here if more than one object must stay pinned at once.
	 */
	MonoObjectHandle tmp_handle;

	/*
	 * Native code can call back into the interpreter, so this invocation is not always the
	 * outermost one. The frames of the outer invocation stay live below ours, and they
	 * become current again when we return.
	 */
	InterpFrame *outer_current_frame;

	/*
	 * Where this invocation's frames start. exit () gives them back. A resume
	 * that jumps over this invocation gives them back too, through the record
	 * mono_interp_push_handle_mark () keeps, because this invocation never runs
	 * again.
	 */
	guchar *frame_watermark;

	/* What context->handle_mark_count goes back to. */
	int handle_mark_depth;

	/*
	 * Sets up one invocation, the way mono_interp_exec_method () sets up its
	 * locals.
	 *
	 * The caller owns three things this constructor cannot: the handle frame,
	 * the MonoError, and the record mono_interp_push_handle_mark () takes of
	 * both. Each must outlive every handler, and no handler frame does.
	 *
	 * The handle frame is HANDLE_FUNCTION_ENTER's, whose mono_thread_info_current_var
	 * is the parameter of that name. It closes with HANDLE_FUNCTION_RETURN after
	 * exit () has run.
	 *
	 * Everything the first opcode needs beyond this is what start () does between
	 * method_entry () and its dispatch. That can throw, so it does not belong in a
	 * constructor.
	 */
	InterpState (InterpFrame *frame, ThreadContext *context, FrameClauseArgs *clause_args,
	             MonoError *error, MonoThreadInfo *mono_thread_info_current_var,
	             int handle_mark_depth)
		: cmethod (nullptr),
		  error (error),
		  frame (frame),
		  context (context),
		  clause_args (clause_args),
		  ip (nullptr),
		  locals (nullptr),
		  // We leave these three for whichever opcode handler sets them. Zeroing costs
		  // nothing, and it makes a site that forgets deterministic rather than random.
		  call_args_offset (0),
		  tail_args_size (0),
		  calli_signature (nullptr),
		  tmp_handle (MONO_HANDLE_NEW (MonoObject, NULL)),
		  outer_current_frame (context->current_frame),
		  frame_watermark (context->frame_stack_pointer),
		  handle_mark_depth (handle_mark_depth)
	{
	}

#ifdef MONO_MUSTTAIL
	void exec () { exec_method (this); }
#else
	void exec ()
	{
		OpFunc next = &exec_method;
		while (next)
			next = (OpFunc) next (this);
	}
#endif

#ifdef ENABLE_INTERP_TRACE
	void trace_op_if_wanted ()
	{
		if (G_UNLIKELY (frame->imethod->tracing))
			trace_op ();
	}

private:
	MONO_NEVER_INLINE void trace_op ();

public:
#else
	void trace_op_if_wanted () {}
#endif

private:
	class LMFGuard {
	private:
		MonoLMFExt ext;

	public:
		LMFGuard (InterpFrame *frame)
		{
			std::memset (&ext, 0, sizeof (ext));
			ext.kind = MONO_LMFEXT_INTERP_EXIT;
			ext.interp_exit_data = frame;

			mono_push_lmf (&ext);
		}

		~LMFGuard () { mono_pop_lmf (&ext.lmf); }
	};

	template<typename F>
	void do_debugger_tramp (F &&tramp)
	{
		LMFGuard guard (frame);
		tramp ();
	}

private:
#ifdef MONO_MUSTTAIL
#define MONO_INTERP_EXEC_DEF(name) static void name (InterpState *state)

	using OpFunc = void (*) (InterpState *);

	static void exec_invalid_opcode (InterpState *state);
#else
#define MONO_INTERP_EXEC_DEF(name) static void *name (InterpState *state)

	using OpFunc = void *(*) (InterpState *);

	static void *exec_invalid_opcode (InterpState *state);
#endif

#define OPDEF(name, b, c, d, e, f)                                                 \
	MONO_INTERP_EXEC_DEF (entry_##name);                                           \
	/* Call this only through its own entry_ function. Any other caller fails to link. */ \
	inline OpFunc exec_##name (MintOpcode opcode);
#include "mintops.def"
#undef OPDEF

	// Used by MONO_INTERP_EXIT as the exit method
	MONO_INTERP_EXEC_DEF (exec_exit);
	MONO_INTERP_EXEC_DEF (exec_exit_frame);
	MONO_INTERP_EXEC_DEF (exec_resume);
	MONO_INTERP_EXEC_DEF (exec_call);
	MONO_INTERP_EXEC_DEF (exec_calli);
	MONO_INTERP_EXEC_DEF (exec_tailcall);
	MONO_INTERP_EXEC_DEF (exec_method);

	// Throw an exception from the interpreter
	void interp_throw (MonoException *ex, const guint16 *ip, bool rethrow);

	// Do not call these directly. Return an exec_* function pointer from your op impl instead.
	inline OpFunc exit_frame ();
	inline OpFunc call ();
	inline OpFunc calli ();
	inline OpFunc tailcall ();
	inline OpFunc resume ();
	inline OpFunc exit ();
	inline OpFunc start ();

	static const OpFunc optable[MINT_LASTOP];
};

#ifdef MONO_MUSTTAIL
#define MONO_INTERP_OP_IMPL(opcode)                       \
	void InterpState::entry_##opcode (InterpState *state) \
	{                                                     \
		state->trace_op_if_wanted ();                     \
		OpFunc next = state->exec_##opcode (opcode);      \
		MONO_MUSTTAIL return next (state);                \
	}                                                     \
	MONO_ALWAYS_INLINE InterpState::OpFunc InterpState::exec_##opcode (MintOpcode _opcode)

// Define an entry point that calls an inner function.
//
// The inner function must return an OpFunc which is the next state to jump to.
#define MONO_INTERP_ENTRY(entry, inner)          \
	void InterpState::entry (InterpState *state) \
	{                                            \
		OpFunc next = state->inner ();           \
		MONO_MUSTTAIL return next (state);       \
	}
#else
#define MONO_INTERP_OP_IMPL(opcode)                        \
	void *InterpState::entry_##opcode (InterpState *state) \
	{                                                      \
		state->trace_op_if_wanted ();                      \
		OpFunc next = state->exec_##opcode (opcode);       \
		return (void *) next;                              \
	}                                                      \
	MONO_ALWAYS_INLINE InterpState::OpFunc InterpState::exec_##opcode (MintOpcode _opcode)

#define MONO_INTERP_ENTRY(entry, inner)           \
	void *InterpState::entry (InterpState *state) \
	{                                             \
		OpFunc next = state->inner ();            \
		return (void *) next;                     \
	}
#endif

#define MONO_INTERP_OP_ADVANCE()               \
	do {                                       \
		this->ip += opinfos[_opcode].oplength; \
	} while (0)

#define MONO_INTERP_DISPATCH()                                       \
	do {                                                             \
		MintOpcode _dispatch_opcode = (MintOpcode) * this->ip;       \
		if (G_UNLIKELY ((unsigned) _dispatch_opcode >= MINT_LASTOP)) \
			return &exec_invalid_opcode;                             \
                                                                     \
		return InterpState::optable[_dispatch_opcode];               \
	} while (0)

#define MONO_INTERP_EXIT return &InterpState::exec_exit

#define LOCAL_VAR(offset, type) (*reinterpret_cast<type *> ((locals + (offset))))

// We conservatively pin the exception object here to avoid tweaking every
// call site of this macro. A few of them do not actually need it.
#define THROW_EX_GENERAL(exception, ex_ip, rethrow)                                       \
	do {                                                                                  \
		MonoException *__ex = (exception);                                                \
		MONO_HANDLE_ASSIGN_RAW (this->tmp_handle, reinterpret_cast<MonoObject *> (__ex)); \
		this->interp_throw (__ex, (ex_ip), (rethrow));                                    \
		MONO_HANDLE_ASSIGN_RAW (this->tmp_handle, nullptr);                               \
		return &InterpState::exec_resume;                                                 \
	} while (0)

#define THROW_EX(exception, ex_ip) THROW_EX_GENERAL ((exception), (ex_ip), FALSE)

#define NULL_CHECK(o)                                            \
	do {                                                         \
		if (G_UNLIKELY (!(o)))                                   \
			THROW_EX (mono_get_exception_null_reference (), ip); \
	} while (0)

/*
 * Deciding whether to raise an abort here walks this thread's stack, and the
 * frames it must see are ours. Once the icall that reached this macro has
 * popped its own LMF, the LMF chain says nothing about them. Publish the
 * frame for the length of the call, the way a safepoint does.
 */
#define EXCEPTION_CHECKPOINT_CALL(exc)                              \
	do {                                                            \
		InterpFrame *prev_stopped_frame = context->safepoint_frame; \
		context->safepoint_frame = frame;                           \
		(exc) = mono_thread_interruption_checkpoint ();             \
		context->safepoint_frame = prev_stopped_frame;              \
	} while (0)

#define EXCEPTION_CHECKPOINT                                                             \
	do {                                                                                 \
		if (G_UNLIKELY (mono_thread_interruption_request_flag                            \
		                && !mono_threads_is_critical_method (frame->imethod->method))) { \
			MonoException *exc;                                                          \
			EXCEPTION_CHECKPOINT_CALL (exc);                                             \
			if (exc)                                                                     \
				THROW_EX (exc, ip);                                                      \
		}                                                                                \
	} while (0)

/* Do not throw while the thread is in GC-safe mode. That happens only inside a managed-to-native wrapper. */
#define EXCEPTION_CHECKPOINT_GC_UNSAFE                                               \
	do {                                                                             \
		if (G_UNLIKELY (mono_thread_interruption_request_flag                        \
		                && !mono_threads_is_critical_method (frame->imethod->method) \
		                && mono_thread_is_gc_unsafe_mode ())) {                      \
			MonoException *exc;                                                      \
			EXCEPTION_CHECKPOINT_CALL (exc);                                         \
			if (exc)                                                                 \
				THROW_EX (exc, ip);                                                  \
		}                                                                            \
	} while (0)

/* Save the interpreter's execution state into frame. */
#define SAVE_INTERP_STATE(frame) \
	do {                         \
		frame->state.ip = ip;    \
	} while (0)

/* Load and clear the interpreter's execution state from frame. */
#define LOAD_INTERP_STATE(frame)                 \
	do {                                         \
		ip = frame->state.ip;                    \
		locals = (unsigned char *) frame->stack; \
		frame->state.ip = NULL;                  \
	} while (0)

/* Initialize the interpreter's execution state for frame. */
#define INIT_INTERP_STATE(frame, _clause_args)                                             \
	do {                                                                                   \
		ip = _clause_args ? (static_cast<FrameClauseArgs *> (_clause_args))->start_with_ip \
		                  : (frame)->imethod->code;                                        \
		locals = (unsigned char *) (frame)->stack;                                         \
	} while (0)

/*
 * If this bit is set, the call threw an exception. We reached this point
 * because mono_handle_exception ()'s EH code unwound every JIT frame below
 * us. interp_set_resume_state () has already set the fields in context that
 * say where to resume.
 */
#define CHECK_RESUME_STATE(context)                   \
	do {                                              \
		if (G_UNLIKELY ((context)->has_resume_state)) \
			return &InterpState::exec_resume;         \
	} while (0)

} // namespace mono::interp

#endif

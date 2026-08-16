#ifndef MONO_MINI_INTERP_OPCODES_DISPATCH_HPP
#define MONO_MINI_INTERP_OPCODES_DISPATCH_HPP

#include "glib.h"
#include "mono/metadata/class-internals.h"
#include "mono/metadata/object-internals.h"
#include "mono/mini/interp/musttail.hpp"
#include "mono/mini/interp/interp-internals.h"
#include "mono/mini/interp/mintops.h"
#include "mono/metadata/metadata.h"
#include "mono/mini/mini-runtime.h"
#include "mono/utils/mono-error.h"
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
	 *  The interpreter executes in gc unsafe (non-preempt) mode. On wasm, we cannot rely on
	 * scanning the stack or any registers. In order to make the code GC safe, every objref
	 * handled by the code needs to be kept alive and pinned in any of the following ways:
	 * - the object needs to be stored on the interpreter stack. In order to make sure the
	 * object actually gets stored on the interp stack and the store is not optimized out,
	 * the store/variable should be volatile.
	 * - if the execution of an opcode requires an object not coming from interp stack to be
	 * kept alive, the tmp_handle below can be used. This handle will keep only one object
	 * pinned by the GC. Ideally, once this object is no longer needed, the handle should be
	 * cleared. If we will need to have more objects pinned simultaneously, additional handles
	 * can be reserved here.
	 */
	MonoObjectHandle tmp_handle;

	/*
	 * Native code can call back into the interpreter, so this invocation is not always the
	 * outermost one. The frames of the outer invocation stay live below ours, and they
	 * become current again when we return.
	 */
	InterpFrame *outer_current_frame;

	/*
	 * Where this invocation's frames start. exit () gives them back, and a resume
	 * that jumps over this invocation gives them back through the record
	 * interp_push_handle_mark () keeps, because nothing here runs again.
	 */
	guchar *frame_watermark;

	/* What context->handle_mark_count goes back to. */
	int handle_mark_depth;

	/*
	 * Sets up one invocation, the way interp_exec_method () sets up its locals.
	 *
	 * The caller owns three things this cannot, because each has to outlive every
	 * handler and no handler frame does: the handle frame (HANDLE_FUNCTION_ENTER,
	 * whose mono_thread_info_current_var is the parameter of that name, and
	 * HANDLE_FUNCTION_RETURN after exit () has run), the MonoError, and the record
	 * interp_push_handle_mark () takes of both.
	 *
	 * Everything the first opcode needs beyond this is what interp_exec_method ()
	 * does between method_entry () and main_loop, and that can throw, so it does not
	 * belong in a constructor.
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
		  // interp.c leaves these three to whichever call site sets them. Zeroing costs
		  // nothing and makes a site that forgets deterministic rather than random.
		  call_args_offset (0),
		  tail_args_size (0),
		  calli_signature (nullptr),
		  tmp_handle (MONO_HANDLE_NEW (MonoObject, NULL)),
		  outer_current_frame (context->current_frame),
		  frame_watermark (context->frame_stack_pointer),
		  handle_mark_depth (handle_mark_depth)
	{
	}

	// Execute the method that has been set up for this interpreter.
	void exec () { exec_method (this); }

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
	using OpFunc = void (*) (InterpState *);

#define OPDEF(name, b, c, d, e, f)                                                 \
	static void entry_##name (InterpState *state);                                 \
	/* this should only be called by entry_, otherwise you'll get linker errors */ \
	MONO_ALWAYS_INLINE inline OpFunc exec_##name (MintOpcode opcode);
#include "mintops.def"
#undef OPDEF

	static void exec_invalid_opcode (InterpState *state);

	// Used by MONO_INTERP_EXIT as the exit method
	static void exec_exit (InterpState *state);
	static void exec_exit_frame (InterpState *state);
	static void exec_resume (InterpState *state);
	static void exec_call (InterpState *state);
	static void exec_calli (InterpState *state);
	static void exec_tailcall (InterpState *state);
	static void exec_method (InterpState *state);

	// Throw an exception from the interpreter
	void interp_throw (MonoException *ex, const guint16 *ip, bool rethrow);

	// Don't call these directly, return an exec_* function pointer from your op impl instead.
	inline OpFunc exit_frame ();
	inline OpFunc call ();
	inline OpFunc calli ();
	inline OpFunc tailcall ();
	inline OpFunc resume ();
	inline OpFunc exit ();
	inline OpFunc start ();

	static const OpFunc optable[MINT_LASTOP];

	struct OpInfo {
		const char *opstring;
		std::uint8_t oplength;
		std::optional<uint8_t> num_dregs;
		std::uint8_t num_sregs;
		MintOpArgType optype;
	};

	static constexpr OpInfo opinfos[MINT_LASTOP] = {
#define CallArgs std::nullopt
#define OPDEF(opsymbol, opstring, oplength, num_dregs, num_sregs, optype) \
	OpInfo{opstring, oplength, num_dregs, num_sregs, optype},
#include "mintops.def"
#undef OPDEF
	};
};

#define MONO_INTERP_OP_IMPL(opcode)                       \
	void InterpState::entry_##opcode (InterpState *state) \
	{                                                     \
		OpFunc next = state->exec_##opcode (opcode);      \
		MONO_MUSTTAIL return next (state);                \
	}                                                     \
	InterpState::OpFunc InterpState::exec_##opcode (MintOpcode _opcode)

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

// Define an entry point that calls an inner function.
//
// The inner function must return a OpFunc which is the next state to jump to.
#define MONO_INTERP_ENTRY(entry, inner)          \
	void InterpState::entry (InterpState *state) \
	{                                            \
		OpFunc next = state->inner ();           \
		MONO_MUSTTAIL return next (state);       \
	}

// Used when implementing a custom entry point
#define MONO_INTERP_CONTINUE(next)

#define LOCAL_VAR(offset, type) (*(type *) (locals + (offset)))

// We conservatively pin exception object here to avoid tweaking the
// numerous call sites of this macro, even though, in a few cases,
// this is not needed.
#define THROW_EX_GENERAL(exception, ex_ip, rethrow)                     \
	do {                                                                \
		MonoException *__ex = (exception);                              \
		MONO_HANDLE_ASSIGN_RAW (this->tmp_handle, (MonoObject *) __ex); \
		this->interp_throw (__ex, (ex_ip), (rethrow));                  \
		MONO_HANDLE_ASSIGN_RAW (this->tmp_handle, (MonoObject *) NULL); \
		return &InterpState::exec_resume;                               \
	} while (0)

#define THROW_EX(exception, ex_ip) THROW_EX_GENERAL ((exception), (ex_ip), FALSE)

#define NULL_CHECK(o)                                            \
	do {                                                         \
		if (G_UNLIKELY (!(o)))                                   \
			THROW_EX (mono_get_exception_null_reference (), ip); \
	} while (0)

/*
 * Deciding whether to raise an abort here means walking this thread's stack, and
 * the frames it has to see are ours - the LMF chain says nothing about them once
 * the icall that got us here has popped its own. Publish the frame for the length
 * of the call, the same way a safepoint does.
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

/* Don't throw exception if thread is in GC Safe mode. Should only happen in managed-to-native wrapper. */
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

/* Save the state of the interpeter main loop into FRAME */
#define SAVE_INTERP_STATE(frame) \
	do {                         \
		frame->state.ip = ip;    \
	} while (0)

/* Load and clear state from FRAME */
#define LOAD_INTERP_STATE(frame)                 \
	do {                                         \
		ip = frame->state.ip;                    \
		locals = (unsigned char *) frame->stack; \
		frame->state.ip = NULL;                  \
	} while (0)

/* Initialize interpreter state for executing FRAME */
#define INIT_INTERP_STATE(frame, _clause_args)                                \
	do {                                                                      \
		ip = _clause_args ? ((FrameClauseArgs *) _clause_args)->start_with_ip \
		                  : (frame)->imethod->code;                           \
		locals = (unsigned char *) (frame)->stack;                            \
	} while (0)

/*
 * If this bit is set, it means the call has thrown the exception, and we
 * reached this point because the EH code in mono_handle_exception ()
 * unwound all the JITted frames below us. mono_interp_set_resume_state ()
 * has set the fields in context to indicate where we have to resume execution.
 */
#define CHECK_RESUME_STATE(context)                   \
	do {                                              \
		if (G_UNLIKELY ((context)->has_resume_state)) \
			return &InterpState::exec_resume;         \
	} while (0)

} // namespace mono::interp



#endif

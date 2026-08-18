#ifndef __MONO_INTERP_INTERNALS_H__
#define __MONO_INTERP_INTERNALS_H__

#include <setjmp.h>
#include <glib.h>
#include <mono/metadata/loader.h>
#include <mono/metadata/object.h>
#include <mono/metadata/domain-internals.h>
#include <mono/metadata/class-internals.h>
#include <mono/metadata/debug-internals.h>
#include <mono/metadata/handle.h>
#include "interp.h"

#include <cstdint>
#include <string>
#include <vector>

#define INLINED_METHOD_FLAG 0xffff
#define TRACING_FLAG 0x1
#define PROFILING_FLAG 0x2

#define MINT_VT_ALIGNMENT 8
#define MINT_STACK_SLOT_SIZE (sizeof (stackval))

#define INTERP_STACK_SIZE (1024 * 1024)
/*
 * Held back from frames so that overflowing the stack above has somewhere to be
 * reported from: raising the exception runs handlers in interpreted frames, and
 * those want stack of their own.
 */
#define INTERP_STACK_RESERVE (16 * 1024)

enum {
	VAL_I32 = 0,
	VAL_DOUBLE = 1,
	VAL_I64 = 2,
	VAL_VALUET = 3,
	VAL_POINTER = 4,
	VAL_NATI = 0 + VAL_POINTER,
	VAL_MP = 1 + VAL_POINTER,
	VAL_TP = 2 + VAL_POINTER,
	VAL_OBJ = 3 + VAL_POINTER
};

#if SIZEOF_VOID_P == 4
typedef guint32 mono_u;
typedef gint32 mono_i;
#elif SIZEOF_VOID_P == 8
typedef guint64 mono_u;
typedef gint64 mono_i;
#endif

/// How a value is held in memory: the width and the signedness a local, a field
/// or an argument is loaded and stored at.
enum class MintType : std::uint8_t {
	I1 = 0,
	U1 = 1,
	I2 = 2,
	U2 = 3,
	I4 = 4,
	I8 = 5,
	R4 = 6,
	R8 = 7,
	O = 8,
	VT = 9,

	/// A native int, and so an alias for whichever of I4 and I8 that is.
#if SIZEOF_VOID_P == 8
	I = I8,
#else
	I = I4,
#endif
};

#ifdef TARGET_WASM
#define INTERP_NO_STACK_SCAN 1
#endif

/*
 * Value types are represented on the eval stack as pointers to the
 * actual storage. A value type cannot be larger than 16 MB.
 */
struct stackval {
	union {
		gint32 i;
		gint64 l;
		struct {
			gint32 lo;
			gint32 hi;
		} pair;
		float f_r4;
		double f;
#ifdef INTERP_NO_STACK_SCAN
		/* Ensure objref is always flushed to interp stack */
		MonoObject *volatile o;
#else
		MonoObject *o;
#endif
		/* native size integer and pointer types */
		gpointer p;
		mono_u nati;
		gpointer vt;
	} data;
};

struct InterpFrame;

typedef void (*MonoFuncV) (void);
typedef void (*MonoPIFunc) (void *callme, void *margs);

enum InterpMethodCodeType {
	IMETHOD_CODE_INTERP,
	IMETHOD_CODE_COMPILED,
	IMETHOD_CODE_UNKNOWN
};

/*
 * Structure representing a method transformed for the interpreter 
 * This is domain specific
 */
struct InterpMethod {
	MonoMethod *method;

	// Sort pointers ahead of integers to minimize padding for alignment.

	unsigned short *code;
	MonoPIFunc func;
	MonoExceptionClause *clauses; // num_clauses
	void **data_items;
	guint32 *local_offsets;
	guint32 *arg_offsets;
	guint32 *clause_data_offsets;
	gpointer jit_call_info;
	gpointer jit_entry;
	/*
	 * The address this method is published at outside the interpreter - the
	 * backend's stub - once something has asked for it. Held so that a calli
	 * site can tell whether the pointer on its stack stands for this method
	 * without going back to the jit-info table on every call.
	 */
	gpointer native_entry;
	gpointer llvmonly_unbox_entry;
	MonoType *rtype;
	MonoType **param_types;
	MonoJitInfo *jinfo;
	MonoDomain *domain;
	/*
	 * Maps an offset into this method's bytecode back to the IL offset it came
	 * from, so a stack trace reports an IL offset for an interpreted frame the
	 * way it does for a compiled one - which answers from llvm_seq_points.
	 *
	 * Every method carries one of these, so the encoding matters: pairs of
	 * deltas as varints, ascending by bytecode offset, which is about a quarter
	 * of what the entries cost as two words each. Read it with
	 * interp_il_offset_from_native_offset ().
	 */
	guint8 *line_numbers;
	guint32 line_numbers_size;
	/*
	 * Weak handle on the object whose death frees this method's code, or NULL when
	 * nothing manages it. A frame executing the method resolves it and holds the
	 * result for as long as it runs.
	 */
	MonoGCHandle code_owner;

	// This doesn't include the size of stack locals
	guint32 total_locals_size;
	// The size of locals that map to the execution stack
	guint32 stack_size;
	guint32 alloca_size;
	int num_clauses; // clauses
	int transformed; // boolean
	unsigned int param_count;
	unsigned int hasthis; // boolean
	MonoProfilerCallInstrumentationFlags prof_flags;
	InterpMethodCodeType code_type;
	// How many calls are left before we promote this method to tier1?
	volatile gint32 tier_counter;
	unsigned int init_locals : 1;
	unsigned int vararg : 1;
	unsigned int needs_thread_attach : 1;
#ifdef ENABLE_INTERP_TRACE
	/* Whether MONO_INTERP_TRACE names this method. */
	unsigned int tracing : 1;
#endif
};

/* Used for localloc memory allocation */
struct FrameDataFragment {
	guint8 *pos, *end;
	FrameDataFragment *next;
#if SIZEOF_VOID_P == 4
	/* Align data field to MINT_VT_ALIGNMENT */
	gint32 pad;
#endif
	double data[MONO_ZERO_LEN_ARRAY];
};

struct FrameDataInfo {
	InterpFrame *frame;
	/*
	 * frag and pos hold the current allocation position when the stored frame
	 * starts allocating memory. This is used for restoring the localloc stack
	 * when frame returns.
	 */
	FrameDataFragment *frag;
	guint8 *pos;
};

struct FrameDataAllocator {
	FrameDataFragment *first, *current;
	FrameDataInfo *infos;
	int infos_len, infos_capacity;
	/* For GC sync */
	int inited;
};

/* Arguments that are passed when invoking only a finally/filter clause from the frame */
struct FrameClauseArgs {
	/* Where we start the frame execution from */
	const guint16 *start_with_ip;
	/*
	 * End ip of the exit_clause. We need it so we know whether the resume
	 * state is for this frame (which is called from EH) or for the original
	 * frame further down the stack.
	 */
	const guint16 *end_at_ip;
	/* When exiting this clause we also exit the frame */
	int exit_clause;
	/* Exception that we are filtering */
	MonoException *filter_exception;
	/* Frame that is executing this clause */
	InterpFrame *exec_frame;
};

/* What a frame keeps of the interpreter loop while a call it made runs. */
struct InterpSavedState {
	const unsigned short *ip;
};

struct InterpFrame {
	InterpFrame *parent;   /* parent */
	InterpMethod *imethod; /* parent */
	stackval *retval;      /* parent */
	stackval *stack;
	InterpFrame *next_free;
	/*
	 * What keeps the code this frame is executing from being freed underneath it -
	 * imethod->code_owner resolved, or NULL when nothing manages the code. A root
	 * frame is a local of the entry point that built it and is rooted by the
	 * conservative scan of the native stack; the frames a call makes live in the
	 * thread's frame stack, which interp_mark_stack () scans for this field.
	 */
	MonoObject *code_owner;
	/*
	 * When this frame was entered, counted per thread. A larger value is the more
	 * recent frame, which is the only ordering the runtime asks for and the only
	 * one that survives frames living somewhere other than the native stack.
	 */
	gsize ordinal;
	/* State saved before calls */
	/* This is valid if state.ip != NULL */
	InterpSavedState state;
};

#define frame_locals(frame) (reinterpret_cast<guchar *> ((frame)->stack))

/*
 * How deep the interpreter can nest calls. A frame that runs out of value stack is
 * refused before this bound is reached, so it only decides the answer for methods
 * whose locals are small enough to outlast INTERP_STACK_SIZE.
 */
#define INTERP_MAX_FRAME_DEPTH (64 * 1024)
#define INTERP_FRAME_STACK_SIZE (INTERP_MAX_FRAME_DEPTH * sizeof (InterpFrame))

/*
 * What one interpreter invocation has to give back, held where the invocation is
 * not: a frame the EH resumes past never runs its own epilogue, so nothing on that
 * frame can be trusted to restore anything - see interp_release_abandoned_handles ().
 */
struct InterpHandleMark {
	HandleStackMark mark;
	/* The native frame that took the mark, for ordering against a resume's sp. */
	gpointer frame;
	/* context->frame_stack_pointer on entry. */
	guchar *frame_watermark;
	/* Ordinal of the first frame this invocation made. */
	gsize first_ordinal;
};

struct ThreadContext {
	/* Lets interpreter know it has to resume execution after EH */
	gboolean has_resume_state;
	/* Frame to resume execution at */
	InterpFrame *handler_frame;
	/* IP to resume execution at */
	const guint16 *handler_ip;
	/* Clause that we are resuming to */
	MonoJitExceptionInfo *handler_ei;
	/* Exception that is being thrown. Set with rest of resume state */
	MonoGCHandle exc_gchandle;
	/* This is a contiguous space allocated for interp execution stack */
	guchar *stack_start;
	/*
	 * This stack pointer is the highest stack memory that can be used by the current frame. This does not
	 * change throughout the execution of a frame and it is essentially the upper limit of the execution
	 * stack pointer. It is needed when re-entering interp, to know from which address we can start using
	 * stack, and also needed for the GC to be able to scan the stack.
	 */
	guchar *stack_pointer;
	/* Used for allocation of localloc regions */
	FrameDataAllocator data_stack;
	/*
	 * Storage for the frames a call makes. A frame outlives the code that made it -
	 * the caller resumes from its own frame long afterwards - so it cannot live on
	 * the native stack of whatever ran the call. Frames are pushed and popped in
	 * step with the calls they belong to, which makes the region below
	 * frame_stack_pointer exactly the live chain.
	 *
	 * interp_mark_stack () scans it: InterpFrame::code_owner is a managed reference
	 * and this is the only thing holding it.
	 */
	guchar *frame_stack_start;
	guchar *frame_stack_pointer;
	/* Stamped onto each frame as it is entered. Never reset, so it orders frames
	 * across nested invocations as well as within one. */
	gsize next_frame_ordinal;
	/* Points to the currently executing frame while the thread is stopped inside the
	 * interpreter and something is about to look at its stack - a self-suspend at a
	 * safepoint, or an interruption checkpoint. (If a thread stops somewhere else in
	 * the runtime this is NULL - the LMF will point to the InterpFrame before the
	 * thread exited the interpreter)
	 */
	InterpFrame *safepoint_frame;
	/* The frame that the interpreter loop runs. The loop writes this field when it enters
	 * or leaves a frame. A thread that stops inside the loop has no other record of its
	 * interpreted frames. The LMF chain only shows where the interpreter last called out,
	 * and that point is below all of them.
	 *
	 * Volatile for the sampling signal, which runs its handler on this same thread and is
	 * therefore a reader the compiler cannot see. The loop can run a long time between
	 * two writes of this field without making a call, and a write held in a register over
	 * that window is a walk that reports the wrong frame. A walk from another thread reads
	 * this only while this one is suspended, which orders it already.
	 */
	InterpFrame *volatile current_frame;
	/* One entry per interp_exec_method () invocation on this thread, outermost first. */
	InterpHandleMark *handle_marks;
	int handle_mark_count;
	int handle_mark_capacity;
#ifdef ENABLE_INTERP_TRACE
	/* How far the execution trace is indented on this thread. */
	int trace_depth;
#endif
};

struct MonoInterpStats {
	gint32 line_numbers_size;
	gint64 transform_time;
	gint64 methods_transformed;
	gint64 cprop_time;
	gint32 stloc_nps;
	gint32 movlocs;
	gint32 copy_propagations;
	gint32 constant_folds;
	gint32 ldlocas_removed;
	gint32 killed_instructions;
	gint32 emitted_instructions;
	gint32 added_pop_count;
	gint32 inlined_methods;
	gint32 inline_failures;
};

extern MonoInterpStats mono_interp_stats;

extern int mono_interp_traceopt;
extern int mono_interp_opt;
/* Class names whose methods run as compiled code rather than interpreted, from
 * the "jit=" option. For testing. */
extern std::vector<std::string> mono_interp_jit_classes;

void mono_interp_transform_method (InterpMethod *imethod, ThreadContext *context, MonoError *error);

void mono_interp_transform_init (void);

InterpMethod *mono_interp_get_imethod (MonoDomain *domain, MonoMethod *method, MonoError *error);

void mono_interp_print_code (InterpMethod *imethod);

/* Prints one instruction of a transformed method, at ip, whose code starts at
 * start. The offset it prints is relative to start. */
void mono_interp_dis_mintop (const guint16 *ip, const guint16 *start);

gboolean mono_interp_jit_call_marshallable (MonoMethod *method, MonoMethodSignature *sig);

gboolean mono_interp_jit_call_supported (MonoMethod *method, MonoMethodSignature *sig);

void mono_interp_error_cleanup (MonoError *error);

/* Whether the debugger wants a single step trampoline at every step location. */
extern gboolean mono_interp_ss_enabled;

MONO_NEVER_INLINE MonoException *mono_interp_leave (InterpFrame *parent_frame);

/*
 * The native frame running the innermost interpreter invocation, or NULL when the
 * thread is in none. This is what orders an interpreter invocation against anything
 * else on the stack, since the frames it runs are not on the stack themselves.
 */
gpointer mono_interp_invocation_anchor (ThreadContext *context);

/* The interpreter state of the calling thread, made on the first ask. */
ThreadContext *mono_interp_get_context (void);

/*
 * Record what one interpreter invocation has to give back, and return the depth to
 * truncate to when it leaves. mark must be a local of the native frame that runs the
 * invocation, and frame the first frame the invocation runs.
 */
int mono_interp_push_handle_mark (ThreadContext *context, HandleStackMark *mark,
                                  InterpFrame *frame);

/*
 * Runs frame until it returns or an exception unwinds past it. A non-NULL clause_args
 * enters an already running frame at a handler instead, which is how the EH re-enters
 * the interpreter.
 *
 * frame is only valid until the next call this makes.
 */
void mono_interp_exec_method (InterpFrame *frame, ThreadContext *context,
                              FrameClauseArgs *clause_args);

/*
 * One call from compiled code into the interpreter, taken apart. args holds a
 * pointer to each argument, or the value itself where the parameter is byref, and
 * spills into many_args past the sixteen it has room for. The low bit of rmethod
 * asks for this_arg to be unboxed first.
 */
struct InterpEntryData {
	InterpMethod *rmethod;
	gpointer this_arg;
	gpointer res;
	gpointer args[16];
	gpointer *many_args;
};

/* Runs the method data names, writing its return value to data->res. */
void mono_interp_entry (InterpEntryData *data);

/* mono_interp_entry () with the arguments spread out, for a caller with no room to
 * build the record. */
void mono_interp_entry_general (gpointer this_arg, gpointer res, gpointer *args, gpointer rmethod);
void mono_interp_entry_from_args (gpointer imethod, gpointer this_arg, gpointer res,
                                  gpointer *args);

/*
 * Runs method through its runtime-invoke wrapper and returns what it returned, boxed.
 * exc may be NULL, and a method that throws with a non-NULL exc returns NULL and
 * writes the exception there.
 */
MonoObject *mono_interp_runtime_invoke (MonoMethod *method, void *obj, void **params,
                                        MonoObject **exc, MonoError *error);

/*
 * Runs the method a native-to-interp trampoline was entered for, taking its arguments
 * out of the CallContext the trampoline spilled and writing the return value back into
 * it.
 */
void mono_interp_entry_from_ccontext (gpointer ccontext, gpointer rmethod);

/*
 * Runs one clause of a frame that is already executing, which is how the EH re-enters
 * the interpreter. Both name the clause by the bytecode range it covers.
 *
 * mono_interp_run_finally () returns whether the handler threw, and
 * mono_interp_run_filter () what the filter decided.
 */
gboolean mono_interp_run_finally (StackFrameInfo *frame, int clause_index, gpointer handler_ip,
                                  gpointer handler_ip_end);
gboolean mono_interp_run_filter (StackFrameInfo *frame, MonoException *ex, int clause_index,
                                 gpointer handler_ip, gpointer handler_ip_end);

static inline MintType
mint_type (MonoType *type_)
{
	MonoType *type = mini_native_type_replace_type (type_);
	if (type->byref)
		return MintType::I;
enum_type:
	switch (type->type) {
	case MONO_TYPE_I1:
		return MintType::I1;
	case MONO_TYPE_U1:
	case MONO_TYPE_BOOLEAN:
		return MintType::U1;
	case MONO_TYPE_I2:
		return MintType::I2;
	case MONO_TYPE_U2:
	case MONO_TYPE_CHAR:
		return MintType::U2;
	case MONO_TYPE_I4:
	case MONO_TYPE_U4:
		return MintType::I4;
	case MONO_TYPE_I:
	case MONO_TYPE_U:
	case MONO_TYPE_PTR:
	case MONO_TYPE_FNPTR:
		return MintType::I;
	case MONO_TYPE_R4:
		return MintType::R4;
	case MONO_TYPE_I8:
	case MONO_TYPE_U8:
		return MintType::I8;
	case MONO_TYPE_R8:
		return MintType::R8;
	case MONO_TYPE_STRING:
	case MONO_TYPE_SZARRAY:
	case MONO_TYPE_CLASS:
	case MONO_TYPE_OBJECT:
	case MONO_TYPE_ARRAY:
		return MintType::O;
	case MONO_TYPE_VALUETYPE:
		if (m_class_is_enumtype (type->data.klass)) {
			type = mono_class_enum_basetype_internal (type->data.klass);
			goto enum_type;
		} else
			return MintType::VT;
	case MONO_TYPE_TYPEDBYREF:
		return MintType::VT;
	case MONO_TYPE_GENERICINST:
		type = m_class_get_byval_arg (type->data.generic_class->container_class);
		goto enum_type;
	default:
		g_warning ("got type 0x%02x", type->type);
		g_assert_not_reached ();
	}
}

#endif /* __MONO_INTERP_INTERNALS_H__ */

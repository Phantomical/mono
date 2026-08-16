/**
 * \file
 *
 * interp.c: Interpreter for CIL byte codes
 *
 * Authors:
 *   Paolo Molaro (lupus@ximian.com)
 *   Miguel de Icaza (miguel@ximian.com)
 *   Dietmar Maurer (dietmar@ximian.com)
 *
 * (C) 2001, 2002 Ximian, Inc.
 */
#ifndef __USE_ISOC99
#define __USE_ISOC99
#endif
#include "config.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <glib.h>
#include <math.h>
#include <locale.h>

#include <mono/utils/gc_wrapper.h>
#include <mono/utils/mono-math.h>
#include <mono/utils/mono-counters.h>
#include <mono/utils/mono-logger-internals.h>
#include <mono/utils/mono-tls-inline.h>
#include <mono/utils/mono-threads.h>
#include <mono/utils/mono-membar.h>

#ifdef HAVE_ALLOCA_H
#   include <alloca.h>
#else
#   ifdef __CYGWIN__
#      define alloca __builtin_alloca
#   endif
#endif

/* trim excessive headers */
#include <mono/metadata/image.h>
#include <mono/metadata/assembly-internals.h>
#include <mono/metadata/cil-coff.h>
#include <mono/metadata/mono-endian.h>
#include <mono/metadata/tabledefs.h>
#include <mono/metadata/tokentype.h>
#include <mono/metadata/loader.h>
#include <mono/metadata/threads.h>
#include <mono/metadata/threadpool.h>
#include <mono/metadata/profiler-private.h>
#include <mono/metadata/appdomain.h>
#include <mono/metadata/reflection.h>
#include <mono/metadata/exception.h>
#include <mono/metadata/verify.h>
#include <mono/metadata/opcodes.h>
#include <mono/metadata/debug-helpers.h>
#include <mono/metadata/mono-config.h>
#include <mono/metadata/marshal.h>
#include <mono/metadata/environment.h>
#include <mono/metadata/mono-debug.h>
#include <mono/metadata/gc-internals.h>
#include <mono/utils/atomic.h>

#include "interp.h"
#include "interp-internals.h"
#include "mintops.h"
#include "interp-intrins.h"

#include <mono/mini/mini.h>
#include <mono/mini/mini-runtime.h>
#include <mono/mini/aot-runtime.h>
#include <mono/mini/llvm-runtime.h>
#include <mono/mini/llvmonly-runtime.h>
#include <mono/mini/jit-icalls.h>
#include <mono/mini/debugger-agent.h>
#include <mono/mini/ee.h>
#include <mono/mini/trace.h>
#include "../../llvm/runtime.h"

#include <mono/metadata/icall-decl.h>

/*
 * This code synchronizes with interp_mark_stack () using compiler memory barriers.
 */

static FrameDataFragment*
frame_data_frag_new (int size)
{
	FrameDataFragment *frag = (FrameDataFragment*)g_malloc (size);

	frag->pos = (guint8*)&frag->data;
	frag->end = (guint8*)frag + size;
	frag->next = NULL;
	return frag;
}

static void
frame_data_frag_free (FrameDataFragment *frag)
{
	while (frag) {
		FrameDataFragment *next = frag->next;
		g_free (frag);
		frag = next;
	}
}

static void
frame_data_allocator_init (FrameDataAllocator *stack, int size)
{
	FrameDataFragment *frag;

	frag = frame_data_frag_new (size);
	stack->first = stack->current = frag;
	stack->infos_capacity = 4;
	stack->infos = (FrameDataInfo*)g_malloc (stack->infos_capacity * sizeof (FrameDataInfo));
}

static void
frame_data_allocator_free (FrameDataAllocator *stack)
{
	/* Assert to catch leaks */
	g_assert_checked (stack->current == stack->first && stack->current->pos == (guint8*)&stack->current->data);
	frame_data_frag_free (stack->first);
}

/*
 * frame_root_code_owner:
 *
 *   Have FRAME hold whatever keeps the code it is about to execute alive.
 *
 * Almost no method has an owner, so this is a load and a predictable branch; the
 * resolve is paid only by the ones that do.
 */
static MONO_ALWAYS_INLINE void
frame_root_code_owner (InterpFrame *frame)
{
	MonoGCHandle owner = frame->imethod->code_owner;

	frame->code_owner = G_UNLIKELY (owner != NULL) ? mono_method_get_code_owner (owner) : NULL;
}

/*
 * Record when this frame was entered. Ordering interpreter frames is what the
 * unwinder, a stack walk and the resume path all ask for, and an address cannot
 * answer it: a frame lives on the native stack or in the thread's frame stack
 * depending on which of them made it, and the two grow in opposite directions.
 */
static void
frame_stamp_ordinal (ThreadContext *context, InterpFrame *frame)
{
	frame->ordinal = ++context->next_frame_ordinal;
}

#define STACK_ADD_BYTES(sp,bytes) ((stackval*)((char*)(sp) + ALIGN_TO(bytes, MINT_STACK_SLOT_SIZE)))

/*
 * List of classes whose methods will be executed by transitioning to JITted code.
 * Used for testing.
 */
GSList *mono_interp_jit_classes;
/* Optimizations enabled with interpreter */
int mono_interp_opt = INTERP_OPT_DEFAULT;
/* If TRUE, interpreted code will be interrupted at function entry/backward branches */
gboolean mono_interp_ss_enabled;

static gboolean interp_init_done = FALSE;

static InterpMethod* lookup_method_pointer (MonoDomain *domain, gpointer addr);

static gpointer interp_create_method_pointer (MonoMethod *method, gboolean compile, MonoError *error);

static InterpMethod* imethod_for_entry (MonoDomain *domain, gpointer addr, MonoError *error);

typedef void (*ICallMethod) (InterpFrame *frame);

static MonoNativeTlsKey thread_context_id;

#define DEBUG_INTERP 0
#define COUNT_OPS 0

#if DEBUG_INTERP
int mono_interp_traceopt = 2;
/* If true, then we output the opcodes as we interpret them */
static int global_tracing = 2;

static int debug_indent_level = 0;

static int break_on_method = 0;
static int nested_trace = 0;
static GList *db_methods = NULL;
static char* dump_args (InterpFrame *inv);

static void
output_indent (void)
{
	int h;

	for (h = 0; h < debug_indent_level; h++)
		g_print ("  ");
}

static void
db_match_method (gpointer data, gpointer user_data)
{
	MonoMethod *m = (MonoMethod*)user_data;
	MonoMethodDesc *desc = (MonoMethodDesc*)data;

	if (mono_method_desc_full_match (desc, m))
		break_on_method = 1;
}

static void
debug_enter (InterpFrame *frame, int *tracing)
{
	if (db_methods) {
		g_list_foreach (db_methods, db_match_method, (gpointer)frame->imethod->method);
		if (break_on_method)
			*tracing = nested_trace ? (global_tracing = 2, 3) : 2;
		break_on_method = 0;
	}
	if (*tracing) {
		MonoMethod *method = frame->imethod->method;
		char *mn, *args = dump_args (frame);
		debug_indent_level++;
		output_indent ();
		mn = mono_method_full_name (method, FALSE);
		g_print ("(%p) Entering %s (", mono_thread_internal_current (), mn);
		g_free (mn);
		g_print  ("%s)\n", args);
		g_free (args);
	}
}

#define DEBUG_LEAVE()	\
	if (tracing) {	\
		char *mn, *args;	\
		args = dump_retval (frame);	\
		output_indent ();	\
		mn = mono_method_full_name (frame->imethod->method, FALSE); \
		g_print  ("(%p) Leaving %s", mono_thread_internal_current (),  mn);	\
		g_free (mn); \
		g_print  (" => %s\n", args);	\
		g_free (args);	\
		debug_indent_level--;	\
		if (tracing == 3) global_tracing = 0; \
	}

#else

int mono_interp_traceopt = 0;
#define DEBUG_LEAVE()

#endif

static void
set_context (ThreadContext *context)
{
	mono_native_tls_set_value (thread_context_id, context);

	if (!context)
		return;

	MonoJitTlsData *jit_tls = mono_tls_get_jit_tls ();
	g_assertf (jit_tls, "ThreadContext needs initialized JIT TLS");

	/* jit_tls assumes ownership of 'context' */
	jit_tls->interp_context = context;
}

ThreadContext *
mono_interp_get_context (void)
{
	ThreadContext *context = (ThreadContext *) mono_native_tls_get_value (thread_context_id);
	if (context == NULL) {
		context = g_new0 (ThreadContext, 1);
		context->stack_start = (guchar*)mono_valloc (0, INTERP_STACK_SIZE, MONO_MMAP_READ | MONO_MMAP_WRITE, MONO_MEM_ACCOUNT_INTERP_STACK);
		context->stack_pointer = context->stack_start;

		context->frame_stack_start = (guchar*)mono_valloc (0, INTERP_FRAME_STACK_SIZE, MONO_MMAP_READ | MONO_MMAP_WRITE, MONO_MEM_ACCOUNT_INTERP_STACK);
		context->frame_stack_pointer = context->frame_stack_start;

		frame_data_allocator_init (&context->data_stack, 8192);
		/* Make sure all data is initialized before publishing the context */
		mono_compiler_barrier ();
		set_context (context);
	}
	return context;
}

int
mono_interp_push_handle_mark (ThreadContext *context, HandleStackMark *mark, InterpFrame *frame)
{
	int depth = context->handle_mark_count;

	if (G_UNLIKELY (depth == context->handle_mark_capacity)) {
		context->handle_mark_capacity = context->handle_mark_capacity ? context->handle_mark_capacity * 2 : 32;
		context->handle_marks = g_renew (InterpHandleMark, context->handle_marks, context->handle_mark_capacity);
	}

	context->handle_marks [depth].mark = *mark;
	/* Compared against the stack pointer a resume restores, so it has to be in the frame. */
	context->handle_marks [depth].frame = (gpointer)mark;
	context->handle_marks [depth].frame_watermark = context->frame_stack_pointer;
	context->handle_marks [depth].first_ordinal = frame->ordinal;
	context->handle_mark_count = depth + 1;

	return depth;
}

static void
interp_free_context (gpointer ctx)
{
	ThreadContext *context = (ThreadContext*)ctx;

	ThreadContext *current_context = (ThreadContext *) mono_native_tls_get_value (thread_context_id);
	/* at thread exit, we can be called from the JIT TLS key destructor with current_context == NULL */
	if (current_context != NULL) {
		/* check that the context we're freeing is the current one before overwriting TLS */
		g_assert (context == current_context);
		set_context (NULL);
	}

	context->safepoint_frame = NULL;

	mono_vfree (context->stack_start, INTERP_STACK_SIZE, MONO_MEM_ACCOUNT_INTERP_STACK);
	mono_vfree (context->frame_stack_start, INTERP_FRAME_STACK_SIZE, MONO_MEM_ACCOUNT_INTERP_STACK);
	/* Prevent interp_mark_stack from trying to scan the data_stack, before freeing it */
	context->stack_start = NULL;
	context->frame_stack_start = NULL;
	mono_compiler_barrier ();
	frame_data_allocator_free (&context->data_stack);
	g_free (context->handle_marks);
	g_free (context);
}

/*
 * Record the frame that the interpreter loop runs, so that a walk of this thread's stack
 * can find its interpreted frames.
 *
 * A call publishes the callee only after the callee is ready to run. A return publishes
 * the caller before anything retires the callee. Both orders name a frame that is live.
 * A walk that arrives in one of those windows reports one frame less than the truth, and
 * that is the safe direction to be wrong in.
 */
static void
context_set_current_frame (ThreadContext *context, InterpFrame *frame)
{
	context->current_frame = frame;
}

void
mono_interp_error_cleanup (MonoError* error)
{
	mono_error_cleanup (error); /* FIXME: don't swallow the error */
	error_init_reuse (error); // one instruction, so this function is good inline candidate
}

static InterpMethod*
lookup_imethod (MonoDomain *domain, MonoMethod *method)
{
	InterpMethod *imethod;
	MonoJitDomainInfo *info;

	info = domain_jit_info (domain);
	mono_domain_jit_code_hash_lock (domain);
	imethod = (InterpMethod*)mono_internal_hash_table_lookup (&info->interp_code_hash, method);
	mono_domain_jit_code_hash_unlock (domain);
	return imethod;
}

static gpointer
interp_get_remoting_invoke (MonoMethod *method, gpointer addr, MonoError *error)
{
#ifndef DISABLE_REMOTING
	InterpMethod *imethod;

	if (addr) {
		imethod = lookup_method_pointer (mono_domain_get (), addr);
	} else {
		g_assert (method);
		imethod = mono_interp_get_imethod (mono_domain_get (), method, error);
		return_val_if_nok (error, NULL);
	}
	g_assert (imethod);
	g_assert (mono_use_interpreter);

	MonoMethod *remoting_invoke_method = mono_marshal_get_remoting_invoke (imethod->method, error);
	return_val_if_nok (error, NULL);
	return mono_interp_get_imethod (mono_domain_get (), remoting_invoke_method, error);
#else
	g_assert_not_reached ();
	return NULL;
#endif
}

InterpMethod*
mono_interp_get_imethod (MonoDomain *domain, MonoMethod *method, MonoError *error)
{
	InterpMethod *imethod;
	MonoJitDomainInfo *info;
	MonoMethodSignature *sig;
	int i;

	error_init (error);

	info = domain_jit_info (domain);
	mono_domain_jit_code_hash_lock (domain);
	imethod = (InterpMethod*)mono_internal_hash_table_lookup (&info->interp_code_hash, method);
	mono_domain_jit_code_hash_unlock (domain);
	if (imethod)
		return imethod;

	sig = mono_method_signature_internal (method);

	imethod = (InterpMethod*)m_method_alloc0 (domain, method, sizeof (InterpMethod));
	imethod->method = method;
	imethod->domain = domain;
	imethod->param_count = sig->param_count;
	imethod->hasthis = sig->hasthis;
	imethod->vararg = sig->call_convention == MONO_CALL_VARARG;
	imethod->code_type = IMETHOD_CODE_UNKNOWN;
	if (imethod->method->string_ctor)
		imethod->rtype = m_class_get_byval_arg (mono_defaults.string_class);
	else
		imethod->rtype = mini_get_underlying_type (sig->ret);
	imethod->code_owner = mono_method_get_code_owner_handle (domain, method);
	imethod->param_types = (MonoType**)m_method_alloc0 (domain, method, sizeof (MonoType*) * sig->param_count);
	for (i = 0; i < sig->param_count; ++i)
		imethod->param_types [i] = mini_get_underlying_type (sig->params [i]);

	mono_domain_jit_code_hash_lock (domain);
	if (!mono_internal_hash_table_lookup (&info->interp_code_hash, method))
		mono_internal_hash_table_insert (&info->interp_code_hash, method, imethod);
	mono_domain_jit_code_hash_unlock (domain);

	imethod->prof_flags = mono_profiler_get_call_instrumentation_flags (imethod->method);

	return imethod;
}

#if defined (MONO_CROSS_COMPILE) || defined (HOST_WASM)
#define INTERP_PUSH_LMF_WITH_CTX_BODY(ext, exit_label) \
	(ext).kind = MONO_LMFEXT_INTERP_EXIT;

#elif defined(MONO_ARCH_HAS_NO_PROPER_MONOCTX)
/* some platforms, e.g. appleTV, don't provide us a precise MonoContext
 * (registers are not accurate), thus resuming to the label does not work. */
#define INTERP_PUSH_LMF_WITH_CTX_BODY(ext, exit_label) \
	(ext).kind = MONO_LMFEXT_INTERP_EXIT;
#elif defined (_MSC_VER)
#define INTERP_PUSH_LMF_WITH_CTX_BODY(ext, exit_label) \
	(ext).kind = MONO_LMFEXT_INTERP_EXIT_WITH_CTX; \
	(ext).interp_exit_label_set = FALSE; \
	MONO_CONTEXT_GET_CURRENT ((ext).ctx); \
	if ((ext).interp_exit_label_set == FALSE) \
		mono_arch_do_ip_adjustment (&(ext).ctx); \
	if ((ext).interp_exit_label_set == TRUE) \
		goto exit_label; \
	(ext).interp_exit_label_set = TRUE;
#elif defined(MONO_ARCH_HAS_MONO_CONTEXT)
#define INTERP_PUSH_LMF_WITH_CTX_BODY(ext, exit_label) \
	(ext).kind = MONO_LMFEXT_INTERP_EXIT_WITH_CTX; \
	MONO_CONTEXT_GET_CURRENT ((ext).ctx); \
	MONO_CONTEXT_SET_IP (&(ext).ctx, (&&exit_label)); \
	mono_arch_do_ip_adjustment (&(ext).ctx);
#else
#define INTERP_PUSH_LMF_WITH_CTX_BODY(ext, exit_label) g_error ("requires working mono-context");
#endif

/* INTERP_PUSH_LMF_WITH_CTX:
 *
 * same as interp_push_lmf, but retrieving and attaching MonoContext to it.
 * This is needed to resume into the interp when the exception is thrown from
 * native code (see ./mono/tests/install_eh_callback.exe).
 *
 * This must be a macro in order to retrieve the right register values for
 * MonoContext.
 */
#define INTERP_PUSH_LMF_WITH_CTX(frame, ext, exit_label) \
	memset (&(ext), 0, sizeof (MonoLMFExt)); \
	(ext).interp_exit_data = (frame); \
	INTERP_PUSH_LMF_WITH_CTX_BODY ((ext), exit_label); \
	mono_push_lmf (&(ext));

static void
interp_pop_lmf (MonoLMFExt *ext)
{
	mono_pop_lmf (&ext->lmf);
}

static InterpMethod*
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
			ret = mono_interp_get_imethod (domain, mono_marshal_get_synchronized_wrapper (m), error);
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
		slot += mono_class_interface_offset_with_variance (vtable->klass, m->klass, &non_exact_match);
	}

	MonoMethod *virtual_method = m_class_get_vtable (vtable->klass) [slot];
	if (m->is_inflated && mono_method_get_context (m)->method_inst) {
		MonoGenericContext context = { NULL, NULL };

		if (mono_class_is_ginst (virtual_method->klass))
			context.class_inst = mono_class_get_generic_class (virtual_method->klass)->context.class_inst;
		else if (mono_class_is_gtd (virtual_method->klass))
			context.class_inst = mono_class_get_generic_container (virtual_method->klass)->context.class_inst;
		context.method_inst = mono_method_get_context (m)->method_inst;

		ERROR_DECL (error);
		virtual_method = mono_class_inflate_generic_method_checked (virtual_method, &context, error);
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

typedef struct {
	InterpMethod *imethod;
	InterpMethod *target_imethod;
} InterpVTableEntry;

// Returns the size it uses on the interpreter stack
static int
stackval_from_data (MonoType *type, stackval *result, const void *data, gboolean pinvoke)
{
	type = mini_native_type_replace_type (type);
	if (type->byref) {
		result->data.p = *(gpointer*)data;
		return MINT_STACK_SLOT_SIZE;
	}
	switch (type->type) {
	case MONO_TYPE_VOID:
		return 0;
	case MONO_TYPE_I1:
		result->data.i = *(gint8*)data;
		return MINT_STACK_SLOT_SIZE;
	case MONO_TYPE_U1:
	case MONO_TYPE_BOOLEAN:
		result->data.i = *(guint8*)data;
		return MINT_STACK_SLOT_SIZE;
	case MONO_TYPE_I2:
		result->data.i = *(gint16*)data;
		return MINT_STACK_SLOT_SIZE;
	case MONO_TYPE_U2:
	case MONO_TYPE_CHAR:
		result->data.i = *(guint16*)data;
		return MINT_STACK_SLOT_SIZE;
	case MONO_TYPE_I4:
		result->data.i = *(gint32*)data;
		return MINT_STACK_SLOT_SIZE;
	case MONO_TYPE_U:
	case MONO_TYPE_I:
		result->data.nati = *(mono_i*)data;
		return MINT_STACK_SLOT_SIZE;
	case MONO_TYPE_PTR:
	case MONO_TYPE_FNPTR:
		result->data.p = *(gpointer*)data;
		return MINT_STACK_SLOT_SIZE;
	case MONO_TYPE_U4:
		result->data.i = *(guint32*)data;
		return MINT_STACK_SLOT_SIZE;
	case MONO_TYPE_R4:
		/* memmove handles unaligned case */
		memmove (&result->data.f_r4, data, sizeof (float));
		return MINT_STACK_SLOT_SIZE;
	case MONO_TYPE_I8:
	case MONO_TYPE_U8:
		memmove (&result->data.l, data, sizeof (gint64));
		return MINT_STACK_SLOT_SIZE;
	case MONO_TYPE_R8:
		memmove (&result->data.f, data, sizeof (double));
		return MINT_STACK_SLOT_SIZE;
	case MONO_TYPE_STRING:
	case MONO_TYPE_SZARRAY:
	case MONO_TYPE_CLASS:
	case MONO_TYPE_OBJECT:
	case MONO_TYPE_ARRAY:
		result->data.p = *(gpointer*)data;
		return MINT_STACK_SLOT_SIZE;
	case MONO_TYPE_TYPEDBYREF:
		/*
		 * The transform pushes a TypedReference as a value of this size, so a plain
		 * copy is what the interpreter stack expects on either side of the boundary.
		 */
		memcpy (result, data, sizeof (MonoTypedRef));
		return ALIGN_TO (sizeof (MonoTypedRef), MINT_STACK_SLOT_SIZE);
	case MONO_TYPE_VALUETYPE:
		if (m_class_is_enumtype (type->data.klass)) {
			return stackval_from_data (mono_class_enum_basetype_internal (type->data.klass), result, data, pinvoke);
		} else {
			int size;
			if (pinvoke)
				size = mono_class_native_size (type->data.klass, NULL);
			else
				size = mono_class_value_size (type->data.klass, NULL);
			memcpy (result, data, size);
			return ALIGN_TO (size, MINT_STACK_SLOT_SIZE);
		}
	case MONO_TYPE_GENERICINST: {
		if (mono_type_generic_inst_is_valuetype (type)) {
			MonoClass *klass = mono_class_from_mono_type_internal (type);
			int size;
			if (pinvoke)
				size = mono_class_native_size (klass, NULL);
			else
				size = mono_class_value_size (klass, NULL);
			memcpy (result, data, size);
			return ALIGN_TO (size, MINT_STACK_SLOT_SIZE);
		}
		return stackval_from_data (m_class_get_byval_arg (type->data.generic_class->container_class), result, data, pinvoke);
	}
	default:
		g_error ("got type 0x%02x", type->type);
	}
}

static int
stackval_to_data (MonoType *type, stackval *val, void *data, gboolean pinvoke)
{
	type = mini_native_type_replace_type (type);
	if (type->byref) {
		gpointer *p = (gpointer*)data;
		*p = val->data.p;
		return MINT_STACK_SLOT_SIZE;
	}
	/* printf ("TODAT0 %p\n", data); */
	switch (type->type) {
	case MONO_TYPE_I1:
	case MONO_TYPE_U1: {
		guint8 *p = (guint8*)data;
		*p = val->data.i;
		return MINT_STACK_SLOT_SIZE;
	}
	case MONO_TYPE_BOOLEAN: {
		guint8 *p = (guint8*)data;
		*p = (val->data.i != 0);
		return MINT_STACK_SLOT_SIZE;
	}
	case MONO_TYPE_I2:
	case MONO_TYPE_U2:
	case MONO_TYPE_CHAR: {
		guint16 *p = (guint16*)data;
		*p = val->data.i;
		return MINT_STACK_SLOT_SIZE;
	}
	case MONO_TYPE_I: {
		mono_i *p = (mono_i*)data;
		/* In theory the value used by stloc should match the local var type
	 	   but in practice it sometimes doesn't (a int32 gets dup'd and stloc'd into
		   a native int - both by csc and mcs). Not sure what to do about sign extension
		   as it is outside the spec... doing the obvious */
		*p = (mono_i)val->data.nati;
		return MINT_STACK_SLOT_SIZE;
	}
	case MONO_TYPE_U: {
		mono_u *p = (mono_u*)data;
		/* see above. */
		*p = (mono_u)val->data.nati;
		return MINT_STACK_SLOT_SIZE;
	}
	case MONO_TYPE_I4:
	case MONO_TYPE_U4: {
		gint32 *p = (gint32*)data;
		*p = val->data.i;
		return MINT_STACK_SLOT_SIZE;
	}
	case MONO_TYPE_I8:
	case MONO_TYPE_U8: {
		memmove (data, &val->data.l, sizeof (gint64));
		return MINT_STACK_SLOT_SIZE;
	}
	case MONO_TYPE_R4: {
		/* memmove handles unaligned case */
		memmove (data, &val->data.f_r4, sizeof (float));
		return MINT_STACK_SLOT_SIZE;
	}
	case MONO_TYPE_R8: {
		memmove (data, &val->data.f, sizeof (double));
		return MINT_STACK_SLOT_SIZE;
	}
	case MONO_TYPE_STRING:
	case MONO_TYPE_SZARRAY:
	case MONO_TYPE_CLASS:
	case MONO_TYPE_OBJECT:
	case MONO_TYPE_ARRAY: {
		gpointer *p = (gpointer *) data;
		mono_gc_wbarrier_generic_store_internal (p, val->data.o);
		return MINT_STACK_SLOT_SIZE;
	}
	case MONO_TYPE_PTR:
	case MONO_TYPE_FNPTR: {
		gpointer *p = (gpointer *) data;
		*p = val->data.p;
		return MINT_STACK_SLOT_SIZE;
	}
	case MONO_TYPE_TYPEDBYREF:
		memcpy (data, val, sizeof (MonoTypedRef));
		return ALIGN_TO (sizeof (MonoTypedRef), MINT_STACK_SLOT_SIZE);
	case MONO_TYPE_VALUETYPE:
		if (m_class_is_enumtype (type->data.klass)) {
			return stackval_to_data (mono_class_enum_basetype_internal (type->data.klass), val, data, pinvoke);
		} else {
			int size;
			if (pinvoke) {
				size = mono_class_native_size (type->data.klass, NULL);
				memcpy (data, val, size);
			} else {
				size = mono_class_value_size (type->data.klass, NULL);
				mono_value_copy_internal (data, val, type->data.klass);
			}
			return ALIGN_TO (size, MINT_STACK_SLOT_SIZE);
		}
	case MONO_TYPE_GENERICINST: {
		MonoClass *container_class = type->data.generic_class->container_class;

		if (m_class_is_valuetype (container_class) && !m_class_is_enumtype (container_class)) {
			MonoClass *klass = mono_class_from_mono_type_internal (type);
			int size;
			if (pinvoke) {
				size = mono_class_native_size (klass, NULL);
				memcpy (data, val, size);
			} else {
				size = mono_class_value_size (klass, NULL);
				mono_value_copy_internal (data, val, klass);
			}
			return ALIGN_TO (size, MINT_STACK_SLOT_SIZE);
		}
		return stackval_to_data (m_class_get_byval_arg (type->data.generic_class->container_class), val, data, pinvoke);
	}
	default:
		g_error ("got type %x", type->type);
	}
}

static gint32
ves_array_calculate_index (MonoArray *ao, stackval *sp, gboolean safe)
{
	MonoClass *ac = ((MonoObject *) ao)->vtable->klass;

	guint32 pos = 0;
	if (ao->bounds) {
		for (gint32 i = 0; i < m_class_get_rank (ac); i++) {
			gint32 idx = sp [i].data.i;
			gint32 lower = ao->bounds [i].lower_bound;
			guint32 len = ao->bounds [i].length;
			if (safe && (idx < lower || (guint32)(idx - lower) >= len))
				return -1;
			pos = (pos * len) + (guint32)(idx - lower);
		}
	} else {
		pos = sp [0].data.i;
		if (safe && pos >= ao->max_length)
			return -1;
	}
	return pos;
}

static MonoException*
ves_array_get (InterpFrame *frame, stackval *sp, stackval *retval, MonoMethodSignature *sig, gboolean safe)
{
	MonoObject *o = sp->data.o;
	MonoArray *ao = (MonoArray *) o;
	MonoClass *ac = o->vtable->klass;

	g_assert (m_class_get_rank (ac) >= 1);

	gint32 pos = ves_array_calculate_index (ao, sp + 1, safe);
	if (pos == -1)
		return mono_get_exception_index_out_of_range ();

	gint32 esize = mono_array_element_size (ac);
	gconstpointer ea = mono_array_addr_with_size_fast (ao, esize, pos);

	MonoType *mt = sig->ret;
	stackval_from_data (mt, retval, ea, FALSE);
	return NULL;
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
			MonoType *type = sig->params [i];
			size = mono_type_size (type, &align);
			offset += ALIGN_TO (size, MINT_STACK_SLOT_SIZE);
		}
		return offset;
	} else {
		int size, align;
		MonoType *type = sig->params [index - 1];
		size = mono_type_size (type, &align);
		return prev_offset + ALIGN_TO (size, MINT_STACK_SLOT_SIZE);
	}
}

static guint32*
initialize_arg_offsets (InterpMethod *imethod)
{
	if (imethod->arg_offsets)
		return imethod->arg_offsets;

	MonoMethodSignature *sig = mono_method_signature_internal (imethod->method);
	int arg_count = sig->hasthis + sig->param_count;
	g_assert (arg_count);
	guint32 *arg_offsets = (guint32*) g_malloc ((sig->hasthis + sig->param_count) * sizeof (int));
	int index = 0, offset_addend = 0, prev_offset = 0;

	if (sig->hasthis) {
		arg_offsets [index++] = 0;
		offset_addend = MINT_STACK_SLOT_SIZE;
	}

	for (int i = 0; i < sig->param_count; i++) {
		prev_offset = compute_arg_offset (sig, i, prev_offset);
		arg_offsets [index++] = prev_offset + offset_addend;
	}

	mono_memory_write_barrier ();
	if (mono_atomic_cas_ptr ((gpointer*)&imethod->arg_offsets, arg_offsets, NULL) != NULL)
		g_free (arg_offsets);
	return imethod->arg_offsets;
}

static guint32
get_arg_offset_fast (InterpMethod *imethod, int index)
{
	guint32 *arg_offsets = imethod->arg_offsets;
	if (arg_offsets)
		return arg_offsets [index];

	arg_offsets = initialize_arg_offsets (imethod);
	g_assert (arg_offsets);
	return arg_offsets [index];
}

static guint32
get_arg_offset (InterpMethod *imethod, MonoMethodSignature *sig, int index)
{
	/*
	 * A managed-to-native wrapper is entered with one signature and calls out
	 * with another, and at the native call the frame holds the marshalled
	 * values, laid out under the second. The cached offsets describe the first
	 * only, so they answer for the signature they were built from and nothing
	 * else. HandleRef is where the difference shows: two words as the managed
	 * type, one word once the wrapper has extracted the handle.
	 */
	if (imethod && sig == mono_method_signature_internal (imethod->method))
		return get_arg_offset_fast (imethod, index);

	g_assert (!sig->hasthis);
	return compute_arg_offset (sig, index, -1);
}

#ifdef MONO_ARCH_HAVE_INTERP_ENTRY_TRAMPOLINE
static MonoFuncV mono_native_to_interp_trampoline = NULL;
#endif

#ifndef MONO_ARCH_HAVE_INTERP_PINVOKE_TRAMP
static InterpMethodArguments* build_args_from_sig (MonoMethodSignature *sig, InterpFrame *frame)
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
		guint32 ptype = sig->params [i]->byref ? MONO_TYPE_PTR : sig->params [i]->type;
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
		margs->iargs [0] = frame->stack [0].data.p;
		int_i++;
		g_error ("FIXME if hasthis, we incorrectly access the args below");
	}

	for (int i = 0; i < sig->param_count; i++) {
		guint32 offset = get_arg_offset (frame->imethod, sig, i);
		stackval *sp_arg = STACK_ADD_BYTES (frame->stack, offset);
		MonoType *type = sig->params [i];
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
			margs->iargs [int_i] = sp_arg->data.p;
#if DEBUG_INTERP
			g_print ("build_args_from_sig: margs->iargs [%d]: %p (frame @ %d)\n", int_i, margs->iargs [int_i], i);
#endif
			int_i++;
			break;
		case MONO_TYPE_VALUETYPE:
			if (m_class_is_enumtype (type->data.klass)) {
				type = mono_class_enum_basetype_internal (type->data.klass);
				goto retry;
			}
			margs->iargs [int_i] = sp_arg;
#if DEBUG_INTERP
			g_print ("build_args_from_sig: margs->iargs [%d]: %p (vt) (frame @ %d)\n", int_i, margs->iargs [int_i], i);
#endif
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
			margs->iargs [int_i] = (gpointer) sp_arg->data.pair.lo;
			int_i++;
			margs->iargs [int_i] = (gpointer) sp_arg->data.pair.hi;
#if DEBUG_INTERP
			g_print ("build_args_from_sig: margs->iargs [%d/%d]: 0x%016" PRIx64 ", hi=0x%08x lo=0x%08x (frame @ %d)\n", int_i - 1, int_i, *((guint64 *) &margs->iargs [int_i - 1]), sp_arg->data.pair.hi, sp_arg->data.pair.lo, i);
#endif
			int_i++;
			break;
		}
#endif
		case MONO_TYPE_R4:
		case MONO_TYPE_R8:
			if (ptype == MONO_TYPE_R4)
				* (float *) &(margs->fargs [int_f]) = sp_arg->data.f_r4;
			else
				margs->fargs [int_f] = sp_arg->data.f;
#if DEBUG_INTERP
			g_print ("build_args_from_sig: margs->fargs [%d]: %p (%f) (frame @ %d)\n", int_f, margs->fargs [int_f], margs->fargs [int_f], i);
#endif
			int_f ++;
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

static void
interp_frame_arg_to_data (MonoInterpFrameHandle frame, MonoMethodSignature *sig, int index, gpointer data)
{
	InterpFrame *iframe = (InterpFrame*)frame;
	InterpMethod *imethod = iframe->imethod;

	// If index == -1, we finished executing an InterpFrame and the result is at the bottom of the stack.
	if (index == -1)
		stackval_to_data (sig->ret, iframe->stack, data, TRUE);
	else if (sig->hasthis && index == 0)
		*(gpointer*)data = iframe->stack->data.p;
	else
		stackval_to_data (sig->params [index - sig->hasthis], STACK_ADD_BYTES (iframe->stack, get_arg_offset (imethod, sig, index)), data, sig->pinvoke);
}

static void
interp_data_to_frame_arg (MonoInterpFrameHandle frame, MonoMethodSignature *sig, int index, gconstpointer data)
{
	InterpFrame *iframe = (InterpFrame*)frame;
	InterpMethod *imethod = iframe->imethod;

	// Get result from pinvoke call, put it directly on top of execution stack in the caller frame
	if (index == -1)
		stackval_from_data (sig->ret, iframe->stack, data, TRUE);
	else if (sig->hasthis && index == 0)
		iframe->stack->data.p = *(gpointer*)data;
	else
		stackval_from_data (sig->params [index - sig->hasthis], STACK_ADD_BYTES (iframe->stack, get_arg_offset (imethod, sig, index)), data, sig->pinvoke);
}

static gpointer
interp_frame_arg_to_storage (MonoInterpFrameHandle frame, MonoMethodSignature *sig, int index)
{
	InterpFrame *iframe = (InterpFrame*)frame;
	InterpMethod *imethod = iframe->imethod;

	if (index == -1)
		return iframe->stack;
	else
		return STACK_ADD_BYTES (iframe->stack, get_arg_offset (imethod, sig, index));
}

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

static void
interp_to_native_trampoline (gpointer addr, gpointer ccontext)
{
	get_interp_to_native_trampoline () (addr, ccontext);
}

/* MONO_NO_OPTIMIZATION is needed due to usage of INTERP_PUSH_LMF_WITH_CTX. */
#ifdef _MSC_VER
#pragma optimize ("", off)
#endif
static MONO_NO_OPTIMIZATION MONO_NEVER_INLINE gpointer
ves_pinvoke_method (
	InterpMethod *imethod,
	MonoMethodSignature *sig,
	MonoFuncV addr,
	ThreadContext *context,
	InterpFrame *parent_frame,
	stackval *sp,
	gboolean save_last_error,
	gpointer *cache)
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
		entry_func = (MonoPIFunc)mono_wasm_get_interp_to_native_trampoline (sig);
		mono_memory_barrier ();
		*cache = entry_func;
	}
#else
	static MonoPIFunc entry_func = NULL;
	if (!entry_func) {
		MONO_ENTER_GC_UNSAFE;
#ifdef MONO_ARCH_HAS_NO_PROPER_MONOCTX
		ERROR_DECL (error);
		entry_func = (MonoPIFunc) mono_jit_compile_method_jit_only (mini_get_interp_lmf_wrapper ("mono_interp_to_native_trampoline", (gpointer) mono_interp_to_native_trampoline), error);
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
		stackval_from_data (sig->ret, frame.stack, (char*)frame.stack->data.p, sig->pinvoke);

	g_free (margs->iargs);
	g_free (margs->fargs);
	g_free (margs);
#endif
	goto exit_pinvoke; // prevent unused label warning in some configurations
exit_pinvoke:
	return NULL;
}
#ifdef _MSC_VER
#pragma optimize ("", on)
#endif

void
mono_interp_do_pinvoke (InterpMethod *imethod, MonoMethodSignature *sig, MonoFuncV addr,
		ThreadContext *context, InterpFrame *parent_frame, stackval *sp,
		gboolean save_last_error, gpointer *cache)
{
	ves_pinvoke_method (imethod, sig, addr, context, parent_frame, sp, save_last_error, cache);
}

/*
 * interp_init_delegate:
 *
 *   Initialize del->interp_method.
 */
static void
interp_init_delegate (MonoDelegate *del, MonoError *error)
{
	MonoDomain *domain = del->object.vtable->domain;
	MonoMethod *method;

	if (del->interp_method) {
		/* A remoting invoke, already resolved by mini_init_delegate (). */
		del->method = ((InterpMethod *)del->interp_method)->method;
	} else if (del->method) {
		del->interp_method = mono_interp_get_imethod (domain, del->method, error);
		return_if_nok (error);
	} else if (del->method_ptr) {
		/*
		 * An entry point and nothing else - ldftn's product, or
		 * MethodHandle.GetFunctionPointer's. Whichever engine published it knows
		 * which method it stands for.
		 */
		del->interp_method = imethod_for_entry (domain, del->method_ptr, error);
		return_if_nok (error);
		g_assert (del->interp_method);
		del->method = ((InterpMethod *)del->interp_method)->method;
	} else {
		g_assert_not_reached ();
	}

	method = ((InterpMethod*)del->interp_method)->method;
	if (del->target &&
			method &&
			method->flags & METHOD_ATTRIBUTE_VIRTUAL &&
			method->flags & METHOD_ATTRIBUTE_ABSTRACT &&
			mono_class_is_abstract (method->klass))
		del->interp_method = get_virtual_method ((InterpMethod*)del->interp_method, del->target->vtable);

	method = ((InterpMethod*)del->interp_method)->method;
	if (method && m_class_get_parent (method->klass) == mono_defaults.multicastdelegate_class) {
		const char *name = method->name;
		if (*name == 'I' && (strcmp (name, "Invoke") == 0)) {
			/*
			 * When invoking the delegate interp_method is executed directly. If it's an
			 * invoke make sure we replace it with the appropriate delegate invoke wrapper.
			 *
			 * FIXME We should do this later, when we also know the delegate on which the
			 * target method is called.
			 */
			del->interp_method = mono_interp_get_imethod (domain, mono_marshal_get_delegate_invoke (method, NULL), error);
			mono_error_assert_ok (error);
		}
	}

	if (!((InterpMethod *) del->interp_method)->transformed && method_is_dynamic (method)) {
		/* Return any errors from method compilation */
		mono_interp_transform_method ((InterpMethod *) del->interp_method, mono_interp_get_context (), error);
		return_if_nok (error);
	}
}

/*
 * From the spec:
 * runtime specifies that the implementation of the method is automatically
 * provided by the runtime and is primarily used for the methods of delegates.
 */
#ifndef ENABLE_NETCORE
static MONO_NEVER_INLINE MonoException*
ves_imethod (InterpFrame *frame, MonoMethod *method, MonoMethodSignature *sig, stackval *sp)
{
	const char *name = method->name;
	mono_class_init_internal (method->klass);

	if (method->klass == mono_defaults.array_class) {
		if (!strcmp (name, "UnsafeMov")) {
			/* TODO: layout checks */
			stackval_from_data (sig->ret, sp, (char*) sp, FALSE);
			return NULL;
		}
		if (!strcmp (name, "UnsafeLoad"))
			return ves_array_get (frame, sp, sp, sig, FALSE);
	}
	
	g_error ("Don't know how to exec runtime method %s.%s::%s", 
			m_class_get_name_space (method->klass), m_class_get_name (method->klass),
			method->name);
}
#endif

#if DEBUG_INTERP
static void
dump_stackval (GString *str, stackval *s, MonoType *type)
{
	switch (type->type) {
	case MONO_TYPE_I1:
	case MONO_TYPE_U1:
	case MONO_TYPE_I2:
	case MONO_TYPE_U2:
	case MONO_TYPE_I4:
	case MONO_TYPE_U4:
	case MONO_TYPE_CHAR:
	case MONO_TYPE_BOOLEAN:
		g_string_append_printf (str, "[%d] ", s->data.i);
		break;
	case MONO_TYPE_STRING:
	case MONO_TYPE_SZARRAY:
	case MONO_TYPE_CLASS:
	case MONO_TYPE_OBJECT:
	case MONO_TYPE_ARRAY:
	case MONO_TYPE_PTR:
	case MONO_TYPE_FNPTR:
	case MONO_TYPE_I:
	case MONO_TYPE_U:
		g_string_append_printf (str, "[%p] ", s->data.p);
		break;
	case MONO_TYPE_VALUETYPE:
		if (m_class_is_enumtype (type->data.klass))
			g_string_append_printf (str, "[%d] ", s->data.i);
		else
			g_string_append_printf (str, "[vt:%p] ", s->data.p);
		break;
	case MONO_TYPE_R4:
		g_string_append_printf (str, "[%g] ", s->data.f_r4);
		break;
	case MONO_TYPE_R8:
		g_string_append_printf (str, "[%g] ", s->data.f);
		break;
	case MONO_TYPE_I8:
	case MONO_TYPE_U8:
	default: {
		GString *res = g_string_new ("");
		mono_type_get_desc (res, type, TRUE);
		g_string_append_printf (str, "[{%s} %" PRId64 "/0x%0" PRIx64 "] ", res->str, (gint64)s->data.l, (guint64)s->data.l);
		g_string_free (res, TRUE);
		break;
	}
	}
}

static char*
dump_retval (InterpFrame *inv)
{
	GString *str = g_string_new ("");
	MonoType *ret = mono_method_signature_internal (inv->imethod->method)->ret;

	if (ret->type != MONO_TYPE_VOID)
		dump_stackval (str, inv->stack, ret);

	return g_string_free (str, FALSE);
}

static char*
dump_args (InterpFrame *inv)
{
	GString *str = g_string_new ("");
	int i;
	MonoMethodSignature *signature = mono_method_signature_internal (inv->imethod->method);
	
	if (signature->param_count == 0 && !signature->hasthis)
		return g_string_free (str, FALSE);

	if (signature->hasthis) {
		MonoMethod *method = inv->imethod->method;
		dump_stackval (str, inv->stack, m_class_get_byval_arg (method->klass));
	}

	for (i = 0; i < signature->param_count; ++i)
		dump_stackval (str, inv->stack + (!!signature->hasthis) + i, signature->params [i]);

	return g_string_free (str, FALSE);
}
#endif

// Initialize the tiering counter, if it hasn't already been initialized.
static void
interp_arm_tier_counter (gpointer imethod_ptr, gint32 calls)
{
	InterpMethod *imethod = (InterpMethod*)imethod_ptr;

	mono_atomic_cas_i32 (&imethod->tier_counter, calls > 0 ? calls : -1, 0);
}

static void
do_icall (MonoMethodSignature *sig, int op, stackval *sp, gpointer ptr, gboolean save_last_error)
{
#ifdef ENABLE_NETCORE
	if (save_last_error)
		mono_marshal_clear_last_error ();
#endif

	switch (op) {
	case MINT_ICALL_V_V: {
		typedef void (*T)(void);
		T func = (T)ptr;
        	func ();
		break;
	}
	case MINT_ICALL_V_P: {
		typedef gpointer (*T)(void);
		T func = (T)ptr;
		sp [0].data.p = func ();
		break;
	}
	case MINT_ICALL_P_V: {
		typedef void (*T)(gpointer);
		T func = (T)ptr;
		func (sp [0].data.p);
		break;
	}
	case MINT_ICALL_P_P: {
		typedef gpointer (*T)(gpointer);
		T func = (T)ptr;
		sp [0].data.p = func (sp [0].data.p);
		break;
	}
	case MINT_ICALL_PP_V: {
		typedef void (*T)(gpointer,gpointer);
		T func = (T)ptr;
		func (sp [0].data.p, sp [1].data.p);
		break;
	}
	case MINT_ICALL_PP_P: {
		typedef gpointer (*T)(gpointer,gpointer);
		T func = (T)ptr;
		sp [0].data.p = func (sp [0].data.p, sp [1].data.p);
		break;
	}
	case MINT_ICALL_PPP_V: {
		typedef void (*T)(gpointer,gpointer,gpointer);
		T func = (T)ptr;
		func (sp [0].data.p, sp [1].data.p, sp [2].data.p);
		break;
	}
	case MINT_ICALL_PPP_P: {
		typedef gpointer (*T)(gpointer,gpointer,gpointer);
		T func = (T)ptr;
		sp [0].data.p = func (sp [0].data.p, sp [1].data.p, sp [2].data.p);
		break;
	}
	case MINT_ICALL_PPPP_V: {
		typedef void (*T)(gpointer,gpointer,gpointer,gpointer);
		T func = (T)ptr;
		func (sp [0].data.p, sp [1].data.p, sp [2].data.p, sp [3].data.p);
		break;
	}
	case MINT_ICALL_PPPP_P: {
		typedef gpointer (*T)(gpointer,gpointer,gpointer,gpointer);
		T func = (T)ptr;
		sp [0].data.p = func (sp [0].data.p, sp [1].data.p, sp [2].data.p, sp [3].data.p);
		break;
	}
	case MINT_ICALL_PPPPP_V: {
		typedef void (*T)(gpointer,gpointer,gpointer,gpointer,gpointer);
		T func = (T)ptr;
		func (sp [0].data.p, sp [1].data.p, sp [2].data.p, sp [3].data.p, sp [4].data.p);
		break;
	}
	case MINT_ICALL_PPPPP_P: {
		typedef gpointer (*T)(gpointer,gpointer,gpointer,gpointer,gpointer);
		T func = (T)ptr;
		sp [0].data.p = func (sp [0].data.p, sp [1].data.p, sp [2].data.p, sp [3].data.p, sp [4].data.p);
		break;
	}
	case MINT_ICALL_PPPPPP_V: {
		typedef void (*T)(gpointer,gpointer,gpointer,gpointer,gpointer,gpointer);
		T func = (T)ptr;
		func (sp [0].data.p, sp [1].data.p, sp [2].data.p, sp [3].data.p, sp [4].data.p, sp [5].data.p);
		break;
	}
	case MINT_ICALL_PPPPPP_P: {
		typedef gpointer (*T)(gpointer,gpointer,gpointer,gpointer,gpointer,gpointer);
		T func = (T)ptr;
		sp [0].data.p = func (sp [0].data.p, sp [1].data.p, sp [2].data.p, sp [3].data.p, sp [4].data.p, sp [5].data.p);
		break;
	}
	default:
		g_assert_not_reached ();
	}

	if (save_last_error)
		mono_marshal_set_last_error ();

	/* convert the native representation to the stackval representation */
	if (sig)
		stackval_from_data (sig->ret, &sp [0], (char*) &sp [0].data.p, sig->pinvoke);
}

/* MONO_NO_OPTIMIZATION is needed due to usage of INTERP_PUSH_LMF_WITH_CTX. */
#ifdef _MSC_VER
#pragma optimize ("", off)
#endif
// Do not inline in case order of frame addresses matters, and maybe other reasons.
static MONO_NO_OPTIMIZATION MONO_NEVER_INLINE gpointer
do_icall_wrapper (InterpFrame *frame, MonoMethodSignature *sig, int op, stackval *sp, gpointer ptr, gboolean save_last_error)
{
	MonoLMFExt ext;
	INTERP_PUSH_LMF_WITH_CTX (frame, ext, exit_icall);

	do_icall (sig, op, sp, ptr, save_last_error);

	interp_pop_lmf (&ext);

	goto exit_icall; // prevent unused label warning in some configurations
	/* If an exception is thrown from native code, execution will continue here */
exit_icall:
	return NULL;
}
#ifdef _MSC_VER
#pragma optimize ("", on)
#endif

void
mono_interp_do_icall (InterpFrame *frame, MonoMethodSignature *sig, int op, stackval *sp,
		gpointer ptr, gboolean save_last_error)
{
	do_icall_wrapper (frame, sig, op, sp, ptr, save_last_error);
}

gpointer
mono_interp_invocation_anchor (ThreadContext *context)
{
	if (!context->handle_mark_count)
		return NULL;

	return context->handle_marks [context->handle_mark_count - 1].frame;
}

typedef struct {
	int pindex;
	gpointer jit_wrapper;
	gpointer *args;
	MonoFtnDesc ftndesc;
} JitCallCbData;

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

/*
 * Fill the buffer ArgIterator walks: the call-site signature in the first word,
 * then the variable arguments behind it.
 *
 * ves_icall_System_ArgIterator_IntGetNextArg () advances by mono_type_stack_size
 * and realigns only on arm and mips, so everywhere else the offsets are that
 * running sum and nothing more. A float is four bytes rather than a whole slot,
 * so padding one out to its slot would leave every argument behind it misread.
 */
static void
init_arglist (InterpFrame *frame, MonoMethodSignature *sig, stackval *sp, char *arglist)
{
	*(gpointer*)arglist = sig;
	arglist += sizeof (gpointer);

	for (int i = sig->sentinelpos; i < sig->param_count; i++) {
		int arg_size, sv_size;
#if defined(__arm__) || defined(__mips__)
		int align;

		arg_size = mono_type_stack_size (sig->params [i], &align);
		arglist = (char*)ALIGN_PTR_TO (arglist, align);
#else
		arg_size = mono_type_stack_size (sig->params [i], NULL);
#endif

		sv_size = stackval_to_data (sig->params [i], sp, arglist, FALSE);
		arglist += arg_size;
		sp = STACK_ADD_BYTES (sp, sv_size);
	}
}

/*
 * These functions are the entry points into the interpreter from compiled code.
 * They are called by the interp_in wrappers. They have the following signature:
 * void (<optional this_arg>, <optional retval pointer>, <arg1>, ..., <argn>, <method ptr>)
 * They pack up their arguments into an InterpEntryData structure and call mono_interp_entry ().
 * It would be possible for the wrappers to pack up the arguments etc, but that would make them bigger, and there are
 * more wrappers then these functions.
 * this/static * ret/void * 16 arguments -> 64 functions.
 */

#define INTERP_ENTRY_BASE(_method, _this_arg, _res) \
	InterpEntryData data; \
	(data).rmethod = (_method); \
	(data).res = (_res); \
	(data).this_arg = (_this_arg); \
	(data).many_args = NULL;

#define INTERP_ENTRY0(_this_arg, _res, _method) {	\
	INTERP_ENTRY_BASE (_method, _this_arg, _res); \
	mono_interp_entry (&data); \
	}
#define INTERP_ENTRY1(_this_arg, _res, _method) {	  \
	INTERP_ENTRY_BASE (_method, _this_arg, _res); \
	(data).args [0] = arg1; \
	mono_interp_entry (&data); \
	}
#define INTERP_ENTRY2(_this_arg, _res, _method) {  \
	INTERP_ENTRY_BASE (_method, _this_arg, _res); \
	(data).args [0] = arg1; \
	(data).args [1] = arg2; \
	mono_interp_entry (&data); \
	}
#define INTERP_ENTRY3(_this_arg, _res, _method) { \
	INTERP_ENTRY_BASE (_method, _this_arg, _res); \
	(data).args [0] = arg1; \
	(data).args [1] = arg2; \
	(data).args [2] = arg3; \
	mono_interp_entry (&data); \
	}
#define INTERP_ENTRY4(_this_arg, _res, _method) {	\
	INTERP_ENTRY_BASE (_method, _this_arg, _res); \
	(data).args [0] = arg1; \
	(data).args [1] = arg2; \
	(data).args [2] = arg3; \
	(data).args [3] = arg4; \
	mono_interp_entry (&data); \
	}
#define INTERP_ENTRY5(_this_arg, _res, _method) {	\
	INTERP_ENTRY_BASE (_method, _this_arg, _res); \
	(data).args [0] = arg1; \
	(data).args [1] = arg2; \
	(data).args [2] = arg3; \
	(data).args [3] = arg4; \
	(data).args [4] = arg5; \
	mono_interp_entry (&data); \
	}
#define INTERP_ENTRY6(_this_arg, _res, _method) {	\
	INTERP_ENTRY_BASE (_method, _this_arg, _res); \
	(data).args [0] = arg1; \
	(data).args [1] = arg2; \
	(data).args [2] = arg3; \
	(data).args [3] = arg4; \
	(data).args [4] = arg5; \
	(data).args [5] = arg6; \
	mono_interp_entry (&data); \
	}
#define INTERP_ENTRY7(_this_arg, _res, _method) {	\
	INTERP_ENTRY_BASE (_method, _this_arg, _res); \
	(data).args [0] = arg1; \
	(data).args [1] = arg2; \
	(data).args [2] = arg3; \
	(data).args [3] = arg4; \
	(data).args [4] = arg5; \
	(data).args [5] = arg6; \
	(data).args [6] = arg7; \
	mono_interp_entry (&data); \
	}
#define INTERP_ENTRY8(_this_arg, _res, _method) {	\
	INTERP_ENTRY_BASE (_method, _this_arg, _res); \
	(data).args [0] = arg1; \
	(data).args [1] = arg2; \
	(data).args [2] = arg3; \
	(data).args [3] = arg4; \
	(data).args [4] = arg5; \
	(data).args [5] = arg6; \
	(data).args [6] = arg7; \
	(data).args [7] = arg8; \
	mono_interp_entry (&data); \
	}

#define ARGLIST0 InterpMethod *rmethod
#define ARGLIST1 gpointer arg1, InterpMethod *rmethod
#define ARGLIST2 gpointer arg1, gpointer arg2, InterpMethod *rmethod
#define ARGLIST3 gpointer arg1, gpointer arg2, gpointer arg3, InterpMethod *rmethod
#define ARGLIST4 gpointer arg1, gpointer arg2, gpointer arg3, gpointer arg4, InterpMethod *rmethod
#define ARGLIST5 gpointer arg1, gpointer arg2, gpointer arg3, gpointer arg4, gpointer arg5, InterpMethod *rmethod
#define ARGLIST6 gpointer arg1, gpointer arg2, gpointer arg3, gpointer arg4, gpointer arg5, gpointer arg6, InterpMethod *rmethod
#define ARGLIST7 gpointer arg1, gpointer arg2, gpointer arg3, gpointer arg4, gpointer arg5, gpointer arg6, gpointer arg7, InterpMethod *rmethod
#define ARGLIST8 gpointer arg1, gpointer arg2, gpointer arg3, gpointer arg4, gpointer arg5, gpointer arg6, gpointer arg7, gpointer arg8, InterpMethod *rmethod

static void interp_entry_static_0 (ARGLIST0) INTERP_ENTRY0 (NULL, NULL, rmethod)
static void interp_entry_static_1 (ARGLIST1) INTERP_ENTRY1 (NULL, NULL, rmethod)
static void interp_entry_static_2 (ARGLIST2) INTERP_ENTRY2 (NULL, NULL, rmethod)
static void interp_entry_static_3 (ARGLIST3) INTERP_ENTRY3 (NULL, NULL, rmethod)
static void interp_entry_static_4 (ARGLIST4) INTERP_ENTRY4 (NULL, NULL, rmethod)
static void interp_entry_static_5 (ARGLIST5) INTERP_ENTRY5 (NULL, NULL, rmethod)
static void interp_entry_static_6 (ARGLIST6) INTERP_ENTRY6 (NULL, NULL, rmethod)
static void interp_entry_static_7 (ARGLIST7) INTERP_ENTRY7 (NULL, NULL, rmethod)
static void interp_entry_static_8 (ARGLIST8) INTERP_ENTRY8 (NULL, NULL, rmethod)
static void interp_entry_static_ret_0 (gpointer res, ARGLIST0) INTERP_ENTRY0 (NULL, res, rmethod)
static void interp_entry_static_ret_1 (gpointer res, ARGLIST1) INTERP_ENTRY1 (NULL, res, rmethod)
static void interp_entry_static_ret_2 (gpointer res, ARGLIST2) INTERP_ENTRY2 (NULL, res, rmethod)
static void interp_entry_static_ret_3 (gpointer res, ARGLIST3) INTERP_ENTRY3 (NULL, res, rmethod)
static void interp_entry_static_ret_4 (gpointer res, ARGLIST4) INTERP_ENTRY4 (NULL, res, rmethod)
static void interp_entry_static_ret_5 (gpointer res, ARGLIST5) INTERP_ENTRY5 (NULL, res, rmethod)
static void interp_entry_static_ret_6 (gpointer res, ARGLIST6) INTERP_ENTRY6 (NULL, res, rmethod)
static void interp_entry_static_ret_7 (gpointer res, ARGLIST7) INTERP_ENTRY7 (NULL, res, rmethod)
static void interp_entry_static_ret_8 (gpointer res, ARGLIST8) INTERP_ENTRY8 (NULL, res, rmethod)
static void interp_entry_instance_0 (gpointer this_arg, ARGLIST0) INTERP_ENTRY0 (this_arg, NULL, rmethod)
static void interp_entry_instance_1 (gpointer this_arg, ARGLIST1) INTERP_ENTRY1 (this_arg, NULL, rmethod)
static void interp_entry_instance_2 (gpointer this_arg, ARGLIST2) INTERP_ENTRY2 (this_arg, NULL, rmethod)
static void interp_entry_instance_3 (gpointer this_arg, ARGLIST3) INTERP_ENTRY3 (this_arg, NULL, rmethod)
static void interp_entry_instance_4 (gpointer this_arg, ARGLIST4) INTERP_ENTRY4 (this_arg, NULL, rmethod)
static void interp_entry_instance_5 (gpointer this_arg, ARGLIST5) INTERP_ENTRY5 (this_arg, NULL, rmethod)
static void interp_entry_instance_6 (gpointer this_arg, ARGLIST6) INTERP_ENTRY6 (this_arg, NULL, rmethod)
static void interp_entry_instance_7 (gpointer this_arg, ARGLIST7) INTERP_ENTRY7 (this_arg, NULL, rmethod)
static void interp_entry_instance_8 (gpointer this_arg, ARGLIST8) INTERP_ENTRY8 (this_arg, NULL, rmethod)
static void interp_entry_instance_ret_0 (gpointer this_arg, gpointer res, ARGLIST0) INTERP_ENTRY0 (this_arg, res, rmethod)
static void interp_entry_instance_ret_1 (gpointer this_arg, gpointer res, ARGLIST1) INTERP_ENTRY1 (this_arg, res, rmethod)
static void interp_entry_instance_ret_2 (gpointer this_arg, gpointer res, ARGLIST2) INTERP_ENTRY2 (this_arg, res, rmethod)
static void interp_entry_instance_ret_3 (gpointer this_arg, gpointer res, ARGLIST3) INTERP_ENTRY3 (this_arg, res, rmethod)
static void interp_entry_instance_ret_4 (gpointer this_arg, gpointer res, ARGLIST4) INTERP_ENTRY4 (this_arg, res, rmethod)
static void interp_entry_instance_ret_5 (gpointer this_arg, gpointer res, ARGLIST5) INTERP_ENTRY5 (this_arg, res, rmethod)
static void interp_entry_instance_ret_6 (gpointer this_arg, gpointer res, ARGLIST6) INTERP_ENTRY6 (this_arg, res, rmethod)
static void interp_entry_instance_ret_7 (gpointer this_arg, gpointer res, ARGLIST7) INTERP_ENTRY7 (this_arg, res, rmethod)
static void interp_entry_instance_ret_8 (gpointer this_arg, gpointer res, ARGLIST8) INTERP_ENTRY8 (this_arg, res, rmethod)

#define INTERP_ENTRY_FUNCLIST(type) (gpointer)interp_entry_ ## type ## _0, (gpointer)interp_entry_ ## type ## _1, (gpointer)interp_entry_ ## type ## _2, (gpointer)interp_entry_ ## type ## _3, (gpointer)interp_entry_ ## type ## _4, (gpointer)interp_entry_ ## type ## _5, (gpointer)interp_entry_ ## type ## _6, (gpointer)interp_entry_ ## type ## _7, (gpointer)interp_entry_ ## type ## _8

static gpointer entry_funcs_static [MAX_INTERP_ENTRY_ARGS + 1] = { INTERP_ENTRY_FUNCLIST (static) };
static gpointer entry_funcs_static_ret [MAX_INTERP_ENTRY_ARGS + 1] = { INTERP_ENTRY_FUNCLIST (static_ret) };
static gpointer entry_funcs_instance [MAX_INTERP_ENTRY_ARGS + 1] = { INTERP_ENTRY_FUNCLIST (instance) };
static gpointer entry_funcs_instance_ret [MAX_INTERP_ENTRY_ARGS + 1] = { INTERP_ENTRY_FUNCLIST (instance_ret) };


/*
 * The interpreter's record for a method in the current domain, creating one if
 * it has none. Says who would run the method; does not transform it.
 */
static gpointer
interp_get_imethod (MonoMethod *method, MonoError *error)
{
	return mono_interp_get_imethod (mono_domain_get (), method, error);
}

static InterpMethod*
lookup_method_pointer (MonoDomain *domain, gpointer addr)
{
	MonoJitDomainInfo *info = domain_jit_info (domain);
	InterpMethod *res = NULL;

	mono_domain_lock (domain);
	if (info->interp_method_pointer_hash)
		res = (InterpMethod*)g_hash_table_lookup (info->interp_method_pointer_hash, addr);
	mono_domain_unlock (domain);

	return res;
}

/*
 * The entry point standing for IMETHOD, creating one if it has none yet.
 */
static gpointer
entry_for_imethod (InterpMethod *imethod, MonoError *error)
{
	if (imethod->jit_entry)
		return imethod->jit_entry;

	return interp_create_method_pointer (imethod->method, FALSE, error);
}

/*
 * Returns the address that stands for imethod outside this engine, and records
 * that the address is now in native hands.
 *
 * A patcher writes a jump over the address it is given, so a method must have one
 * address and not one per engine. A compiled ldftn names the backend's stub, and
 * this gives an interpreted one the same stub. No compile happens here: the stub
 * is minted on its own, which is what a compiled caller's ldftn does as well.
 *
 * With the interpreter as the whole engine there is no backend to ask, and its own
 * entry is the only address there is.
 */
static gpointer
escaping_entry_for_imethod (InterpMethod *imethod, MonoError *error)
{
	mono_method_entry_escaped (imethod->method);

	if (mono_ee_features.force_use_interpreter)
		return entry_for_imethod (imethod, error);

	return mono_llvm_jit_stub_for (imethod->method, imethod->domain, error);
}

/*
 * The method an entry point stands for, or NULL if this domain published no such
 * entry.
 *
 * ldftn's product is an entry point in both engines, so a delegate constructor - or
 * anything else handed one - recovers the method by asking the engine that published
 * it: the jit-info table for a compiled entry, this for an interpreted one.
 */
static MonoMethod*
interp_method_from_entry (MonoDomain *domain, gpointer addr)
{
	InterpMethod *imethod = lookup_method_pointer (domain, addr);

	return imethod ? imethod->method : NULL;
}

/*
 * The InterpMethod to execute for the entry point ADDR, whichever engine published
 * it. Returns NULL for a pointer that names no method at all.
 */
static InterpMethod*
imethod_for_entry (MonoDomain *domain, gpointer addr, MonoError *error)
{
	InterpMethod *imethod = lookup_method_pointer (domain, addr);

	if (imethod)
		return imethod;

	/*
	 * A compiled entry: the method is the one whose code that address falls in.
	 * Executing it here rather than jumping to the code keeps a single engine in
	 * charge of the frame.
	 */
	MonoJitInfo *ji = mono_jit_info_table_find_internal (
		domain, mono_get_addr_from_ftnptr (MINI_FTNPTR_TO_ADDR (addr)), TRUE, TRUE);

	if (!ji)
		return NULL;

	/*
	 * A method's published address is its stub, and a stub is registered as a
	 * trampoline. The record still says which method it stands for, so a
	 * trampoline is an answer rather than a refusal. Some trampolines belong to
	 * no method, and those are the ones with nothing to return.
	 */
	MonoMethod *method = ji->is_trampoline ? ji->d.tramp_info->method
	                                       : mono_jit_info_get_method (ji);

	if (!method)
		return NULL;

	return mono_interp_get_imethod (domain, method, error);
}

#ifndef MONO_ARCH_HAVE_INTERP_NATIVE_TO_MANAGED
static void
interp_no_native_to_managed (void)
{
	g_error ("interpreter: native-to-managed transition not available on this platform");
}
#endif

static void
no_llvmonly_interp_method_pointer (void)
{
	g_assert_not_reached ();
}

/*
 * interp_create_method_pointer_llvmonly:
 *
 *   Return an ftndesc for entering the interpreter and executing METHOD.
 */
static MonoFtnDesc*
interp_create_method_pointer_llvmonly (MonoMethod *method, gboolean unbox, MonoError *error)
{
	MonoDomain *domain = mono_domain_get ();
	gpointer addr, entry_func, entry_wrapper;
	MonoMethodSignature *sig;
	MonoMethod *wrapper;
	MonoJitDomainInfo *info;
	InterpMethod *imethod;

	imethod = mono_interp_get_imethod (domain, method, error);
	return_val_if_nok (error, NULL);

	if (unbox) {
		if (imethod->llvmonly_unbox_entry)
			return (MonoFtnDesc*)imethod->llvmonly_unbox_entry;
	} else {
		if (imethod->jit_entry)
			return (MonoFtnDesc*)imethod->jit_entry;
	}

	sig = mono_method_signature_internal (method);

	/*
	 * The entry functions need access to the method to call, so we have
	 * to use a ftndesc. The caller uses a normal signature, while the
	 * entry functions use a gsharedvt_in signature, so wrap the entry function in
	 * a gsharedvt_in_sig wrapper.
	 * We use a gsharedvt_in_sig wrapper instead of an interp_in wrapper, because they
	 * are mostly the same, and they are already generated. The exception is the
	 * wrappers for methods with more than 8 arguments, those are different.
	 */
	if (sig->param_count > MAX_INTERP_ENTRY_ARGS)
		wrapper = mini_get_interp_in_wrapper (sig);
	else
		wrapper = mini_get_gsharedvt_in_sig_wrapper (sig);

	entry_wrapper = mono_jit_compile_method_jit_only (wrapper, error);
	mono_error_assertf_ok (error, "couldn't compile wrapper \"%s\" for \"%s\"",
			mono_method_get_name_full (wrapper, TRUE, TRUE, MONO_TYPE_NAME_FORMAT_IL),
			mono_method_get_name_full (method,  TRUE, TRUE, MONO_TYPE_NAME_FORMAT_IL));

	if (sig->param_count > MAX_INTERP_ENTRY_ARGS) {
		entry_func = (gpointer)mono_interp_entry_general;
	} else if (sig->hasthis) {
		if (sig->ret->type == MONO_TYPE_VOID)
			entry_func = entry_funcs_instance [sig->param_count];
		else
			entry_func = entry_funcs_instance_ret [sig->param_count];
	} else {
		if (sig->ret->type == MONO_TYPE_VOID)
			entry_func = entry_funcs_static [sig->param_count];
		else
			entry_func = entry_funcs_static_ret [sig->param_count];
	}
	g_assert (entry_func);

	/* Encode unbox in the lower bit of imethod */
	gpointer entry_arg = imethod;
	if (unbox)
		entry_arg = (gpointer)(((gsize)entry_arg) | 1);
	MonoFtnDesc *entry_ftndesc = mini_llvmonly_create_ftndesc (mono_domain_get (), entry_func, entry_arg);

	addr = mini_llvmonly_create_ftndesc (mono_domain_get (), entry_wrapper, entry_ftndesc);

	info = domain_jit_info (domain);
	mono_domain_lock (domain);
	if (!info->interp_method_pointer_hash)
		info->interp_method_pointer_hash = g_hash_table_new (NULL, NULL);
	g_hash_table_insert (info->interp_method_pointer_hash, addr, imethod);
	mono_domain_unlock (domain);

	mono_memory_barrier ();
	if (unbox)
		imethod->llvmonly_unbox_entry = addr;
	else
		imethod->jit_entry = addr;

	return (MonoFtnDesc*)addr;
}

/*
 * interp_transform_method:
 *
 * Transform METHOD now rather than on the first call into it, so that whether
 * the interpreter can run it at all is known before anything can. Returns FALSE
 * with ERROR set when it cannot.
 *
 * This is what create_method_pointer () does for itself when asked to compile,
 * and is separate so that a caller which has to publish the entry first can
 * still find out. Transforming runs the class initializer, so a caller holding
 * a lock that managed code could reach has to get the entry reachable before it
 * asks.
 */
static gboolean
interp_transform_method (MonoMethod *method, MonoError *error)
{
	InterpMethod *imethod = mono_interp_get_imethod (mono_domain_get (), method, error);

	if (!is_ok (error))
		return FALSE;

	if (imethod->transformed)
		return TRUE;

	mono_interp_transform_method (imethod, mono_interp_get_context (), error);
	return is_ok (error);
}

/*
 * interp_create_method_pointer:
 *
 * Return a function pointer which can be used to call METHOD using the
 * interpreter. Return NULL for methods which are not supported.
 */
static gpointer
interp_create_method_pointer (MonoMethod *method, gboolean compile, MonoError *error)
{
	gpointer addr, entry_func, entry_wrapper = NULL;
	MonoDomain *domain = mono_domain_get ();
	MonoJitDomainInfo *info;
	InterpMethod *imethod = mono_interp_get_imethod (domain, method, error);

	if (imethod->jit_entry)
		return imethod->jit_entry;

	if (compile && !imethod->transformed) {
		/* Return any errors from method compilation */
		mono_interp_transform_method (imethod, mono_interp_get_context (), error);
		return_val_if_nok (error, NULL);
	}

	MonoMethodSignature *sig = mono_method_signature_internal (method);
	if (method->string_ctor) {
		MonoMethodSignature *newsig = (MonoMethodSignature*)g_alloca (MONO_SIZEOF_METHOD_SIGNATURE + ((sig->param_count + 2) * sizeof (MonoType*)));
		memcpy (newsig, sig, mono_metadata_signature_size (sig));
		newsig->ret = m_class_get_byval_arg (mono_defaults.string_class);
		sig = newsig;
	}

	if (sig->param_count > MAX_INTERP_ENTRY_ARGS) {
		entry_func = (gpointer)mono_interp_entry_general;
	} else if (sig->hasthis) {
		if (sig->ret->type == MONO_TYPE_VOID)
			entry_func = entry_funcs_instance [sig->param_count];
		else
			entry_func = entry_funcs_instance_ret [sig->param_count];
	} else {
		if (sig->ret->type == MONO_TYPE_VOID)
			entry_func = entry_funcs_static [sig->param_count];
		else
			entry_func = entry_funcs_static_ret [sig->param_count];
	}

#ifndef MONO_ARCH_HAVE_INTERP_NATIVE_TO_MANAGED
#ifdef HOST_WASM
	if (method->wrapper_type == MONO_WRAPPER_NATIVE_TO_MANAGED) {
		WrapperInfo *info = mono_marshal_get_wrapper_info (method);
		MonoMethod *orig_method = info->d.native_to_managed.method;

		/*
		 * These are called from native code. Ask the host app for a trampoline.
		 */
		MonoFtnDesc *ftndesc = g_new0 (MonoFtnDesc, 1);
		ftndesc->addr = entry_func;
		ftndesc->arg = imethod;

		addr = mono_wasm_get_native_to_interp_trampoline (orig_method, ftndesc);
		if (addr) {
			mono_memory_barrier ();
			imethod->jit_entry = addr;
			return addr;
		}

#ifdef ENABLE_NETCORE
		/*
		 * The runtime expects a function pointer unique to method and
		 * the native caller expects a function pointer with the
		 * right signature, so fail right away.
		 */
		mono_error_set_platform_not_supported (error, "No native to managed transitions on this platform.");
		return NULL;
#endif
	}
#endif
	return (gpointer)interp_no_native_to_managed;
#endif

	if (mono_llvm_only) {
		/* The caller should call interp_create_method_pointer_llvmonly */
		//g_assert_not_reached ();
		return (gpointer)no_llvmonly_interp_method_pointer;
	}

	if (method->wrapper_type && method->wrapper_type == MONO_WRAPPER_MANAGED_TO_NATIVE)
		return imethod;

#ifndef MONO_ARCH_HAVE_FTNPTR_ARG_TRAMPOLINE
	/*
	 * Interp in wrappers get the argument in the rgctx register. If
	 * MONO_ARCH_HAVE_FTNPTR_ARG_TRAMPOLINE is defined it means that
	 * on that arch the rgctx register is not scratch, so we use a
	 * separate temp register. We should update the wrappers for this
	 * if we really care about those architectures (arm).
	 */
	MonoMethod *wrapper = mini_get_interp_in_wrapper (sig);

	entry_wrapper = mono_jit_compile_method_jit_only (wrapper, error);
#endif
	if (!entry_wrapper) {
#ifndef MONO_ARCH_HAVE_INTERP_ENTRY_TRAMPOLINE
		g_assertion_message ("couldn't compile wrapper \"%s\" for \"%s\"",
				mono_method_get_name_full (wrapper, TRUE, TRUE, MONO_TYPE_NAME_FORMAT_IL),
				mono_method_get_name_full (method,  TRUE, TRUE, MONO_TYPE_NAME_FORMAT_IL));
#else
		mono_interp_error_cleanup (error);
		if (!mono_native_to_interp_trampoline) {
			if (mono_aot_only) {
				mono_native_to_interp_trampoline = (MonoFuncV)mono_aot_get_trampoline ("native_to_interp_trampoline");
			} else {
				MonoTrampInfo *info;
				mono_native_to_interp_trampoline = (MonoFuncV)mono_arch_get_native_to_interp_trampoline (&info);
				mono_tramp_info_register (info, NULL);
			}
		}
		entry_wrapper = (gpointer)mono_native_to_interp_trampoline;
		/* We need the lmf wrapper only when being called from mixed mode */
		if (sig->pinvoke)
			entry_func = (gpointer)mono_interp_entry_from_ccontext;
		else {
			static gpointer cached_func = NULL;
			if (!cached_func) {
				cached_func = mono_jit_compile_method_jit_only (mini_get_interp_lmf_wrapper ("mono_interp_entry_from_trampoline", (gpointer) mono_interp_entry_from_trampoline), error);
				mono_memory_barrier ();
			}
			entry_func = cached_func;
		}
#endif
	}

	g_assert (entry_func);
	/* This is the argument passed to the interp_in wrapper by the static rgctx trampoline */
	MonoFtnDesc *ftndesc = g_new0 (MonoFtnDesc, 1);
	ftndesc->addr = entry_func;
	ftndesc->arg = imethod;
	mono_error_assert_ok (error);

	/*
	 * The wrapper is called by compiled code, which doesn't pass the extra argument, so we pass it in the
	 * rgctx register using a trampoline.
	 */

	addr = mono_create_ftnptr_arg_trampoline (ftndesc, entry_wrapper);

	info = domain_jit_info (domain);
	mono_domain_lock (domain);
	if (!info->interp_method_pointer_hash)
		info->interp_method_pointer_hash = g_hash_table_new (NULL, NULL);
	g_hash_table_insert (info->interp_method_pointer_hash, addr, imethod);
	mono_domain_unlock (domain);

	mono_memory_barrier ();
	imethod->jit_entry = addr;

	return addr;
}

static void
interp_free_method (MonoDomain *domain, MonoMethod *method)
{
	MonoJitDomainInfo *info = domain_jit_info (domain);

	mono_domain_jit_code_hash_lock (domain);
	/* InterpMethod is allocated in the domain mempool. We might haven't
	 * allocated an InterpMethod for this instance yet */
	mono_internal_hash_table_remove (&info->interp_code_hash, method);
	mono_domain_jit_code_hash_unlock (domain);
}

/*
 * METHOD now has native code in DOMAIN, so calls to it from interpreted code go
 * there instead of interpreting it. Nothing is created here: a method the
 * interpreter has never seen settles this for itself the first time it is
 * called, and it can only be settled the other way while there is no code.
 */
static void
interp_method_compiled (MonoDomain *domain, MonoMethod *method)
{
	InterpMethod *imethod = lookup_imethod (domain, method);

	if (imethod == NULL || imethod->code_type == IMETHOD_CODE_COMPILED)
		return;

	if (mono_interp_jit_call_marshallable (method, mono_method_signature_internal (method)))
		imethod->code_type = IMETHOD_CODE_COMPILED;
}

/*
 * Takes back the answer resolve_code_type () settled for this method, because the
 * address it is entered at is now in native hands. Native code can write a jump
 * over that address, and an answer taken before that did not know calls have to
 * go through it.
 */
static void
interp_entry_escaped (MonoDomain *domain, MonoMethod *method)
{
	InterpMethod *imethod = lookup_imethod (domain, method);

	if (imethod == NULL)
		return;

	mono_atomic_cas_i32 ((gint32*)&imethod->code_type, IMETHOD_CODE_UNKNOWN,
	                     IMETHOD_CODE_INTERP);
}

#if COUNT_OPS
static long opcode_counts[MINT_LASTOP];

#define COUNT_OP(op) opcode_counts[op]++
#else
#define COUNT_OP(op) 
#endif

#if DEBUG_INTERP
#define DUMP_INSTR() \
	if (tracing > 1) { \
		output_indent (); \
		char *mn = mono_method_full_name (frame->imethod->method, FALSE); \
		char *disasm = mono_interp_dis_mintop ((gint32)(ip - frame->imethod->code), TRUE, ip + 1, *ip); \
		g_print ("(%p) %s -> %s\n", mono_thread_internal_current (), mn, disasm); \
		g_free (mn); \
		g_free (disasm); \
	}
#else
#define DUMP_INSTR()
#endif

// Do not inline in case order of frame addresses matters.
MONO_NEVER_INLINE MonoException*
mono_interp_leave (InterpFrame* parent_frame)
{
	InterpFrame frame = {parent_frame};

	frame_stamp_ordinal (mono_interp_get_context (), &frame);

	stackval tmp_sp;
	/*
	 * We need for mono_thread_get_undeniable_exception to be able to unwind
	 * to check the abort threshold. For this to work we use frame as a
	 * dummy frame that is stored in the lmf and serves as the transition frame
	 */
	do_icall_wrapper (&frame, NULL, MINT_ICALL_V_P, &tmp_sp, (gpointer)mono_thread_get_undeniable_exception, FALSE);

	return (MonoException*)tmp_sp.data.p;
}

// varargs in wasm consumes extra linear stack per call-site.
// These g_warning/g_error wrappers fix that. It is not the
// small wasm stack, but conserving it is still desirable.
static void
g_warning_d (const char *format, int d)
{
	g_warning (format, d);
}

#if PROFILE_INTERP
static long total_executed_opcodes;
#endif

static void
interp_parse_options (const char *options)
{
	char **args, **ptr;

	if (!options)
		return;

	args = g_strsplit (options, ",", -1);
	for (ptr = args; ptr && *ptr; ptr ++) {
		char *arg = *ptr;

		if (strncmp (arg, "jit=", 4) == 0)
			mono_interp_jit_classes = g_slist_prepend (mono_interp_jit_classes, arg + 4);
		else if (strncmp (arg, "interp-only=", strlen ("interp-only=")) == 0)
			mono_interp_only_classes = g_slist_prepend (mono_interp_only_classes, arg + strlen ("interp-only="));
		else if (strncmp (arg, "-inline", 7) == 0)
			mono_interp_opt &= ~INTERP_OPT_INLINE;
		else if (strncmp (arg, "-cprop", 6) == 0)
			mono_interp_opt &= ~INTERP_OPT_CPROP;
		else if (strncmp (arg, "-super", 6) == 0)
			mono_interp_opt &= ~INTERP_OPT_SUPER_INSTRUCTIONS;
		else if (strncmp (arg, "-bblocks", 8) == 0)
			mono_interp_opt &= ~INTERP_OPT_BBLOCKS;
		else if (strncmp (arg, "-all", 4) == 0)
			mono_interp_opt = INTERP_OPT_NONE;
	}
}

/*
 * interp_release_abandoned_handles:
 *
 *   Give back the handles held by the interpreter frames an exception resume is
 * about to skip.
 */
static void
interp_release_abandoned_handles (MonoJitTlsData *jit_tls, gpointer resume_sp)
{
	g_assert (jit_tls);
	ThreadContext *context = (ThreadContext*)jit_tls->interp_context;
	if (!context)
		return;

	/*
	 * Resuming restores the stack pointer over every frame below the one it
	 * resumes into, so those interp_exec_method () invocations never reach
	 * their own HANDLE_FUNCTION_RETURN. Entries are in stack order, so the
	 * ones being skipped are a suffix, and restoring the outermost of them
	 * hands back every handle above it at once.
	 */
	int first = context->handle_mark_count;

	while (first > 0 && context->handle_marks [first - 1].frame < resume_sp)
		first--;

	gboolean dropped = first < context->handle_mark_count;
	guchar *watermark = NULL;
	gsize first_ordinal = 0;

	if (dropped) {
		watermark = context->handle_marks [first].frame_watermark;
		first_ordinal = context->handle_marks [first].first_ordinal;

		mono_stack_mark_pop (mono_thread_info_current (), &context->handle_marks [first].mark);
		context->handle_mark_count = first;
	}

	/*
	 * The skipped invocations do not restore anything else either. Their frames go
	 * back here, because nothing on a frame the resume jumps over runs again, and the
	 * marker goes with them: nothing here knows which frame of the invocation below is
	 * current, so say nothing rather than name a dead one.
	 */
	if (dropped) {
		context->frame_stack_pointer = watermark;

		if (context->current_frame && context->current_frame->ordinal >= first_ordinal)
			context_set_current_frame (context, NULL);
	}
}

/*
 * interp_set_resume_state:
 *
 *   Set the state the interpeter will continue to execute from after execution returns to the interpreter.
 */
static void
interp_set_resume_state (MonoJitTlsData *jit_tls, MonoObject *ex, MonoJitExceptionInfo *ei, MonoInterpFrameHandle interp_frame, gpointer handler_ip)
{
	ThreadContext *context;

	g_assert (jit_tls);
	context = (ThreadContext*)jit_tls->interp_context;
	g_assert (context);

	context->has_resume_state = TRUE;
	context->handler_frame = (InterpFrame*)interp_frame;
	context->handler_ei = ei;
	if (context->exc_gchandle)
		mono_gchandle_free_internal (context->exc_gchandle);
	context->exc_gchandle = mono_gchandle_new_internal ((MonoObject*)ex, FALSE);
	/* Ditto */
	if (ei)
		*(MonoObject**)(frame_locals (context->handler_frame) + ei->exvar_offset) = ex;
	context->handler_ip = (const guint16*)handler_ip;
}

static void
interp_get_resume_state (const MonoJitTlsData *jit_tls, gboolean *has_resume_state, MonoInterpFrameHandle *interp_frame, gpointer *handler_ip)
{
	g_assert (jit_tls);
	ThreadContext *context = (ThreadContext*)jit_tls->interp_context;

	*has_resume_state = context ? context->has_resume_state : FALSE;
	if (!*has_resume_state)
		return;

	*interp_frame = context->handler_frame;
	*handler_ip = (gpointer)context->handler_ip;
}

typedef struct {
	InterpFrame *current;
} StackIter;

static const guint8*
decode_uleb128 (const guint8 *p, guint32 *out)
{
	guint32 value = 0;
	int shift = 0;

	while (TRUE) {
		guint8 b = *p++;

		value |= (guint32) (b & 0x7f) << shift;
		if (!(b & 0x80))
			break;
		shift += 7;
	}

	*out = value;
	return p;
}

/*
 * imethod_il_offset:
 *
 *   Return the IL offset in effect at an offset into a method's bytecode, or -1
 * if it is not known. A compiled body answers the same question from its own
 * per-body map, so a stack trace reads the same either side of a promotion.
 */
static int
imethod_il_offset (InterpMethod *imethod, int native_offset)
{
	if (!imethod || !imethod->line_numbers || native_offset < 0)
		return -1;

	const guint8 *p = imethod->line_numbers;
	const guint8 *end = p + imethod->line_numbers_size;
	guint32 native = 0;
	gint32 il = 0;
	int result = -1;

	/*
	 * The entries ascend by bytecode offset, so the one in effect is the last
	 * starting at or before the offset asked about. Walking them is fine: this
	 * runs only while a stack trace is being built.
	 */
	while (p < end) {
		guint32 native_delta, il_delta;

		p = decode_uleb128 (p, &native_delta);
		p = decode_uleb128 (p, &il_delta);

		native += native_delta;
		il += (gint32) (il_delta >> 1) ^ -(gint32) (il_delta & 1);

		if ((int) native > native_offset)
			break;

		result = il;
	}

	return result;
}

/*
 * interp_il_offset_from_native_offset:
 *
 *   Return the IL offset in effect at an offset into a method's bytecode, for a
 * method named rather than held. Finding it means the per-domain table, so this
 * is for callers that can take a lock.
 */
static int
interp_il_offset_from_native_offset (MonoDomain *domain, MonoMethod *method, int native_offset)
{
	return imethod_il_offset (lookup_imethod (domain, method), native_offset);
}

/*
 * interp_frame_il_offset:
 *
 *   Return the IL offset in effect in a frame. A frame holds its own method, so
 * this reaches the map without a lock and a signal handler can ask.
 */
static int
interp_frame_il_offset (MonoInterpFrameHandle frame, int native_offset)
{
	InterpFrame *iframe = (InterpFrame*)frame;

	return imethod_il_offset (iframe ? iframe->imethod : NULL, native_offset);
}

static gpointer
interp_frame_get_ip (MonoInterpFrameHandle frame)
{
	InterpFrame *iframe = (InterpFrame*)frame;

	g_assert (iframe->imethod);
	/*
	 * The interpreter keeps the ip of a running frame in a local variable, and writes
	 * state.ip only where the frame stops. A running frame therefore has no ip to
	 * report, and the subtraction below turns a NULL into an address that looks correct.
	 */
	if (!iframe->state.ip)
		return NULL;

	/*
	 * For calls, state.ip points to the instruction following the call, so we need to subtract
	 * in order to get inside the call instruction range. Other instructions that set the IP for
	 * the rest of the runtime to see, like throws and sdb breakpoints, will need to account for
	 * this subtraction that we are doing here.
	 */
	return (gpointer)(iframe->state.ip - 1);
}

/*
 * interp_get_stopped_frame:
 *
 *   Return the frame a thread stopped at inside the interpreter, or NULL if it did
 * not stop there. A stack walk starts an interpreted frame iterator from this.
 *
 * The lmf is the head of the chain the walk itself will follow, and sp is the stack
 * pointer the walk starts from. Both must describe the thread being walked.
 */
static gpointer
interp_get_stopped_frame (const MonoJitTlsData *jit_tls, MonoLMF *lmf, gpointer sp)
{
	g_assert (jit_tls);
	ThreadContext *context = (ThreadContext*)jit_tls->interp_context;

	if (!context)
		return NULL;

	/* A safepoint names the frame exactly, so it wins. */
	if (context->safepoint_frame)
		return context->safepoint_frame;

	/* Read once. A sampling signal can arrive between two reads of this field. */
	InterpFrame *frame = context->current_frame;

	if (!frame)
		return NULL;

	/*
	 * The interpreter publishes a frame for as long as it runs one, so the frame alone
	 * does not say whether the loop is the innermost thing on this thread. Ask the LMF
	 * chain, and ask it by address. The kind will not do: native code called from the
	 * interpreter pushes its own plain MonoLMF over the interp-exit marker.
	 *
	 * The comparison is against the invocation's native anchor rather than against a
	 * frame, because a frame is not on this stack at all - only the invocation that
	 * runs it is. The stack grows down, so a callout from the loop - a jit call, an
	 * icall, the debugger tramp - lands below that anchor, and an entry the chain
	 * already held belongs to something outer and is above it.
	 *
	 * Only an lmf on the walked thread's stack can be compared this way. Under
	 * --interpreter the head of the chain is off that stack and far below it, and reads
	 * as a callout that did not happen. The stack pointer is the bound: nothing live on
	 * this stack sits below it.
	 *
	 * With no anchor there is no invocation to have stopped in, so say nothing: a walk
	 * that reports one frame less than the truth is the safe way to be wrong.
	 */
	if (!context->handle_mark_count)
		return NULL;

	gpointer anchor = context->handle_marks [context->handle_mark_count - 1].frame;

	if (lmf && (gsize)sp <= (gsize)lmf && (gsize)lmf < (gsize)anchor)
		return NULL;

	return frame;
}

/*
 * The order this frame was entered in, which is what a walk compares frames by.
 */
static gsize
interp_frame_ordinal (gpointer interp_frame)
{
	return ((InterpFrame*)interp_frame)->ordinal;
}

/*
 * interp_frame_iter_init:
 *
 *   Initialize an iterator for iterating through interpreted frames.
 */
static void
interp_frame_iter_init (MonoInterpStackIter *iter, gpointer interp_exit_data)
{
	StackIter *stack_iter = (StackIter*)iter;

	stack_iter->current = (InterpFrame*)interp_exit_data;
}

/*
 * interp_frame_iter_next:
 *
 *   Fill out FRAME with date for the next interpreter frame.
 */
static gboolean
interp_frame_iter_next (MonoInterpStackIter *iter, StackFrameInfo *frame)
{
	StackIter *stack_iter = (StackIter*)iter;
	InterpFrame *iframe = stack_iter->current;

	memset (frame, 0, sizeof (StackFrameInfo));
	/* pinvoke frames doesn't have imethod set */
	while (iframe && !(iframe->imethod && iframe->imethod->code && iframe->imethod->jinfo))
		iframe = iframe->parent;
	if (!iframe)
		return FALSE;

	MonoMethod *method = iframe->imethod->method;
	frame->domain = iframe->imethod->domain;
	frame->interp_frame = iframe;
	frame->method = method;
	frame->actual_method = method;
	if (method && ((method->flags & METHOD_ATTRIBUTE_PINVOKE_IMPL) || (method->iflags & (METHOD_IMPL_ATTRIBUTE_INTERNAL_CALL | METHOD_IMPL_ATTRIBUTE_RUNTIME)))) {
		frame->native_offset = -1;
		frame->type = FRAME_TYPE_MANAGED_TO_NATIVE;
	} else {
		frame->type = FRAME_TYPE_INTERP;
		/* The offset in the interpreter IR. It is -1 if the frame has no ip. */
		gpointer ip = interp_frame_get_ip (iframe);
		frame->native_offset = ip ? (int)((guint8*)ip - (guint8*)iframe->imethod->code) : -1;
		if (!method->wrapper_type || method->wrapper_type == MONO_WRAPPER_DYNAMIC_METHOD)
			frame->managed = TRUE;
	}
	frame->ji = iframe->imethod->jinfo;
	frame->frame_addr = iframe;

	stack_iter->current = iframe->parent;

	return TRUE;
}

static MonoJitInfo*
interp_find_jit_info (MonoDomain *domain, MonoMethod *method)
{
	InterpMethod* imethod;

	imethod = lookup_imethod (domain, method);
	if (imethod)
		return imethod->jinfo;
	else
		return NULL;
}

static void
interp_set_breakpoint (MonoJitInfo *jinfo, gpointer ip)
{
	guint16 *code = (guint16*)ip;
	g_assert (*code == MINT_SDB_SEQ_POINT);
	*code = MINT_SDB_BREAKPOINT;
}

static void
interp_clear_breakpoint (MonoJitInfo *jinfo, gpointer ip)
{
	guint16 *code = (guint16*)ip;
	g_assert (*code == MINT_SDB_BREAKPOINT);
	*code = MINT_SDB_SEQ_POINT;
}

static MonoJitInfo*
interp_frame_get_jit_info (MonoInterpFrameHandle frame)
{
	InterpFrame *iframe = (InterpFrame*)frame;

	g_assert (iframe->imethod);
	return iframe->imethod->jinfo;
}

static gpointer
interp_frame_get_arg (MonoInterpFrameHandle frame, int pos)
{
	InterpFrame *iframe = (InterpFrame*)frame;

	g_assert (iframe->imethod);

	return (char*)iframe->stack + get_arg_offset_fast (iframe->imethod, pos + iframe->imethod->hasthis);
}

static gpointer
interp_frame_get_local (MonoInterpFrameHandle frame, int pos)
{
	InterpFrame *iframe = (InterpFrame*)frame;

	g_assert (iframe->imethod);

	return frame_locals (iframe) + iframe->imethod->local_offsets [pos];
}

static gpointer
interp_frame_get_this (MonoInterpFrameHandle frame)
{
	InterpFrame *iframe = (InterpFrame*)frame;

	g_assert (iframe->imethod);
	g_assert (iframe->imethod->hasthis);
	return iframe->stack;
}

static MonoInterpFrameHandle
interp_frame_get_parent (MonoInterpFrameHandle frame)
{
	InterpFrame *iframe = (InterpFrame*)frame;

	return iframe->parent;
}

static void
interp_start_single_stepping (void)
{
	mono_interp_ss_enabled = TRUE;
}

static void
interp_stop_single_stepping (void)
{
	mono_interp_ss_enabled = FALSE;
}

/*
 * interp_mark_stack:
 *
 *   Mark the interpreter stack frames for a thread.
 *
 */
gpointer
mono_interp_entry_for_imethod (InterpMethod *imethod, MonoError *error)
{
	return entry_for_imethod (imethod, error);
}

gpointer
mono_interp_escaping_entry_for_imethod (InterpMethod *imethod, MonoError *error)
{
	return escaping_entry_for_imethod (imethod, error);
}

#ifndef ENABLE_NETCORE
MonoException *
mono_interp_ves_imethod (InterpFrame *frame, MonoMethod *method, MonoMethodSignature *sig, stackval *sp)
{
	return ves_imethod (frame, method, sig, sp);
}
#endif

void
mono_interp_init_arglist (InterpFrame *frame, MonoMethodSignature *sig, stackval *sp, char *arglist)
{
	init_arglist (frame, sig, sp, arglist);
}

static void
interp_mark_stack (gpointer thread_data, GcScanFunc func, gpointer gc_data, gboolean precise)
{
	MonoThreadInfo *info = (MonoThreadInfo*)thread_data;

	if (!mono_use_interpreter)
		return;
	if (precise)
		return;

	/*
	 * We explicitly mark the frames instead of registering the stack fragments as GC roots, so
	 * we have to process less data and avoid false pinning from data which is above 'pos'.
	 *
	 * The stack frame handling code uses compiler write barriers only, but the calling code
	 * in sgen-mono.c already did a mono_memory_barrier_process_wide () so we can
	 * process these data structures normally.
	 */
	MonoJitTlsData *jit_tls = (MonoJitTlsData *)info->tls [TLS_KEY_JIT_TLS];
	if (!jit_tls)
		return;

	ThreadContext *context = (ThreadContext*)jit_tls->interp_context;
	if (!context || !context->stack_start)
		return;

	// FIXME: Scan the whole area with 1 call
	for (gpointer *p = (gpointer*)context->stack_start; p < (gpointer*)context->stack_pointer; p++)
		func (p, gc_data);

	/*
	 * Frames the interpreter made for its own calls. The rest live on the native
	 * stack, which is scanned conservatively already.
	 */
	if (context->frame_stack_start) {
		for (gpointer *p = (gpointer*)context->frame_stack_start; p < (gpointer*)context->frame_stack_pointer; p++)
			func (p, gc_data);
	}

	FrameDataFragment *frag;
	for (frag = context->data_stack.first; frag; frag = frag->next) {
		// FIXME: Scan the whole area with 1 call
		for (gpointer *p = (gpointer*)&frag->data; p < (gpointer*)frag->pos; ++p)
			func (p, gc_data);
		if (frag == context->data_stack.current)
			break;
	}
}

#if COUNT_OPS

static int
opcode_count_comparer (const void * pa, const void * pb)
{
	long counta = opcode_counts [*(int*)pa];
	long countb = opcode_counts [*(int*)pb];

	if (counta < countb)
		return 1;
	else if (counta > countb)
		return -1;
	else
		return 0;
}

static void
interp_print_op_count (void)
{
	int ordered_ops [MINT_LASTOP];
	int i;
	long total_ops = 0;

	for (i = 0; i < MINT_LASTOP; i++) {
		ordered_ops [i] = i;
		total_ops += opcode_counts [i];
	}
	qsort (ordered_ops, MINT_LASTOP, sizeof (int), opcode_count_comparer);

	g_print ("total ops %ld\n", total_ops);
	for (i = 0; i < MINT_LASTOP; i++) {
		long count = opcode_counts [ordered_ops [i]];
		g_print ("%s : %ld (%.2lf%%)\n", mono_interp_opname (ordered_ops [i]), count, (double)count / total_ops * 100);
	}
}
#endif

#if PROFILE_INTERP

static InterpMethod **imethods;
static int num_methods;
const int opcount_threshold = 100000;

static void
interp_add_imethod (gpointer method)
{
	InterpMethod *imethod = (InterpMethod*) method;
	if (imethod->opcounts > opcount_threshold)
		imethods [num_methods++] = imethod;
}

static int
imethod_opcount_comparer (gconstpointer m1, gconstpointer m2)
{
	return (*(InterpMethod**)m2)->opcounts - (*(InterpMethod**)m1)->opcounts;
}

static void
interp_print_method_counts (void)
{
	MonoDomain *domain = mono_get_root_domain ();
	MonoJitDomainInfo *info = domain_jit_info (domain);

	mono_domain_jit_code_hash_lock (domain);
	imethods = (InterpMethod**) malloc (info->interp_code_hash.num_entries * sizeof (InterpMethod*));
	mono_internal_hash_table_apply (&info->interp_code_hash, interp_add_imethod);
	mono_domain_jit_code_hash_unlock (domain);

	qsort (imethods, num_methods, sizeof (InterpMethod*), imethod_opcount_comparer);

	printf ("Total executed opcodes %ld\n", total_executed_opcodes);
	long cumulative_executed_opcodes = 0;
	for (int i = 0; i < num_methods; i++) {
		cumulative_executed_opcodes += imethods [i]->opcounts;
		printf ("%d%% Opcounts %ld, calls %ld, Method %s, imethod ptr %p\n", (int)(cumulative_executed_opcodes * 100 / total_executed_opcodes), imethods [i]->opcounts, imethods [i]->calls, mono_method_full_name (imethods [i]->method, TRUE), imethods [i]);
	}
}
#endif

static void
interp_set_optimizations (guint32 opts)
{
	mono_interp_opt = opts;
}

static void
invalidate_transform (gpointer imethod_)
{
	InterpMethod *imethod = (InterpMethod *) imethod_;
	imethod->transformed = FALSE;
}

static void
copy_imethod_for_frame (MonoDomain *domain, InterpFrame *frame)
{
	InterpMethod *copy = (InterpMethod *) mono_domain_alloc0 (domain, sizeof (InterpMethod));
	memcpy (copy, frame->imethod, sizeof (InterpMethod));
	copy->next_jit_code_hash = NULL; /* we don't want that in our copy */
	frame->imethod = copy;
	/* Note: The copy will be around until the domain is unloading. Ideally we
	 * would reclaim its memory when the corresponding InterpFrame is popped.
	 */
}

static void
interp_metadata_update_init (MonoError *error)
{
	if ((mono_interp_opt & INTERP_OPT_INLINE) != 0)
		mono_error_set_execution_engine (error, "Interpreter inlining must be turned off for metadata updates");
}

#ifdef ENABLE_METADATA_UPDATE
static void
metadata_update_backup_frames (MonoDomain *domain, MonoThreadInfo *info, InterpFrame *frame)
{
	while (frame) {
		mono_trace (G_LOG_LEVEL_DEBUG, MONO_TRACE_METADATA_UPDATE, "threadinfo=%p, copy imethod for method=%s", info, mono_method_full_name (frame->imethod->method, 1));
		copy_imethod_for_frame (domain, frame);
		frame = frame->parent;
	}
}

static void
metadata_update_prepare_to_invalidate (MonoDomain *domain)
{
	/* (1) make a copy of imethod for every interpframe that is on the stack,
	 * so we do not invalidate currently running methods */

	FOREACH_THREAD_EXCLUDE (info, MONO_THREAD_INFO_FLAGS_NO_GC) {
		if (!info || !info->jit_data)
			continue;

		ThreadContext *context = (ThreadContext*)info->jit_data->interp_context;

		/* If the thread was in the interpreter and hit a safepoint
		 * opcode and suspended, backup the frames since the last lmf.
		 */
		if (context && context->safepoint_frame) {
			mono_trace (G_LOG_LEVEL_DEBUG, MONO_TRACE_METADATA_UPDATE, "threadinfo=%p, has safepoint frame %p", info, context->safepoint_frame);
			metadata_update_backup_frames (domain, info, context->safepoint_frame);
		}

		MonoLMF *lmf = info->jit_data->lmf;
		while (lmf) {
			if (((gsize) lmf->previous_lmf) & 2) {
				MonoLMFExt *ext = (MonoLMFExt *) lmf;
				if (ext->kind == MONO_LMFEXT_INTERP_EXIT || ext->kind == MONO_LMFEXT_INTERP_EXIT_WITH_CTX) {
					InterpFrame *frame = ext->interp_exit_data;
					metadata_update_backup_frames (domain, info, frame);
				}
			}
			lmf = (MonoLMF *)(((gsize) lmf->previous_lmf) & ~3);
		}
	} FOREACH_THREAD_END

	/* (2) invalidate all the registered imethods */
}
#endif


static void
interp_invalidate_transformed (MonoDomain *domain)
{
	gboolean need_stw_restart = FALSE;
#ifdef ENABLE_METADATA_UPDATE
	need_stw_restart = TRUE;
	mono_gc_stop_world ();
	metadata_update_prepare_to_invalidate (domain);
#endif
	MonoJitDomainInfo *info = domain_jit_info (domain);
	mono_domain_jit_code_hash_lock (domain);
	mono_internal_hash_table_apply (&info->interp_code_hash, invalidate_transform);
	mono_domain_jit_code_hash_unlock (domain);

	if (need_stw_restart)
		mono_gc_restart_world ();
}

static void
interp_cleanup (void)
{
#if COUNT_OPS
	interp_print_op_count ();
#endif
#if PROFILE_INTERP
	interp_print_method_counts ();
#endif
}

static void
register_interp_stats (void)
{
	mono_counters_init ();
	mono_counters_register ("Total transform time", MONO_COUNTER_INTERP | MONO_COUNTER_LONG | MONO_COUNTER_TIME, &mono_interp_stats.transform_time);
	mono_counters_register ("Methods transformed", MONO_COUNTER_INTERP | MONO_COUNTER_LONG, &mono_interp_stats.methods_transformed);
	mono_counters_register ("Line number table size", MONO_COUNTER_INTERP | MONO_COUNTER_INT, &mono_interp_stats.line_numbers_size);
	mono_counters_register ("Total cprop time", MONO_COUNTER_INTERP | MONO_COUNTER_LONG | MONO_COUNTER_TIME, &mono_interp_stats.cprop_time);
	mono_counters_register ("Total super instructions time", MONO_COUNTER_INTERP | MONO_COUNTER_LONG | MONO_COUNTER_TIME, &mono_interp_stats.super_instructions_time);
	mono_counters_register ("STLOC_NP count", MONO_COUNTER_INTERP | MONO_COUNTER_INT, &mono_interp_stats.stloc_nps);
	mono_counters_register ("MOVLOC count", MONO_COUNTER_INTERP | MONO_COUNTER_INT, &mono_interp_stats.movlocs);
	mono_counters_register ("Copy propagations", MONO_COUNTER_INTERP | MONO_COUNTER_INT, &mono_interp_stats.copy_propagations);
	mono_counters_register ("Added pop count", MONO_COUNTER_INTERP | MONO_COUNTER_INT, &mono_interp_stats.added_pop_count);
	mono_counters_register ("Constant folds", MONO_COUNTER_INTERP | MONO_COUNTER_INT, &mono_interp_stats.constant_folds);
	mono_counters_register ("Ldlocas removed", MONO_COUNTER_INTERP | MONO_COUNTER_INT, &mono_interp_stats.ldlocas_removed);
	mono_counters_register ("Super instructions", MONO_COUNTER_INTERP | MONO_COUNTER_INT, &mono_interp_stats.super_instructions);
	mono_counters_register ("Killed instructions", MONO_COUNTER_INTERP | MONO_COUNTER_INT, &mono_interp_stats.killed_instructions);
	mono_counters_register ("Emitted instructions", MONO_COUNTER_INTERP | MONO_COUNTER_INT, &mono_interp_stats.emitted_instructions);
	mono_counters_register ("Methods inlined", MONO_COUNTER_INTERP | MONO_COUNTER_INT, &mono_interp_stats.inlined_methods);
	mono_counters_register ("Inline failures", MONO_COUNTER_INTERP | MONO_COUNTER_INT, &mono_interp_stats.inline_failures);
}

/*
 * The engine itself lives in interp.cpp, and these five callbacks land there rather
 * than here. The table is still generated from the one list, so a callback added to
 * ee.h stays a compile error until something answers it.
 */
#define interp_entry_from_trampoline mono_interp_entry_from_ccontext
#define interp_entry_from_args       mono_interp_entry_from_args
#define interp_runtime_invoke        mono_interp_runtime_invoke
#define interp_run_finally           mono_interp_run_finally
#define interp_run_filter            mono_interp_run_filter

#undef MONO_EE_CALLBACK
#define MONO_EE_CALLBACK(ret, name, sig) interp_ ## name,

static const MonoEECallbacks mono_interp_callbacks = {
	MONO_EE_CALLBACKS
};

void
mono_ee_interp_init (const char *opts)
{
	g_assert (mono_ee_api_version () == MONO_EE_API_VERSION);
	g_assert (!interp_init_done);
	interp_init_done = TRUE;

	mono_native_tls_alloc (&thread_context_id, NULL);
	set_context (NULL);

	interp_parse_options (opts);
	/* Don't do any optimizations if running under debugger */
	if (mini_get_debug_options ()->mdb_optimizations)
		mono_interp_opt = 0;
	mono_interp_transform_init ();

	mini_install_interp_callbacks (&mono_interp_callbacks);

	register_interp_stats ();
}

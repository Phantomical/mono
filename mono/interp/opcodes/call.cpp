#include "config.h"

#include "glib.h"
#include "mintops.hpp"
#include "mono/llvm/runtime.h"
#include "mono/metadata/appdomain.h"
#include "mono/metadata/class-internals.h"
#include "mono/metadata/metadata.h"
#include "mono/metadata/object-forward.h"
#include "mono/metadata/object-internals.h"
#include "mono/metadata/object.h"
#include "mono/metadata/profiler.h"
#include "mono/metadata/tabledefs.h"
#include "mono/interp/runtime/internals.hpp"
#include "mono/interp/runtime/frame.hpp"
#include "mono/interp/runtime/lmf.hpp"
#include "mono/interp/runtime/method.hpp"
#include "mono/interp/runtime/stackval.hpp"
#include "mono/interp/runtime/entry.hpp"
#include "mono/interp/runtime/icall.hpp"
#include "mono/interp/runtime/pinvoke.hpp"
#include "mono/interp/interp.hpp"
#include "mono/interp/runtime/call.hpp"
#include "mono/interp/runtime/jit-call.hpp"
#include "mono/interp/transform/transform.hpp"
#include "mono/mini/llvm-runtime.h"
#include "mono/mini/llvmonly-runtime.h"
#include "mono/utils/atomic.h"
#include "mono/utils/mono-compiler.h"
#include "mono/utils/mono-error-internals.h"
#include <cstring>

namespace mono::interp {

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
		arm_tier_counter (imethod, mono_llvm_jit_tier0_calls (method));

	if (marshallable && (patched || mono_jit_method_is_compiled (imethod->domain, method)))
		code_type = IMETHOD_CODE_COMPILED;

	/*
	 * A compile that finished while the queries above were running has already
	 * written COMPILED, and that answer is the later one, so leave it alone.
	 */
	mono_atomic_cas_i32 (reinterpret_cast<gint32 *> (&imethod->code_type), code_type,
	                     IMETHOD_CODE_UNKNOWN);
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
			do_jit_call (reinterpret_cast<stackval *> ((locals + call_args_offset)), frame, cmethod,
			             error);
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
		auto child_frame = reinterpret_cast<InterpFrame *> (context->frame_stack_pointer);

		if (G_UNLIKELY (reinterpret_cast<guchar *> ((child_frame + 1))
		                > context->frame_stack_start + INTERP_FRAME_STACK_SIZE))
			THROW_EX (mono_get_exception_stack_overflow (), ip);

		context->frame_stack_pointer = reinterpret_cast<guchar *> ((child_frame + 1));
		/* reinit_frame () roots the callee's code in the frame, so the pointer has
		 * to cover it before that reference is stored. */
		mono_compiler_barrier ();

		reinit_frame (child_frame, context, frame, cmethod, locals + call_args_offset);
		frame = child_frame;
	}

	MonoException *ex;
	if (method_entry (context, frame, &ex)) {
		if (ex)
			THROW_EX (ex, NULL);
		EXCEPTION_CHECKPOINT;
	}

	// check for stack overflow
	if (G_UNLIKELY (reinterpret_cast<guchar *> (frame->stack) + cmethod->alloca_size
	                > context->stack_start + INTERP_STACK_SIZE - INTERP_STACK_RESERVE))
		THROW_EX (mono_get_exception_stack_overflow (), NULL);

	context->stack_pointer = reinterpret_cast<guchar *> (frame->stack) + cmethod->alloca_size;
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
	if (G_UNLIKELY (reinterpret_cast<guchar *> (frame->stack) + cmethod->alloca_size
	                > context->stack_start + INTERP_STACK_SIZE))
		return &exec_call;

	if (G_UNLIKELY (frame->imethod->prof_flags & MONO_PROFILER_CALL_INSTRUMENTATION_TAIL_CALL))
		MONO_PROFILER_RAISE (method_tail_call, (frame->imethod->method, cmethod->method));

	guchar *new_top = reinterpret_cast<guchar *> (frame->stack) + cmethod->alloca_size;

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
	auto csignature = static_cast<MonoMethodSignature *> (frame->imethod->data_items[ip[3]]);
	guint16 param_count = csignature->param_count;
	call_args_offset = ip[1];
	auto del = LOCAL_VAR (call_args_offset, MonoDelegate *);
	bool is_multicast = del->method == nullptr;
	auto del_imethod = static_cast<InterpMethod *> (del->interp_invoke_impl);

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
			del_imethod = static_cast<InterpMethod *> (del->interp_method);

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
	cmethod = static_cast<InterpMethod *> (frame->imethod->data_items[ip[4]]);

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

	calli_signature = static_cast<MonoMethodSignature *> (frame->imethod->data_items[ip[3]]);
	call_args_offset = ip[1];

	MONO_INTERP_OP_ADVANCE ();
	return &exec_calli;
}

MONO_INTERP_OP_IMPL (MINT_CALLI_IMETHOD)
{
	cmethod = LOCAL_VAR (ip[2], InterpMethod *);
	calli_signature = static_cast<MonoMethodSignature *> (frame->imethod->data_items[ip[3]]);
	call_args_offset = ip[1];

	MONO_INTERP_OP_ADVANCE ();
	return &exec_calli;
}

MONO_INTERP_OP_IMPL (MINT_CALLI_NAT_FAST)
{
	auto csignature = static_cast<MonoMethodSignature *> (frame->imethod->data_items[ip[2]]);
	guint16 opcode = ip[3];
	bool save_last_error = ip[4];
	auto args = &LOCAL_VAR (ip[1], stackval);
	gpointer target_ip = args[csignature->param_count].data.p;

	// for calls, have ip pointing at the start of next instruction
	frame->state.ip = ip + 5;

	do_icall_wrapper (frame, csignature, opcode, args, target_ip, save_last_error);
	EXCEPTION_CHECKPOINT_GC_UNSAFE;
	CHECK_RESUME_STATE (context);

	MONO_INTERP_OP_ADVANCE ();
	MONO_INTERP_DISPATCH ();
}

MONO_INTERP_OP_IMPL (MINT_CALLI_NAT_DYNAMIC)
{
	auto csignature = static_cast<MonoMethodSignature *> (frame->imethod->data_items[ip[3]]);

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
	auto csignature = static_cast<MonoMethodSignature *> (frame->imethod->data_items[ip[3]]);
	auto imethod = static_cast<InterpMethod *> (frame->imethod->data_items[ip[4]]);
	auto code = LOCAL_VAR (ip[2], guchar *);
	bool save_last_error = ip[5];
	auto cache = static_cast<gpointer *> (&frame->imethod->data_items[ip[6]]);

	/* for calls, have ip pointing at the start of next instruction */
	frame->state.ip = ip + 7;
	ves_pinvoke_method (imethod, csignature, (MonoFuncV) code, context, frame,
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

	entry = static_cast<InterpVTableEntry *> (
		mono_mem_manager_alloc_nolock (memory_manager, sizeof (InterpVTableEntry)));
	entry->imethod = imethod;
	entry->target_imethod = target_imethod;
	ret = g_slist_append_mempool (memory_manager->mp, list, entry);

	return ret;
}

static InterpMethod *
get_target_imethod (GSList *list, InterpMethod *imethod)
{
	while (list != NULL) {
		InterpVTableEntry *entry = static_cast<InterpVTableEntry *> (list->data);
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
		return reinterpret_cast<gpointer *> (vtable);
}

static gpointer *
alloc_method_table (MonoVTable *vtable, int offset)
{
	gpointer *table;

	if (offset >= 0) {
		table = static_cast<gpointer *> (
			m_class_alloc0 (vtable->domain, vtable->klass,
		                    m_class_get_vtable_size (vtable->klass) * sizeof (gpointer)));
		vtable->interp_vtable = table;
	} else {
		table = reinterpret_cast<gpointer *> (vtable);
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
		return reinterpret_cast<InterpMethod *> ((gsize) table[offset] & ~0x1);
	} else {
		/* Virtual generic or interface call. Multiple methods in slot */
		InterpMethod *target_imethod =
			get_target_imethod (static_cast<GSList *> (table[offset]), imethod);

		if (!target_imethod) {
			target_imethod = get_virtual_method (imethod, vtable);
			mono_mem_manager_lock (memory_manager);
			if (!get_target_imethod (static_cast<GSList *> (table[offset]), imethod))
				table[offset] = append_imethod (
					memory_manager, static_cast<GSList *> (table[offset]), imethod, target_imethod);
			mono_mem_manager_unlock (memory_manager);
		}
		return target_imethod;
	}
}

MONO_INTERP_OP_IMPL (MINT_CALLVIRT_FAST)
{
	cmethod = static_cast<InterpMethod *> (frame->imethod->data_items[ip[2]]);
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
	cmethod = static_cast<InterpMethod *> (frame->imethod->data_items[ip[2]]);
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
	cmethod = static_cast<InterpMethod *> (frame->imethod->data_items[ip[2]]);
	call_args_offset = ip[1];

	MONO_INTERP_OP_ADVANCE ();
	return &exec_call;
}

MONO_INTERP_OP_IMPL (MINT_CALLVIRT)
{
	// FIXME CALLVIRT opcodes are not used on netcore. We should kill them.
	cmethod = static_cast<InterpMethod *> (frame->imethod->data_items[ip[2]]);
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
	cmethod = static_cast<InterpMethod *> (frame->imethod->data_items[ip[2]]);
	call_args_offset = ip[1];
	tail_args_size = ip[3];

	MONO_INTERP_OP_ADVANCE ();
	return &exec_tailcall;
}

MONO_INTERP_OP_IMPL (MINT_CALL)
{
	cmethod = static_cast<InterpMethod *> (frame->imethod->data_items[ip[2]]);
	call_args_offset = ip[1];

	MONO_INTERP_OP_ADVANCE ();
	return &exec_call;
}

// The transform picks this for a method the runtime implements itself rather than
// in IL.

MONO_INTERP_OP_IMPL (MINT_LDFTN)
{
	error_init_reuse (error);
	LOCAL_VAR (ip[1], gpointer) =
		entry_for_imethod (static_cast<InterpMethod *> (frame->imethod->data_items[ip[2]]), error);
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

	LOCAL_VAR (ip[1], gpointer) = escaping_entry_for_imethod (m, error);
	mono_error_assert_ok (error);

	MONO_INTERP_OP_ADVANCE ();
	MONO_INTERP_DISPATCH ();
}

MONO_INTERP_OP_IMPL (MINT_LDVIRTFTN)
{
	auto m = static_cast<InterpMethod *> (frame->imethod->data_items[ip[3]]);
	auto o = LOCAL_VAR (ip[2], MonoObject *);
	NULL_CHECK (o);

	error_init_reuse (error);
	LOCAL_VAR (ip[1], gpointer) = entry_for_imethod (get_virtual_method (m, o->vtable), error);
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

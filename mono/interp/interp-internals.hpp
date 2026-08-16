#ifndef __MONO_INTERP_INTERP_INTERNALS_HPP__
#define __MONO_INTERP_INTERP_INTERNALS_HPP__

#include "interp.hpp"
#include "mono/llvm/runtime.h"
#include "mono/metadata/appdomain.h"
#include "mono/mini/mini.h"
#include "mono/utils/atomic.h"

namespace mono::interp {
namespace {

inline bool
isinst (MonoObject *object, MonoClass *klass, MonoError *error)
{
	MonoClass *obj_class = mono_object_class (object);

	// mono_class_is_assignable_from_checked can't handle remoting casts
	if (G_UNLIKELY (mono_class_is_transparent_proxy (obj_class)))
		return mono_object_isinst_checked (object, klass, error);

	gboolean isinst = false;
	mono_class_is_assignable_from_checked (klass, obj_class, &isinst, error);
	return isinst;
}

} // namespace

/*
 * Moving a value between the interpreter stack and memory laid out the way the
 * runtime lays out a field, an array element or a pinvoke argument. pinvoke
 * picks the native layout of a value type over the managed one.
 *
 * Both return the size the value takes on the interpreter stack, which is a
 * whole number of slots and so is not the size in memory.
 */

inline int
stackval_from_data (MonoType *type, stackval *result, const void *data, gboolean pinvoke)
{
	type = mini_native_type_replace_type (type);
	if (type->byref) {
		result->data.p = *(gpointer *) data;
		return MINT_STACK_SLOT_SIZE;
	}
	switch (type->type) {
	case MONO_TYPE_VOID:
		return 0;
	case MONO_TYPE_I1:
		result->data.i = *(gint8 *) data;
		return MINT_STACK_SLOT_SIZE;
	case MONO_TYPE_U1:
	case MONO_TYPE_BOOLEAN:
		result->data.i = *(guint8 *) data;
		return MINT_STACK_SLOT_SIZE;
	case MONO_TYPE_I2:
		result->data.i = *(gint16 *) data;
		return MINT_STACK_SLOT_SIZE;
	case MONO_TYPE_U2:
	case MONO_TYPE_CHAR:
		result->data.i = *(guint16 *) data;
		return MINT_STACK_SLOT_SIZE;
	case MONO_TYPE_I4:
		result->data.i = *(gint32 *) data;
		return MINT_STACK_SLOT_SIZE;
	case MONO_TYPE_U:
	case MONO_TYPE_I:
		result->data.nati = *(mono_i *) data;
		return MINT_STACK_SLOT_SIZE;
	case MONO_TYPE_PTR:
	case MONO_TYPE_FNPTR:
		result->data.p = *(gpointer *) data;
		return MINT_STACK_SLOT_SIZE;
	case MONO_TYPE_U4:
		result->data.i = *(guint32 *) data;
		return MINT_STACK_SLOT_SIZE;
	case MONO_TYPE_R4:
		/* memmove handles unaligned case */
		std::memmove (&result->data.f_r4, data, sizeof (float));
		return MINT_STACK_SLOT_SIZE;
	case MONO_TYPE_I8:
	case MONO_TYPE_U8:
		std::memmove (&result->data.l, data, sizeof (gint64));
		return MINT_STACK_SLOT_SIZE;
	case MONO_TYPE_R8:
		std::memmove (&result->data.f, data, sizeof (double));
		return MINT_STACK_SLOT_SIZE;
	case MONO_TYPE_STRING:
	case MONO_TYPE_SZARRAY:
	case MONO_TYPE_CLASS:
	case MONO_TYPE_OBJECT:
	case MONO_TYPE_ARRAY:
		result->data.p = *(gpointer *) data;
		return MINT_STACK_SLOT_SIZE;
	case MONO_TYPE_TYPEDBYREF:
		/*
		 * The transform pushes a TypedReference as a value of this size, so a plain
		 * copy is what the interpreter stack expects on either side of the boundary.
		 */
		std::memcpy (result, data, sizeof (MonoTypedRef));
		return ALIGN_TO (sizeof (MonoTypedRef), MINT_STACK_SLOT_SIZE);
	case MONO_TYPE_VALUETYPE:
		if (m_class_is_enumtype (type->data.klass)) {
			return stackval_from_data (mono_class_enum_basetype_internal (type->data.klass), result,
			                           data, pinvoke);
		} else {
			int size;
			if (pinvoke)
				size = mono_class_native_size (type->data.klass, NULL);
			else
				size = mono_class_value_size (type->data.klass, NULL);
			std::memcpy (result, data, size);
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
			std::memcpy (result, data, size);
			return ALIGN_TO (size, MINT_STACK_SLOT_SIZE);
		}
		return stackval_from_data (
			m_class_get_byval_arg (type->data.generic_class->container_class), result, data,
			pinvoke);
	}
	default:
		g_error ("got type 0x%02x", type->type);
	}
}

inline int
stackval_to_data (MonoType *type, stackval *val, void *data, gboolean pinvoke)
{
	type = mini_native_type_replace_type (type);
	if (type->byref) {
		gpointer *p = (gpointer *) data;
		*p = val->data.p;
		return MINT_STACK_SLOT_SIZE;
	}
	switch (type->type) {
	case MONO_TYPE_I1:
	case MONO_TYPE_U1: {
		guint8 *p = (guint8 *) data;
		*p = val->data.i;
		return MINT_STACK_SLOT_SIZE;
	}
	case MONO_TYPE_BOOLEAN: {
		guint8 *p = (guint8 *) data;
		*p = (val->data.i != 0);
		return MINT_STACK_SLOT_SIZE;
	}
	case MONO_TYPE_I2:
	case MONO_TYPE_U2:
	case MONO_TYPE_CHAR: {
		guint16 *p = (guint16 *) data;
		*p = val->data.i;
		return MINT_STACK_SLOT_SIZE;
	}
	case MONO_TYPE_I: {
		mono_i *p = (mono_i *) data;
		/* In theory the value used by stloc should match the local var type
	 	   but in practice it sometimes doesn't (a int32 gets dup'd and stloc'd into
		   a native int - both by csc and mcs). Not sure what to do about sign extension
		   as it is outside the spec... doing the obvious */
		*p = (mono_i) val->data.nati;
		return MINT_STACK_SLOT_SIZE;
	}
	case MONO_TYPE_U: {
		mono_u *p = (mono_u *) data;
		/* see above. */
		*p = (mono_u) val->data.nati;
		return MINT_STACK_SLOT_SIZE;
	}
	case MONO_TYPE_I4:
	case MONO_TYPE_U4: {
		gint32 *p = (gint32 *) data;
		*p = val->data.i;
		return MINT_STACK_SLOT_SIZE;
	}
	case MONO_TYPE_I8:
	case MONO_TYPE_U8: {
		std::memmove (data, &val->data.l, sizeof (gint64));
		return MINT_STACK_SLOT_SIZE;
	}
	case MONO_TYPE_R4: {
		/* memmove handles unaligned case */
		std::memmove (data, &val->data.f_r4, sizeof (float));
		return MINT_STACK_SLOT_SIZE;
	}
	case MONO_TYPE_R8: {
		std::memmove (data, &val->data.f, sizeof (double));
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
		std::memcpy (data, val, sizeof (MonoTypedRef));
		return ALIGN_TO (sizeof (MonoTypedRef), MINT_STACK_SLOT_SIZE);
	case MONO_TYPE_VALUETYPE:
		if (m_class_is_enumtype (type->data.klass)) {
			return stackval_to_data (mono_class_enum_basetype_internal (type->data.klass), val,
			                         data, pinvoke);
		} else {
			int size;
			if (pinvoke) {
				size = mono_class_native_size (type->data.klass, NULL);
				std::memcpy (data, val, size);
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
				std::memcpy (data, val, size);
			} else {
				size = mono_class_value_size (klass, NULL);
				mono_value_copy_internal (data, val, klass);
			}
			return ALIGN_TO (size, MINT_STACK_SLOT_SIZE);
		}
		return stackval_to_data (m_class_get_byval_arg (type->data.generic_class->container_class),
		                         val, data, pinvoke);
	}
	default:
		g_error ("got type %x", type->type);
	}
}

inline gint32
enum_hasflag (stackval *sp1, stackval *sp2, MonoClass *klass)
{
	guint64 a_val = 0, b_val = 0;

	stackval_to_data (m_class_get_byval_arg (klass), sp1, &a_val, FALSE);
	stackval_to_data (m_class_get_byval_arg (klass), sp2, &b_val, FALSE);
	return (a_val & b_val) == b_val;
}

inline InterpMethod *
lookup_method_pointer (MonoDomain *domain, gpointer addr)
{
	MonoJitDomainInfo *info = domain_jit_info (domain);
	InterpMethod *res = NULL;

	mono_domain_lock (domain);
	if (info->interp_method_pointer_hash)
		res = (InterpMethod *) g_hash_table_lookup (info->interp_method_pointer_hash, addr);
	mono_domain_unlock (domain);

	return res;
}

/// Get the InterpMethod* that corresponds to entry point address addr.
///
/// \returns the method, if known, and null otherwise.
inline InterpMethod *
imethod_for_entry (MonoDomain *domain, gpointer addr, MonoError *error)
{
	if (InterpMethod *imethod = lookup_method_pointer (domain, addr))
		return imethod;

	MonoJitInfo *ji = mono_jit_info_table_find_internal (
		domain, mono_get_addr_from_ftnptr (MINI_FTNPTR_TO_ADDR (addr)), TRUE, TRUE);

	if (!ji)
		return nullptr;

	MonoMethod *method;
	if (ji->is_trampoline) {
		method = ji->d.tramp_info->method;

		// some trampolines do not have an associated MonoMethod*
		if (!method)
			return nullptr;
	} else {
		method = mono_jit_info_get_method (ji);
	}

	return mono_interp_get_imethod (domain, method, error);
}

/*
 * interp_push_lmf:
 *
 * Push an LMF frame on the LMF stack
 * to mark the transition to native code.
 * This is needed for the native code to
 * be able to do stack walks.
 */
inline void
interp_push_lmf (MonoLMFExt *ext, InterpFrame *frame)
{
	/*
	 * Only these two fields and lmf.previous_lmf, which mono_push_lmf ()
	 * writes, are ever read back: the rest of the MonoLMF is documented as
	 * invalid once its second lowest bit marks the entry as an ext, and ctx
	 * belongs to the WITH_CTX kind. Zeroing the whole thing instead would
	 * clear a MonoContext, which is most of the ~450 bytes here and costs
	 * around a fifth of a jit call.
	 */
	ext->kind = MONO_LMFEXT_INTERP_EXIT;
	ext->interp_exit_data = frame;

	mono_push_lmf (ext);
}

inline void
interp_pop_lmf (MonoLMFExt *ext)
{
	mono_pop_lmf (&ext->lmf);
}

// Initialize the tiering counter, if it hasn't already been initialized.
inline void
interp_arm_tier_counter (gpointer imethod_ptr, gint32 calls)
{
	InterpMethod *imethod = (InterpMethod *) imethod_ptr;

	mono_atomic_cas_i32 (&imethod->tier_counter, calls > 0 ? calls : -1, 0);
}

// Check whether we should start a background compilation of this method to tier1.
inline MONO_NEVER_INLINE void
interp_check_call_promotion (InterpMethod *imethod)
{
	gint32 left;

	left = mono_atomic_dec_i32 (&imethod->tier_counter);
	if (left != 0)
		return;

	/*
	 * A refused request is the counter spent for nothing, and nothing else
	 * arms it again: interp_arm_tier_counter () is reached once per method,
	 * from whichever of resolve_code_type () and the backend's entry sees it
	 * first. Arming it here is what makes the loss cost this method another
	 * threshold of calls rather than the rest of the process.
	 */
	if (!mono_llvm_jit_request_promotion (imethod->method, imethod->domain))
		interp_arm_tier_counter (imethod, mono_llvm_jit_tier0_calls (imethod->method));
}

/*
 * Transforms the method frame is about to run, and returns what that threw or null.
 *
 * A frame whose imethod is not transformed yet is incomplete, so the transform runs
 * under the parent instead. A root frame has no parent and no walk to satisfy.
 */
inline MonoException *
do_transform_method (InterpFrame *frame, ThreadContext *context)
{
	MonoLMFExt ext;
	gboolean push_lmf = frame->parent != NULL;
	ERROR_DECL (error);

#if DEBUG_INTERP
	char *mn = mono_method_full_name (frame->imethod->method, TRUE);
	g_print ("(%p) Transforming %s\n", mono_thread_internal_current (), mn);
	g_free (mn);
#endif

	if (push_lmf)
		interp_push_lmf (&ext, frame->parent);

	mono_interp_transform_method (frame->imethod, context, error);

	if (push_lmf)
		interp_pop_lmf (&ext);

	return mono_error_convert_to_exception (error);
}

/*
 * Makes frame ready to run, and returns whether it took the slow path. out_ex holds
 * what the transform threw, and is null when nothing did.
 *
 * A caller that took the slow path has to check out_ex and run an interruption
 * checkpoint. Both are rare, which is what keeps them out of the fast path.
 */
inline MONO_ALWAYS_INLINE gboolean
method_entry (ThreadContext *context, InterpFrame *frame,
#if DEBUG_INTERP
              int *out_tracing,
#endif
              MonoException **out_ex)
{
	gboolean slow = FALSE;

#if DEBUG_INTERP
	debug_enter (frame, out_tracing);
#endif
#if PROFILE_INTERP
	frame->imethod->calls++;
#endif

	*out_ex = NULL;
	if (!G_UNLIKELY (frame->imethod->transformed)) {
		slow = TRUE;
		MonoException *ex = do_transform_method (frame, context);
		if (ex) {
			*out_ex = ex;
			/*
			 * Initialize the stack base pointer here, in the uncommon branch, so we don't
			 * need to check for it everytime when exitting a frame.
			 */
			frame->stack = (stackval *) context->stack_pointer;
			return slow;
		}
	}

	return slow;
}

/// Runs a class initializer, and returns what it threw or null.
inline MONO_NEVER_INLINE MonoException *
init_vtable (MonoVTable *vtable)
{
	ERROR_DECL (error);

	mono_runtime_class_init_full (vtable, error);
	if (!is_ok (error))
		return mono_error_convert_to_exception (error);
	return nullptr;
}

} // namespace mono::interp

// The class init runs here, not at transform time, so that cctors run in
// program order.
#define INIT_VTABLE(vtable)                        \
	do {                                           \
		MonoVTable *__vtable = (vtable);           \
		if (G_UNLIKELY (!__vtable->initialized)) { \
			if (auto ex = init_vtable (__vtable))  \
				THROW_EX (ex, ip);                 \
		}                                          \
	} while (0)

#define STACK_ADD_BYTES(sp, bytes) \
	((stackval *) ((char *) (sp) + ALIGN_TO (bytes, MINT_STACK_SLOT_SIZE)))
#define STACK_SUB_BYTES(sp, bytes) \
	((stackval *) ((char *) (sp) - ALIGN_TO (bytes, MINT_STACK_SLOT_SIZE)))

#endif

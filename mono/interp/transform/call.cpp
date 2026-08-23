/**
 * \file
 * \brief Turning a call site into interpreter bytecode.
 *
 * What shape the call takes - inlined, a tail call, a direct call, a virtual
 * dispatch, or a pinvoke - and the checks that decide between them.
 */

#include "config.h"

#include <algorithm>

#include <mono/metadata/mono-endian.h>

#include <mono/metadata/class-internals.h>
#include <mono/metadata/debug-helpers.h>
#include <mono/metadata/marshal.h>
#include <mono/metadata/mono-basic-block.h>
#include <mono/metadata/reflection-internals.h>
#include <mono/metadata/tabledefs.h>
#include <mono/utils/unlocked.h>

#include <mono/mini/mini.h>
#include <mono/mini/domain-method.hpp>
#include <mono/mini/mini-runtime.h>

#include "mintops.hpp"
#include "runtime/internals.hpp"
#include "runtime/sharing.hpp"
#include "interp.h"
#include "transform.hpp"
#include "internal.hpp"

#include "mono/llvm/runtime.h"

namespace mono::interp {

/* Same as mono jit */
#define INLINE_LENGTH_LIMIT 20
#define INLINE_DEPTH_LIMIT 10

gboolean
TransformData::interp_method_check_inlining (MonoMethod *method, MonoMethodSignature *csignature)
{
	MonoMethodHeaderSummary header;

	if (method->flags & METHOD_ATTRIBUTE_REQSECOBJ)
		/* Used to mark methods containing StackCrawlMark locals */
		return FALSE;

	if (csignature->call_convention == MONO_CALL_VARARG)
		return FALSE;

	if (!mono_method_get_header_summary (method, &header))
		return FALSE;

	/*runtime, icall and pinvoke are checked by summary call*/
	if ((method->iflags & METHOD_IMPL_ATTRIBUTE_NOINLINING)
	    || (method->iflags & METHOD_IMPL_ATTRIBUTE_SYNCHRONIZED)
	    || (mono_class_is_marshalbyref (method->klass)) || header.has_clauses)
		return FALSE;

	if (inline_depth > INLINE_DEPTH_LIMIT)
		return FALSE;

	if (header.code_size >= INLINE_LENGTH_LIMIT
	    && !(method->iflags & METHOD_IMPL_ATTRIBUTE_AGGRESSIVE_INLINING))
		return FALSE;

	if (mono_class_needs_cctor_run (method->klass, NULL)) {
		MonoVTable *vtable;
		ERROR_DECL (error);
		if (!m_class_get_runtime_info (method->klass))
			/* No vtable created yet */
			return FALSE;
		vtable = mono_class_vtable_checked (rtm->domain, method->klass, error);
		if (!is_ok (error)) {
			mono_interp_error_cleanup (error);
			return FALSE;
		}
		if (!vtable->initialized)
			return FALSE;
	}

	/* We currently access at runtime the wrapper data */
	if (method->wrapper_type != MONO_WRAPPER_NONE)
		return FALSE;

	if (prof_coverage)
		return FALSE;

	if (std::find (dont_inline.begin (), dont_inline.end (), method) != dont_inline.end ())
		return FALSE;

	/*
	 * An inlined body never gets a transform of its own, so this is the last
	 * point its IL can be put through the verifier. Declining the inline is
	 * all that is needed: the call stays a real one, and the callee is
	 * verified when it is transformed.
	 */
	{
		ERROR_DECL (verify_error);

		if (!mono_llvm_jit_verify_method (method, verify_error)) {
			mono_interp_error_cleanup (verify_error);
			return FALSE;
		}
	}

	return TRUE;
}

gboolean
TransformData::interp_inline_method (MonoMethod *target_method, MonoMethodHeader *header,
                                     MonoError *error)
{
	const unsigned char *prev_ip, *prev_il_code, *prev_in_start;
	int *prev_in_offsets;
	gboolean ret;
	unsigned int prev_max_stack_height, prev_locals_size;
	size_t prev_n_data_items;
	int i;
	int prev_sp_offset;
	MonoGenericContext *generic_context = NULL;
	StackInfo *prev_param_area;
	InterpBasicBlock **prev_offset_to_bb;
	InterpBasicBlock *prev_cbb, *prev_entry_bb;
	MonoMethod *prev_inlined_method;
	MonoMethodSignature *csignature = mono_method_signature_internal (target_method);
	int nargs = csignature->param_count + !!csignature->hasthis;
	InterpInst *prev_last_ins;

	if (csignature->is_inflated)
		generic_context = mono_method_get_context (target_method);
	else {
		MonoGenericContainer *generic_container = mono_method_get_generic_container (target_method);
		if (generic_container)
			generic_context = &generic_container->context;
	}

	prev_ip = ip;
	prev_il_code = il_code;
	prev_in_start = in_start;
	prev_sp_offset = sp - stack;
	prev_inlined_method = inlined_method;
	prev_last_ins = last_ins;
	prev_offset_to_bb = offset_to_bb;
	prev_cbb = cbb;
	prev_entry_bb = entry_bb;
	inlined_method = target_method;

	prev_max_stack_height = max_stack_height;
	prev_locals_size = locals.size ();

	prev_n_data_items = data_items.size ();
	prev_in_offsets = in_offsets;
	in_offsets = (int *) g_malloc0 ((header->code_size + 1) * sizeof (int));

	/* Inlining pops the arguments, restore the stack */
	prev_param_area = (StackInfo *) g_malloc (nargs * sizeof (StackInfo));
	memcpy (prev_param_area, &sp[-nargs], nargs * sizeof (StackInfo));

	int const prev_code_size = code_size;
	code_size = header->code_size;

	if (verbose_level)
		g_print ("Inline start method %s.%s\n", m_class_get_name (target_method->klass),
		         target_method->name);

	inline_depth++;
	ret = generate_code (target_method, header, generic_context, error);
	inline_depth--;

	if (!ret) {
		if (!is_ok (error))
			mono_interp_error_cleanup (error);

		if (verbose_level)
			g_print ("Inline aborted method %s.%s\n", m_class_get_name (target_method->klass),
			         target_method->name);
		max_stack_height = prev_max_stack_height;
		locals.resize (prev_locals_size);

		/* Remove any newly added items */
		for (size_t item = prev_n_data_items; item < data_items.size (); item++)
			data_hash.erase (data_items[item]);
		data_items.resize (prev_n_data_items);
		sp = stack + prev_sp_offset;
		memcpy (&sp[-nargs], prev_param_area, nargs * sizeof (StackInfo));
		last_ins = prev_last_ins;
		cbb = prev_cbb;
		if (last_ins)
			last_ins->next = NULL;
		UnlockedIncrement (&mono_interp_stats.inline_failures);
	} else {
		if (verbose_level)
			g_print ("Inline end method %s.%s\n", m_class_get_name (target_method->klass),
			         target_method->name);
		UnlockedIncrement (&mono_interp_stats.inlined_methods);

		interp_link_bblocks (prev_cbb, entry_bb);
		prev_cbb->next_bb = entry_bb;
	}

	ip = prev_ip;
	in_start = prev_in_start;
	il_code = prev_il_code;
	inlined_method = prev_inlined_method;
	offset_to_bb = prev_offset_to_bb;
	code_size = prev_code_size;
	entry_bb = prev_entry_bb;

	g_free (in_offsets);
	in_offsets = prev_in_offsets;

	g_free (prev_param_area);
	return ret;
}

void
TransformData::interp_constrained_box (MonoDomain *domain, MonoClass *constrained_class,
                                       MonoMethodSignature *csignature, MonoError *error)
{
	MintType mt = mint_type (m_class_get_byval_arg (constrained_class));
	StackInfo *sp = this->sp - 1 - csignature->param_count;
	bool from_context = sharing && depends_on_context (constrained_class);
	// What a shared body fetches, which each arm below names for itself: the
	// class where a nullable box reads one, the vtable where a plain box does.
	int fetched = -1;

	if (mono_class_is_nullable (constrained_class)) {
		g_assert (mt == MintType::VT);

		if (from_context) {
			fetched = emit_rgctx_fetch (MONO_RGCTX_INFO_KLASS, constrained_class);

			if (sharing_refusal != nullptr)
				return;

			interp_add_ins (MINT_BOX_NULLABLE_PTR_DYN);
		} else {
			interp_add_ins (MINT_BOX_NULLABLE_PTR);
			last_ins->data[0] = get_data_item_index (constrained_class);
		}
	} else if (from_context) {
		fetched = emit_rgctx_fetch (MONO_RGCTX_INFO_VTABLE, constrained_class);

		if (sharing_refusal != nullptr)
			return;

		interp_add_ins (MINT_BOX_PTR_DYN);
	} else {
		MonoVTable *vtable = mono_class_vtable_checked (domain, constrained_class, error);
		return_if_nok (error);

		interp_add_ins (MINT_BOX_PTR);
		last_ins->data[0] = get_data_item_index (vtable);
	}

	if (fetched >= 0)
		interp_ins_set_sregs2 (last_ins, sp->local, fetched);
	else
		interp_ins_set_sreg (last_ins, sp->local);
	set_simple_type_and_local (sp, StackType::O);
	interp_ins_set_dreg (last_ins, sp->local);
}

MonoMethod *
interp_get_method (MonoMethod *method, guint32 token, MonoImage *image,
                   MonoGenericContext *generic_context, MonoError *error)
{
	if (method->wrapper_type == MONO_WRAPPER_NONE)
		return mono_get_method_checked (image, token, NULL, generic_context, error);
	else
		return (MonoMethod *) mono_method_get_wrapper_data (method, token);
}

/**
 * Whether an argument can travel through a call that hands the caller's frame away.
 *
 * Both ends of a tail call promise the callee touches nothing of the frame it
 * replaces. Anything that can point into it has to stay behind. The signature
 * covers the declared cases. The stack type covers a managed pointer travelling
 * under a signature that no longer says so, which is unverifiable but cheap to
 * spot here.
 *
 * A native int is not refused: it is how the frame-address checks in the
 * tailcall corpus travel. The value is compared rather than dereferenced, and
 * the compiled engine passes it through as well.
 */
static gboolean
interp_tail_call_arg_is_safe (MonoType *param, StackType stack_type)
{
	if (param->byref)
		return FALSE;

	if (param->type == MONO_TYPE_PTR || param->type == MONO_TYPE_FNPTR)
		return FALSE;

	return stack_type != StackType::MP;
}

/**
 * Whether the two returns can be folded into one: the tail callee writes its
 * result where the method being replaced would have written its own. That slot
 * is one the caller's caller sized from the signature it called.
 */
static gboolean
interp_tail_call_returns_match (MonoType *caller_ret, MonoType *callee_ret)
{
	MintType mt;

	caller_ret = mini_type_get_underlying_type (caller_ret);
	callee_ret = mini_type_get_underlying_type (callee_ret);

	/* mint_type () has no answer for void, and a method returning nothing can only
	 * hand its return over to another that returns nothing. */
	if (caller_ret->type == MONO_TYPE_VOID || callee_ret->type == MONO_TYPE_VOID)
		return caller_ret->type == callee_ret->type;

	mt = mint_type (caller_ret);
	if (mt != mint_type (callee_ret))
		return FALSE;

	/* MINT_RET_VT copies the callee's own size into that slot, so it has to be the
	 * size the slot was made for. Everything else travels as a whole stackval, which
	 * makes the narrow integer types interchangeable. */
	if (mt == MintType::VT)
		return mono_class_value_size (mono_class_from_mono_type_internal (caller_ret), NULL)
		       == mono_class_value_size (mono_class_from_mono_type_internal (callee_ret), NULL);

	return TRUE;
}

/**
 * Returns the reason the tail. prefix at this call site cannot be honoured, or
 * NULL to hand this frame to the callee instead of making an ordinary call.
 *
 * Every shape the LLVM back end honours in should_tail_call () must be honoured
 * here too. A method can run in either engine, and under tier 0 in both, so a
 * guarantee only one keeps is not a guarantee.
 *
 * The reverse does not hold. The compiler declines several shapes for reasons
 * about prototypes and argument registers. Those reasons mean nothing to an
 * engine whose calling convention is a byte image laid out from the call
 * site's signature. The interpreter declines a shape only where it has a
 * reason of its own.
 *
 * td->sp still holds the arguments, so this must run before they are popped.
 */
static const char *
interp_tail_call_refusal (TransformData *td, MonoMethod *method, MonoMethod *target_method,
                          MonoMethodSignature *csignature, gboolean calli, gboolean is_virtual,
                          int op)
{
	MonoMethodHeader *header = td->header;
	int in_offset = td->ip - td->il_code;
	int nargs = csignature->param_count + !!csignature->hasthis;
	int i;

	/* An intrinsified call is emitted inline and has no frame to hand over. */
	if (op != -1)
		return "the call is intrinsified";

	/*
	 * A dispatched call is fine. The target is resolved to an InterpMethod at
	 * the site, before the frame changes hands. Every override reads its
	 * arguments from the layout the site's signature already produced.
	 *
	 * An indirect one is not done here: we have not needed it yet, and its
	 * target can turn out to be a p/invoke.
	 */
	if (calli)
		return "the target is indirect";
	if (target_method == NULL)
		return "the target is unknown";
	/* A marshalbyref target goes through a remoting check that wants a frame. */
	if (is_virtual && mono_class_is_marshalbyref (target_method->klass))
		return "the target may be remote";

	/* A frame that is gone has nowhere to stop at. The method-exit sequence
	 * point that a step-out lands on is one the folded return never reaches. */
	if (td->gen_sdb_seq_points)
		return "the debugger is watching";

	/* Under either of these CEE_RET emits MINT_PROF_EXIT, which does the return
	 * itself. A tail call would jump past it. */
	if (td->rtm->prof_flags & MONO_PROFILER_CALL_INSTRUMENTATION_LEAVE)
		return "the profiler wants the method exit";
	if (mono_jit_trace_calls != NULL && mono_trace_eval (method))
		return "the method is traced";

	/* A vararg caller's cookie buffer is allocated in the frame being handed
	 * away, and read for the whole of the callee's execution. A vararg
	 * callee's own signature is not the one the site pushed. */
	if (td->rtm->vararg)
		return "the caller is vararg";
	if (csignature->call_convention == MONO_CALL_VARARG)
		return "the callee is vararg";

	/* Neither is entered by running IL: the transition saves state that a jump
	 * straight into the callee would skip. */
	if (target_method->flags & METHOD_ATTRIBUTE_PINVOKE_IMPL)
		return "the callee is a p/invoke";
	if (target_method->iflags & METHOD_IMPL_ATTRIBUTE_INTERNAL_CALL)
		return "the callee is an internal call";

	/*
	 * III.2.4 forbids the prefix inside a protected block, and a reused frame
	 * would leave the clause ranges describing a method that is no longer
	 * running. td->clause_indexes only covers handler bodies, so the try and
	 * filter ranges have to be looked up here.
	 */
	for (i = 0; i < header->num_clauses; i++) {
		MonoExceptionClause *c = header->clauses + i;

		if (MONO_OFFSET_IN_CLAUSE (c, in_offset) || MONO_OFFSET_IN_HANDLER (c, in_offset))
			return "the site is inside a protected block";
		if (c->flags == MONO_EXCEPTION_CLAUSE_FILTER && in_offset >= (int) c->data.filter_offset
		    && in_offset < (int) c->handler_offset)
			return "the site is inside a filter";
	}

	/* The prefix promises a ret follows at once, which is what lets the two
	 * returns fold into one. The arguments must be all the evaluation stack
	 * holds. */
	if (in_offset + 5 >= td->code_size || *(td->ip + 5) != CEE_RET)
		return "no ret follows the call";
	if (td->sp - td->stack != nargs)
		return "the evaluation stack outlives the call";

	if (!interp_tail_call_returns_match (mono_method_signature_internal (method)->ret,
	                                     csignature->ret))
		return "the two returns are shaped differently";

	/* A value type's this is a pointer to the value, and the value is usually a local
	 * of the frame being handed away. */
	if (csignature->hasthis && m_class_is_valuetype (target_method->klass))
		return "this is a value type";

	for (i = 0; i < csignature->param_count; i++) {
		if (!interp_tail_call_arg_is_safe (csignature->params[i],
		                                   td->sp[i - csignature->param_count].type))
			return "an argument may point into the frame";
	}

	return NULL;
}

bool
TransformData::may_call_through_context (MonoMethod *body, MonoMethod *target,
                                         MonoMethodSignature *csignature, gboolean is_virtual)
{
	// A fetch reads the receiver of the body being written, which is the
	// caller's rather than the callee's.
	if (body != this->method) {
		cannot_share ("a call inside an inlined callee");
		return false;
	}

	if (csignature->call_convention == MONO_CALL_VARARG) {
		cannot_share ("a vararg call to a method the generic context names");
		return false;
	}

	if (is_virtual)
		return dispatch_reads_the_context (target);

	return true;
}

bool
TransformData::dispatch_reads_the_context (MonoMethod *target)
{
	/*
	 * The slot off an interface is an offset into the receiver's interface
	 * table. get_virtual_method () looks that up under the interface the callee
	 * is declared on, and the receiver implements the instantiation's rather
	 * than the shared form's, so the site fetches the method the instantiation
	 * declares.
	 *
	 * The slot the site carries stays a constant, because
	 * mono_method_get_imt_slot () hashes an inflated method's definition rather
	 * than the method itself.
	 */
	if (mono_class_is_interface (target->klass))
		return true;

	/*
	 * A remoted class keeps the site dispatched even for a method that is not
	 * virtual, and get_virtual_method () then hands the method it was given
	 * straight back as the callee. So the site fetches, and what it hands over
	 * is the instantiation's own.
	 */
	if (mono_class_is_marshalbyref (target->klass))
		return true;

	/*
	 * get_virtual_method () re-inflates the override it finds with the type
	 * arguments of the method it was handed, so a method with arguments of its
	 * own has to arrive as the instantiation's.
	 */
	MonoGenericContext *own = mini_method_get_context (target);

	if (own != nullptr && own->method_inst != nullptr)
		return true;

	/*
	 * A class slot is an index into the receiver's vtable, and reference
	 * sharing keeps that common, so the shared form names the site as well as
	 * the instantiation would.
	 */
	return false;
}

/* Return FALSE if error, including inline failure */
gboolean
TransformData::interp_transform_call (MonoMethod *method, MonoMethod *target_method,
                                      MonoDomain *domain, MonoGenericContext *generic_context,
                                      MonoClass *constrained_class, gboolean readonly,
                                      MonoError *error, gboolean check_visibility,
                                      gboolean save_last_error, gboolean tailcall)
{
	MonoImage *image = m_class_get_image (method->klass);
	MonoMethodSignature *csignature;
	int is_virtual = *ip == CEE_CALLVIRT;
	int calli_extra_arg = *ip == CEE_MONO_CALLI_EXTRA_ARG;
	int calli = *ip == CEE_CALLI || calli_extra_arg;
	int i;
	guint32 res_size = 0;
	int op = -1;
	int native = 0;
	int need_null_check = is_virtual;
	int fp_sreg = -1, first_sreg = -1, dreg = -1;
	gboolean is_delegate_invoke = FALSE;
	gboolean emit_tailcall = FALSE;

	guint32 token = read32 (ip + 1);

	if (target_method == NULL) {
		if (calli) {
			CHECK_STACK (1);
			if (method->wrapper_type != MONO_WRAPPER_NONE)
				csignature = (MonoMethodSignature *) mono_method_get_wrapper_data (method, token);
			else {
				csignature = mono_metadata_parse_signature_checked (image, token, error);
				return_val_if_nok (error, FALSE);
			}

			if (generic_context) {
				csignature = mono_inflate_generic_signature (csignature, generic_context, error);
				return_val_if_nok (error, FALSE);
			}

			/*
			 * The compiled interp entry wrapper is passed to runtime_invoke instead of
			 * the InterpMethod pointer. FIXME
			 */
			native = csignature->pinvoke || method->wrapper_type == MONO_WRAPPER_RUNTIME_INVOKE;

			target_method = NULL;
		} else {
			target_method = interp_get_method (method, token, image, generic_context, error);
			return_val_if_nok (error, FALSE);
			csignature = mono_method_signature_internal (target_method);

			if (generic_context) {
				csignature = mono_inflate_generic_signature (csignature, generic_context, error);
				return_val_if_nok (error, FALSE);
				target_method = mono_class_inflate_generic_method_checked (target_method,
				                                                           generic_context, error);
				return_val_if_nok (error, FALSE);
			}
		}
	} else {
		csignature = mono_method_signature_internal (target_method);
	}

	/*
	 * Every path above has settled the target by now, whichever token shape it
	 * came from, so one test covers the lot.
	 *
	 * Whether such a callee is reached through a fetch is settled further down,
	 * once is_virtual has, and callee_from_context is what routes the rest of
	 * this function onto the fetch. context_named is the wider question, and it
	 * keeps the shared form away from what answers for the method it matched.
	 */
	bool context_named = false;
	bool callee_from_context = false;
	bool shared_constraint = false;

	if (sharing) {
		/*
		 * Reference sharing keeps a class a value type in every instantiation
		 * or none, so this splits the same way for all of them. Only the
		 * reference arm skips the refinement further down, which ECMA does not
		 * ask for there and which cannot answer for an open class anyway. A
		 * value type keeps it, because reference sharing gives every
		 * instantiation the same vtable layout and so the same answer.
		 */
		if (constrained_class != nullptr && depends_on_context (constrained_class))
			shared_constraint = !m_class_is_valuetype (constrained_class);

		if (target_method != nullptr && depends_on_context (target_method))
			context_named = true;

		/*
		 * Before the class initializer below, which would otherwise run on a
		 * shared class. Asking one for a vtable reaches its type variables,
		 * and the collector has no bitmap for a field of that type.
		 */
		if (sharing_refusal != nullptr)
			return TRUE;
	}

	if (check_visibility && target_method && !mono_method_can_access_method (method, target_method))
		interp_generate_mae_throw (method, target_method);

	if (target_method && target_method->string_ctor) {
		/* Create the real signature */
		MonoMethodSignature *ctor_sig =
			mono_metadata_signature_dup_mempool (arena.pool (), csignature);
		ctor_sig->ret = m_class_get_byval_arg (mono_defaults.string_class);

		csignature = ctor_sig;
	}

	/* Ahead of the intrinsics, which read the arguments where they lie. A
	 * calli's function pointer is above them, so it is not the last argument. */
	StackInfo *args = this->sp - csignature->param_count - (calli ? 1 : 0);

	for (i = 0; i < csignature->param_count; i++) {
		if (args[i].type == StackType::R4 || args[i].type == StackType::R8)
			coerce_fp (args + i, fp_stack_type (csignature->params[i]));
	}

	/* Intrinsics */
	// An intrinsic answers for the method it matched, and a shared form matches
	// as the type variable rather than as the instantiation.
	if (target_method && !context_named
	    && interp_handle_intrinsics (target_method, constrained_class, csignature, readonly, &op))
		return TRUE;

	if (constrained_class) {
		if (m_class_is_enumtype (constrained_class)
		    && !strcmp (target_method->name, "GetHashCode")) {
			/* Use the corresponding method from the base type to avoid boxing */
			MonoType *base_type = mono_class_enum_basetype_internal (constrained_class);
			g_assert (base_type);
			constrained_class = mono_class_from_mono_type_internal (base_type);
			target_method = mono_class_get_method_from_name_checked (
				constrained_class, target_method->name, 0, 0, error);
			mono_error_assert_ok (error);
			g_assert (target_method);
		}
	}

	if (constrained_class) {
		mono_class_setup_vtable (constrained_class);
		if (mono_class_has_failure (constrained_class)) {
			mono_error_set_for_class_failure (error, constrained_class);
			return FALSE;
		}
#if DEBUG_INTERP
		g_print ("CONSTRAINED.CALLVIRT: %s::%s.  %s (%p) ->\n", target_method->klass->name,
		         target_method->name, mono_signature_full_name (target_method->signature),
		         target_method);
#endif
		/*
		 * The refinement asks whether the constrained class is assignable to
		 * the method's, and a shared body hands it the shared form rather than
		 * what the constraint promised. ECMA-335 III.2.1 asks for no
		 * refinement anyway where thisType is a reference type: ptr is
		 * dereferenced and the call is an ordinary callvirt on the method the
		 * token named. The arm below is that dereference.
		 */
		if (!shared_constraint) {
			target_method = mono_get_method_constrained_with_method (
				image, target_method, constrained_class, generic_context, error);
#if DEBUG_INTERP
			g_print ("                    : %s::%s.  %s (%p)\n", target_method->klass->name,
			         target_method->name, mono_signature_full_name (target_method->signature),
			         target_method);
#endif
			/* Intrinsics: try again. mono_get_method_constrained_with_method () can resolve to a method we can substitute. */
			if (target_method && !context_named
			    && interp_handle_intrinsics (target_method, constrained_class, csignature, readonly,
			                                 &op))
				return TRUE;

			return_val_if_nok (error, FALSE);
			mono_class_setup_vtable (target_method->klass);

			// The refinement can land on a class the context names where the
			// method the token named was common.
			if (sharing && !context_named && depends_on_context (target_method))
				context_named = true;
		}

		// Follow the rules for constrained calls from ECMA spec
		if (!m_class_is_valuetype (constrained_class)) {
			StackInfo *sp = this->sp - 1 - csignature->param_count;
			/* managed pointer on the stack, we need to deref that puppy */
			interp_add_ins (MINT_LDIND_I);
			interp_ins_set_sreg (last_ins, sp->local);
			set_simple_type_and_local (sp, StackType::I);
			interp_ins_set_dreg (last_ins, sp->local);
		} else if (target_method->klass != constrained_class) {
			/*
			 * The type parameter is instantiated as a valuetype,
			 * but that type doesn't override the method we're
			 * calling, so we need to box `this'.
			 */
			StackType this_type = (this->sp - csignature->param_count - 1)->type;
			g_assert (this_type == StackType::I || this_type == StackType::MP);
			interp_constrained_box (domain, constrained_class, csignature, error);
			if (sharing_refusal != nullptr)
				return TRUE;
			return_val_if_nok (error, FALSE);
		} else {
			is_virtual = FALSE;
		}
	}

	// A shared class is left alone. Its own instantiation is initialized where
	// the call lands, and this one has no storage of its own to prepare.
	if (target_method && !context_named)
		mono_class_init_internal (target_method->klass);

	if (!is_virtual && target_method && (target_method->flags & METHOD_ATTRIBUTE_ABSTRACT)) {
		if (!mono_class_is_interface (method->klass))
			interp_generate_bie_throw ();
		else
			is_virtual = TRUE;
	}

	if (is_virtual && target_method
	    && (!(target_method->flags & METHOD_ATTRIBUTE_VIRTUAL)
	        || (MONO_METHOD_IS_FINAL (target_method)
	            && target_method->wrapper_type != MONO_WRAPPER_REMOTING_INVOKE_WITH_CHECK))
	    && !(mono_class_is_marshalbyref (target_method->klass))) {
		/* Not really virtual: it needs a null check */
		is_virtual = FALSE;
		need_null_check = TRUE;
	}

	// Here rather than where context_named was decided, because whether the
	// site dispatches is what settles the route and is_virtual has only just
	// stopped moving.
	if (context_named) {
		callee_from_context =
			may_call_through_context (method, target_method, csignature, is_virtual);

		if (sharing_refusal != nullptr)
			return TRUE;
	}

	CHECK_STACK (csignature->param_count + csignature->hasthis);
	if (tailcall) {
		const char *refusal = interp_tail_call_refusal (this, method, target_method, csignature,
		                                                calli, is_virtual, op);

		/* A declined tail call is an ordinary call and return, so nothing about it
		 * shows up in what the program does. This is the only way to tell one from a
		 * site that took the jump. */
		if (refusal && verbose_level)
			g_print ("Decline tail call at IL_%04x: %s\n", (int) (ip - il_code), refusal);

		emit_tailcall = refusal == NULL;
	}

	if (emit_tailcall) {
		/*
		 * The ask below reaches a shared class, which is open and has no
		 * runtime vtable. A declined tail call is an ordinary call and never
		 * gets here, so the refusal costs only the sites that take the jump.
		 */
		if (context_named) {
			cannot_share ("a tail call to a method the generic context names");
			return TRUE;
		}

		(void) mono_class_vtable_checked (domain, target_method->klass, error);
		return_val_if_nok (error, FALSE);

		/*
		 * A method calling itself needs no call at all: write the arguments back over
		 * the incoming ones and branch to the top. That skips the argument move and
		 * the frame bookkeeping a general tail call pays for. The target must be the
		 * method itself, not merely the one named at the site. A dispatched call can
		 * land on an override, so it must keep its own resolution.
		 */
		if (method == target_method && !is_virtual) {
			if (inlined_method)
				return FALSE;

			if (verbose_level)
				g_print ("Optimize tail call of %s.%s\n", m_class_get_name (target_method->klass),
				         target_method->name);

			for (i = csignature->param_count - 1 + !!csignature->hasthis; i >= 0; --i)
				store_arg (i);

			/*
			 * The next invocation owns none of this one's localloc memory, and the
			 * branch never reaches a ret to release it. Whether the method locallocs
			 * at all is not settled until the whole body has been read, so this is
			 * emitted unconditionally. It costs a load and a predictable branch.
			 */
			interp_add_ins (MINT_LOCALLOC_UNWIND);

			interp_add_ins (MINT_BR_S);
			// We are branching to the beginning of the method
			last_ins->info.target_bb = entry_bb;
			int in_offset = ip - il_code;
			if (interp_ip_in_cbb (in_offset + 5))
				++ip; /* gobble the CEE_RET if it isn't branched to */
			ip += 5;
			return TRUE;
		}
	}

	target_method = interp_transform_internal_calls (method, target_method, csignature, is_virtual);

	/*
	 * A replaced method is called as its replacement from here on, so a site
	 * that copies the callee's body into itself copies the replacement's. A
	 * dispatched site is left alone: it settles its callee at run time, and
	 * mono_interp_get_imethod () substitutes there.
	 *
	 * The site keeps its own signature. An override is static where the method
	 * it replaces is not, and takes the receiver as its first argument. The two
	 * occupy the same stack slots either way.
	 */
	if (!is_virtual && target_method != NULL) {
		MonoMethod *replacement = mono::method_override_for (domain, target_method);

		if (replacement != NULL)
			target_method = replacement;
	}

	/*
	 * A wrapper stands in for the method the site named, and an rgctx entry
	 * holds no wrapper. Tested on the target rather than on what produces one,
	 * because both substitutions above can and the conditions are theirs.
	 */
	if (callee_from_context && target_method->wrapper_type != MONO_WRAPPER_NONE) {
		cannot_share ("a call a wrapper stands in for");
		return TRUE;
	}

	if (csignature->call_convention == MONO_CALL_VARARG)
		csignature =
			mono_method_get_signature_checked (target_method, image, token, generic_context, error);

	/* A null check tests the receiver, so a signature with no this has nothing to
	 * test. A callvirt that names a static method arrives here: the block above
	 * degrades it to a plain call, and the slot under the arguments then belongs
	 * to the caller instead of to this call. */
	if (need_null_check && csignature->hasthis) {
		StackInfo *sp = this->sp - 1 - csignature->param_count;
		interp_add_ins (MINT_CKNULL);
		interp_ins_set_sreg (last_ins, sp->local);
		set_simple_type_and_local (sp, sp->type);
		interp_ins_set_dreg (last_ins, sp->local);
	}

	g_assert (csignature->call_convention != MONO_CALL_FASTCALL);
	// A shared callee is not inlined: its body reads a generic context of its
	// own, which the receiver of this one does not carry.
	if ((mono_interp_opt & INTERP_OPT_INLINE) && op == -1 && !is_virtual && !callee_from_context
	    && target_method
	    && interp_method_check_inlining (target_method, csignature)) {
		MonoMethodHeader *mheader = interp_method_get_header (target_method, error);
		return_val_if_nok (error, FALSE);

		if (interp_inline_method (target_method, mheader, error)) {
			ip += 5;
			return TRUE;
		}
	}

	/* Don't inline methods that do calls */
	if (op == -1 && inlined_method)
		return FALSE;

	/* We need to convert delegate invoke to a indirect call on the interp_invoke_impl field */
	if (target_method
	    && m_class_get_parent (target_method->klass) == mono_defaults.multicastdelegate_class) {
		const char *name = target_method->name;
		if (*name == 'I' && (strcmp (name, "Invoke") == 0))
			is_delegate_invoke = TRUE;
	}

	/*
	 * MINT_CALL_DELEGATE settles its callee off the delegate object, which is
	 * the instantiation's own, and reads the signature for its parameter count
	 * alone. Reference sharing keeps that count common, so the site needs
	 * nothing fetched.
	 */
	if (is_delegate_invoke)
		callee_from_context = false;

	/* Pop the function pointer */
	if (calli) {
		--this->sp;
		fp_sreg = this->sp[0].local;
		locals[fp_sreg].flags |= INTERP_LOCAL_FLAG_CALL_ARGS;
	}

	/* The callee reads each argument at the width its parameter is declared at. */
	for (i = 0; i < csignature->param_count; i++)
		widen_i4_to_i8 (this->sp - csignature->param_count + i, csignature->params[i]);

	guint32 tos_offset = get_tos_offset ();
	this->sp -= csignature->param_count + !!csignature->hasthis;
	guint32 params_stack_size = tos_offset - get_tos_offset ();

	if (op == -1 || num_dregs (op) == MINT_CALL_ARGS) {
		// We must not optimize out these locals, storing to them is part of the interp call convention
		// unless we already intrinsified this call
		for (int i = 0; i < (csignature->param_count + !!csignature->hasthis); i++)
			locals[this->sp[i].local].flags |= INTERP_LOCAL_FLAG_CALL_ARGS;
	}

	// We overwrite it with the return local, save it for future use
	if (csignature->param_count || csignature->hasthis)
		first_sreg = this->sp[0].local;

	/* need to handle typedbyref ... */
	if (csignature->ret->type != MONO_TYPE_VOID) {
		MintType mt = mint_type (csignature->ret);
		MonoClass *klass = mono_class_from_mono_type_internal (csignature->ret);

		if (mt == MintType::VT) {
			if (csignature->pinvoke && method->wrapper_type != MONO_WRAPPER_NONE)
				res_size = mono_class_native_size (klass, NULL);
			else
				res_size = mono_class_value_size (klass, NULL);
			push_type_vt (klass, res_size);
			res_size = ALIGN_TO (res_size, MINT_VT_ALIGNMENT);
			if (mono_class_has_failure (klass)) {
				mono_error_set_for_class_failure (error, klass);
				return FALSE;
			}
		} else {
			push_type (stack_type_of (mt), klass);
			res_size = MINT_STACK_SLOT_SIZE;
		}
		dreg = this->sp[-1].local;
		if (op == -1 || num_dregs (op) == MINT_CALL_ARGS) {
			// This dreg needs to be at the same offset as the call args
			locals[dreg].flags |= INTERP_LOCAL_FLAG_CALL_ARGS;
		}
	} else {
		// Create a new dummy local to serve as the dreg of the call
		// This dreg is only used to resolve the call args offset
		push_simple_type (StackType::I4);
		this->sp--;
		dreg = this->sp[0].local;
	}

	if (op >= 0) {
		interp_add_ins (op);

		int has_dreg = num_dregs (op);
		int sregs_count = num_sregs (op);
		if (has_dreg)
			interp_ins_set_dreg (last_ins, dreg);
		if (sregs_count > 0) {
			if (sregs_count == 1)
				interp_ins_set_sreg (last_ins, first_sreg);
			else if (sregs_count == 2)
				interp_ins_set_sregs2 (last_ins, first_sreg, this->sp[!has_dreg].local);
			else if (sregs_count == 3)
				interp_ins_set_sregs3 (last_ins, first_sreg, this->sp[!has_dreg].local,
				                       this->sp[!has_dreg + 1].local);
			else
				g_error ("Unsupported opcode");
		}

		if (op == MINT_LDLEN) {
#ifdef MONO_BIG_ARRAYS
			SET_SIMPLE_TYPE (this->sp - 1, StackType::I8);
#else
			SET_SIMPLE_TYPE (this->sp - 1, StackType::I4);
#endif
		}

#ifndef ENABLE_NETCORE
		if (op == MINT_CALLRUN) {
			interp_ins_set_dreg (last_ins, dreg);
			last_ins->data[0] = get_data_item_index (target_method);
			last_ins->data[1] =
				get_data_item_index (mono_method_signature_internal (target_method));
		}
#endif
	} else if (!calli && !is_delegate_invoke && !is_virtual && !emit_tailcall
	           && !callee_from_context
	           && mono_interp_jit_call_supported (target_method, csignature)) {
		interp_add_ins (MINT_JIT_CALL);
		interp_ins_set_dreg (last_ins, dreg);
		last_ins->data[0] =
			get_data_item_index ((void *) mono_interp_get_imethod (domain, target_method, error));
		mono_error_assert_ok (error);
	} else {
		if (is_delegate_invoke) {
			interp_add_ins (MINT_CALL_DELEGATE);
			interp_ins_set_dreg (last_ins, dreg);
			last_ins->data[0] = params_stack_size;
			last_ins->data[1] = get_data_item_index ((void *) csignature);
		} else if (calli) {
#ifndef MONO_ARCH_HAS_NO_PROPER_MONOCTX
			/* Try using fast icall path for simple signatures */
			if (native && !method->dynamic)
				op = interp_icall_op_for_sig (csignature);
#endif
			// FIXME calli receives both the args offset and sometimes another arg for the frame pointer,
			// therefore some args are in the param area, while the fp is not. We should differentiate for
			// this, probably once we will have an explicit param area where we copy arguments.
			if (op != -1) {
				interp_add_ins (MINT_CALLI_NAT_FAST);
				interp_ins_set_dreg (last_ins, dreg);
				last_ins->data[0] = get_data_item_index ((void *) csignature);
				last_ins->data[1] = op;
				last_ins->data[2] = save_last_error;
			} else if (native && method->dynamic && csignature->pinvoke) {
				interp_add_ins (MINT_CALLI_NAT_DYNAMIC);
				interp_ins_set_dreg (last_ins, dreg);
				interp_ins_set_sreg (last_ins, fp_sreg);
				last_ins->data[0] = get_data_item_index ((void *) csignature);
			} else if (native) {
				interp_add_ins (MINT_CALLI_NAT);
#ifdef TARGET_X86
				/* Windows not tested/supported yet */
				g_assertf (csignature->call_convention == MONO_CALL_DEFAULT
				               || csignature->call_convention == MONO_CALL_C,
				           "Interpreter supports only cdecl pinvoke on x86");
#endif

				InterpMethod *imethod = NULL;
				/*
				 * We can have pinvoke calls outside M2N wrappers, in xdomain calls, where we can't easily get the called imethod.
				 * Those calls will be slower since we will not cache the arg offsets on the imethod, and have to compute them
				 * every time based on the signature.
				 */
				if (method->wrapper_type == MONO_WRAPPER_MANAGED_TO_NATIVE) {
					WrapperInfo *info = mono_marshal_get_wrapper_info (method);
					if (info) {
						MonoMethod *pinvoke_method = info->d.managed_to_native.method;
						imethod = mono_interp_get_imethod (domain, pinvoke_method, error);
						return_val_if_nok (error, FALSE);
					}
				}

				interp_ins_set_dreg (last_ins, dreg);
				interp_ins_set_sreg (last_ins, fp_sreg);
				last_ins->data[0] = get_data_item_index (csignature);
				last_ins->data[1] = get_data_item_index (imethod);
				last_ins->data[2] = save_last_error;
				/* Cache slot */
				last_ins->data[3] = get_data_item_index_nonshared (NULL);
			} else if (calli_extra_arg) {
				/*
				 * Only a delegate-invoke wrapper emits this, and only over what
				 * MINT_LD_DELEGATE_METHOD_PTR pushed - the delegate's target
				 * as an InterpMethod, which needs no resolving.
				 */
				interp_add_ins (MINT_CALLI_IMETHOD);
				interp_ins_set_dreg (last_ins, dreg);
				interp_ins_set_sreg (last_ins, fp_sreg);
				last_ins->data[0] = get_data_item_index ((void *) csignature);
			} else {
				interp_add_ins (MINT_CALLI);
				interp_ins_set_dreg (last_ins, dreg);
				interp_ins_set_sreg (last_ins, fp_sreg);
				last_ins->data[0] = get_data_item_index ((void *) csignature);
				/* Cache slot for the entry point this site last resolved */
				last_ins->data[1] = get_data_item_index_nonshared (NULL);
			}
		} else if (callee_from_context) {
			int callee = emit_rgctx_fetch (MONO_RGCTX_INFO_INTERP_METHOD, target_method);

			if (sharing_refusal != nullptr)
				return TRUE;

			// A remoted receiver can be a proxy with no slot of its own, which
			// is why the ordinary arm sends one to MINT_CALLVIRT as well.
			bool by_slot = is_virtual && !mono_class_is_marshalbyref (target_method->klass);

			if (!is_virtual)
				interp_add_ins (MINT_CALL_DYN);
			else if (by_slot)
				interp_add_ins (MINT_CALLVIRT_FAST_DYN);
			else
				interp_add_ins (MINT_CALLVIRT_DYN);

			interp_ins_set_dreg (last_ins, dreg);
			interp_ins_set_sreg (last_ins, callee);

			// The slot comes off the shared form, which the two lookups below
			// answer for as well as the instantiation would. An interface slot
			// is hashed from the method's definition, and a class slot is an
			// index reference sharing keeps common.
			if (by_slot) {
				if (mono_class_is_interface (target_method->klass))
					last_ins->data[0] =
						-2 * MONO_IMT_SIZE + mono_method_get_imt_slot (target_method);
				else
					last_ins->data[0] = mono_method_get_vtable_slot (target_method);
			}
		} else {
			InterpMethod *imethod = mono_interp_get_imethod (domain, target_method, error);
			return_val_if_nok (error, FALSE);

			if (csignature->call_convention == MONO_CALL_VARARG) {
				interp_add_ins (MINT_CALL_VARARG);
				last_ins->data[1] = get_data_item_index ((void *) csignature);
				last_ins->data[2] = params_stack_size;
			} else if (is_virtual && !mono_class_is_marshalbyref (target_method->klass)) {
				interp_add_ins (emit_tailcall ? MINT_TAILCALLVIRT_FAST : MINT_CALLVIRT_FAST);
				if (mono_class_is_interface (target_method->klass))
					last_ins->data[1] =
						-2 * MONO_IMT_SIZE + mono_method_get_imt_slot (target_method);
				else
					last_ins->data[1] = mono_method_get_vtable_slot (target_method);
				/* How much of this frame the callee is handed as its arguments. */
				if (emit_tailcall)
					last_ins->data[2] = params_stack_size;
			} else if (is_virtual) {
				interp_add_ins (MINT_CALLVIRT);
			} else if (emit_tailcall) {
				interp_add_ins (MINT_TAILCALL);
				last_ins->data[1] = params_stack_size;
			} else {
				interp_add_ins (MINT_CALL);
			}
			interp_ins_set_dreg (last_ins, dreg);
			last_ins->data[0] = get_data_item_index ((void *) imethod);
		}
	}
	ip += 5;

	return TRUE;
}

MonoClassField *
interp_field_from_token (MonoMethod *method, guint32 token, MonoClass **klass,
                         MonoGenericContext *generic_context, MonoError *error)
{
	MonoClassField *field = NULL;
	if (method->wrapper_type != MONO_WRAPPER_NONE) {
		field = (MonoClassField *) mono_method_get_wrapper_data (method, token);
		*klass = field->parent;

		mono_class_setup_fields (field->parent);
	} else {
		field = mono_field_from_token_checked (m_class_get_image (method->klass), token, klass,
		                                       generic_context, error);
		return_val_if_nok (error, NULL);
	}

	if (!method->skip_visibility && !mono_method_can_access_field (method, field)) {
		char *method_fname = mono_method_full_name (method, TRUE);
		char *field_fname = mono_field_full_name (field);
		mono_error_set_generic_error (error, "System", "FieldAccessException",
		                              "Field `%s' is inaccessible from method `%s'\n", field_fname,
		                              method_fname);
		g_free (method_fname);
		g_free (field_fname);
		return NULL;
	}

	return field;
}

} // namespace mono::interp

/**
 * \file
 * \brief Calling the runtime's own C functions from interpreted code.
 */

#include "config.h"

#include "interp-callbacks.hpp"
#include "interp-internals.h"
#include "frame-data.hpp"
#include "interp-internals.hpp"
#include "interp-array.hpp"
#include "interp-icall.hpp"
#include "interp-lmf.hpp"

#include <mono/metadata/exception.h>
#include <mono/mini/jit-icalls.h>
#include <mono/mini/mini-runtime.h>

namespace mono::interp {

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

#ifndef ENABLE_NETCORE
MONO_NEVER_INLINE MonoException*
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

// Do not inline in case order of frame addresses matters, and maybe other reasons.
MONO_NO_OPTIMIZATION MONO_NEVER_INLINE gpointer
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

/*
 * Fill the buffer ArgIterator walks: the call-site signature in the first word,
 * then the variable arguments behind it.
 *
 * ves_icall_System_ArgIterator_IntGetNextArg () advances by mono_type_stack_size
 * and realigns only on arm and mips, so everywhere else the offsets are that
 * running sum and nothing more. A float is four bytes rather than a whole slot,
 * so padding one out to its slot would leave every argument behind it misread.
 */
void
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

} // namespace mono::interp

/* Outside the namespace: interp-internals.h declares these for C, so the
 * definitions have to match the C linkage that gives them. */

using namespace mono::interp;

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

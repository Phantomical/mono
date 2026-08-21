/**
 * \file
 * \brief Entering the interpreter from code that is not the interpreter.
 */

#include "config.h"

#include "callbacks.hpp"
#include "internals.hpp"
#include "call.hpp"
#include "entry.hpp"
#include "imethod.hpp"

#include <mono/metadata/marshal.h>
#include <mono/llvm/runtime.h>
#include <mono/mini/aot-runtime.h>
#include <mono/mini/llvmonly-runtime.h>
#include <mono/mini/mini-runtime.h>
#include <mono/utils/mono-logger-internals.h>

namespace mono::interp {

#ifdef MONO_ARCH_HAVE_INTERP_ENTRY_TRAMPOLINE
static MonoFuncV mono_native_to_interp_trampoline = NULL;
#endif

/*
 * These functions are the entry points into the interpreter from compiled code.
 * They are called by the interp_in wrappers. They have the following signature:
 * void (<optional this_arg>, <optional retval pointer>, <arg1>, ..., <argn>, <method ptr>)
 * They pack up their arguments into an InterpEntryData structure and call mono_interp_entry ().
 * this/static * ret/void * 9 argument counts (0 through MAX_INTERP_ENTRY_ARGS) -> 36 functions.
 * The wrappers can pack the arguments themselves, but that makes each wrapper bigger, and
 * there are more wrappers than there are of these functions.
 */

#define INTERP_ENTRY_BASE(_method, _this_arg, _res) \
	InterpEntryData data;                           \
	(data).rmethod = (_method);                     \
	(data).res = (_res);                            \
	(data).this_arg = (_this_arg);                  \
	(data).many_args = NULL;

#define INTERP_ENTRY0(_this_arg, _res, _method)       \
	{                                                 \
		INTERP_ENTRY_BASE (_method, _this_arg, _res); \
		mono_interp_entry (&data);                    \
	}
#define INTERP_ENTRY1(_this_arg, _res, _method)       \
	{                                                 \
		INTERP_ENTRY_BASE (_method, _this_arg, _res); \
		(data).args[0] = arg1;                        \
		mono_interp_entry (&data);                    \
	}
#define INTERP_ENTRY2(_this_arg, _res, _method)       \
	{                                                 \
		INTERP_ENTRY_BASE (_method, _this_arg, _res); \
		(data).args[0] = arg1;                        \
		(data).args[1] = arg2;                        \
		mono_interp_entry (&data);                    \
	}
#define INTERP_ENTRY3(_this_arg, _res, _method)       \
	{                                                 \
		INTERP_ENTRY_BASE (_method, _this_arg, _res); \
		(data).args[0] = arg1;                        \
		(data).args[1] = arg2;                        \
		(data).args[2] = arg3;                        \
		mono_interp_entry (&data);                    \
	}
#define INTERP_ENTRY4(_this_arg, _res, _method)       \
	{                                                 \
		INTERP_ENTRY_BASE (_method, _this_arg, _res); \
		(data).args[0] = arg1;                        \
		(data).args[1] = arg2;                        \
		(data).args[2] = arg3;                        \
		(data).args[3] = arg4;                        \
		mono_interp_entry (&data);                    \
	}
#define INTERP_ENTRY5(_this_arg, _res, _method)       \
	{                                                 \
		INTERP_ENTRY_BASE (_method, _this_arg, _res); \
		(data).args[0] = arg1;                        \
		(data).args[1] = arg2;                        \
		(data).args[2] = arg3;                        \
		(data).args[3] = arg4;                        \
		(data).args[4] = arg5;                        \
		mono_interp_entry (&data);                    \
	}
#define INTERP_ENTRY6(_this_arg, _res, _method)       \
	{                                                 \
		INTERP_ENTRY_BASE (_method, _this_arg, _res); \
		(data).args[0] = arg1;                        \
		(data).args[1] = arg2;                        \
		(data).args[2] = arg3;                        \
		(data).args[3] = arg4;                        \
		(data).args[4] = arg5;                        \
		(data).args[5] = arg6;                        \
		mono_interp_entry (&data);                    \
	}
#define INTERP_ENTRY7(_this_arg, _res, _method)       \
	{                                                 \
		INTERP_ENTRY_BASE (_method, _this_arg, _res); \
		(data).args[0] = arg1;                        \
		(data).args[1] = arg2;                        \
		(data).args[2] = arg3;                        \
		(data).args[3] = arg4;                        \
		(data).args[4] = arg5;                        \
		(data).args[5] = arg6;                        \
		(data).args[6] = arg7;                        \
		mono_interp_entry (&data);                    \
	}
#define INTERP_ENTRY8(_this_arg, _res, _method)       \
	{                                                 \
		INTERP_ENTRY_BASE (_method, _this_arg, _res); \
		(data).args[0] = arg1;                        \
		(data).args[1] = arg2;                        \
		(data).args[2] = arg3;                        \
		(data).args[3] = arg4;                        \
		(data).args[4] = arg5;                        \
		(data).args[5] = arg6;                        \
		(data).args[6] = arg7;                        \
		(data).args[7] = arg8;                        \
		mono_interp_entry (&data);                    \
	}

#define ARGLIST0 InterpMethod *rmethod
#define ARGLIST1 gpointer arg1, InterpMethod *rmethod
#define ARGLIST2 gpointer arg1, gpointer arg2, InterpMethod *rmethod
#define ARGLIST3 gpointer arg1, gpointer arg2, gpointer arg3, InterpMethod *rmethod
#define ARGLIST4 gpointer arg1, gpointer arg2, gpointer arg3, gpointer arg4, InterpMethod *rmethod
#define ARGLIST5 \
	gpointer arg1, gpointer arg2, gpointer arg3, gpointer arg4, gpointer arg5, InterpMethod *rmethod
#define ARGLIST6                                                                              \
	gpointer arg1, gpointer arg2, gpointer arg3, gpointer arg4, gpointer arg5, gpointer arg6, \
		InterpMethod *rmethod
#define ARGLIST7                                                                              \
	gpointer arg1, gpointer arg2, gpointer arg3, gpointer arg4, gpointer arg5, gpointer arg6, \
		gpointer arg7, InterpMethod *rmethod
#define ARGLIST8                                                                              \
	gpointer arg1, gpointer arg2, gpointer arg3, gpointer arg4, gpointer arg5, gpointer arg6, \
		gpointer arg7, gpointer arg8, InterpMethod *rmethod

static void interp_entry_static_0 (ARGLIST0) INTERP_ENTRY0 (NULL, NULL, rmethod) static void interp_entry_static_1 (
	ARGLIST1) INTERP_ENTRY1 (NULL, NULL, rmethod) static void interp_entry_static_2 (ARGLIST2)
	INTERP_ENTRY2 (NULL, NULL, rmethod) static void interp_entry_static_3 (ARGLIST3) INTERP_ENTRY3 (
		NULL, NULL,
		rmethod) static void interp_entry_static_4 (ARGLIST4) INTERP_ENTRY4 (NULL, NULL,
                                                                             rmethod) static void interp_entry_static_5 (ARGLIST5)
		INTERP_ENTRY5 (NULL, NULL, rmethod) static void interp_entry_static_6 (ARGLIST6) INTERP_ENTRY6 (
			NULL, NULL,
			rmethod) static void interp_entry_static_7 (ARGLIST7) INTERP_ENTRY7 (NULL, NULL,
                                                                                 rmethod) static void interp_entry_static_8 (ARGLIST8)
			INTERP_ENTRY8 (NULL, NULL, rmethod) static void interp_entry_static_ret_0 (
				gpointer res,
				ARGLIST0) INTERP_ENTRY0 (NULL, res,
                                         rmethod) static void interp_entry_static_ret_1 (gpointer
                                                                                             res,
                                                                                         ARGLIST1)
				INTERP_ENTRY1 (NULL, res, rmethod) static void interp_entry_static_ret_2 (
					gpointer res,
					ARGLIST2) INTERP_ENTRY2 (NULL, res,
                                             rmethod) static void interp_entry_static_ret_3 (gpointer
                                                                                                 res,
                                                                                             ARGLIST3)
					INTERP_ENTRY3 (NULL, res, rmethod) static void interp_entry_static_ret_4 (
						gpointer res,
						ARGLIST4) INTERP_ENTRY4 (NULL, res,
                                                 rmethod) static void interp_entry_static_ret_5 (gpointer
                                                                                                     res,
                                                                                                 ARGLIST5)
						INTERP_ENTRY5 (NULL, res, rmethod) static void interp_entry_static_ret_6 (
							gpointer res,
							ARGLIST6) INTERP_ENTRY6 (NULL, res,
                                                     rmethod) static void interp_entry_static_ret_7 (gpointer
                                                                                                         res,
                                                                                                     ARGLIST7)
							INTERP_ENTRY7 (NULL, res, rmethod) static void interp_entry_static_ret_8 (
								gpointer res,
								ARGLIST8) INTERP_ENTRY8 (NULL,
                                                         res, rmethod) static void interp_entry_instance_0 (gpointer this_arg, ARGLIST0)
								INTERP_ENTRY0 (this_arg, NULL, rmethod) static void interp_entry_instance_1 (gpointer this_arg, ARGLIST1) INTERP_ENTRY1 (
									this_arg, NULL,
									rmethod) static void interp_entry_instance_2 (gpointer this_arg,
                                                                                  ARGLIST2)
									INTERP_ENTRY2 (this_arg, NULL, rmethod) static void interp_entry_instance_3 (gpointer this_arg, ARGLIST3) INTERP_ENTRY3 (
										this_arg, NULL,
										rmethod) static void interp_entry_instance_4 (gpointer
                                                                                          this_arg,
                                                                                      ARGLIST4)
										INTERP_ENTRY4 (this_arg, NULL, rmethod) static void interp_entry_instance_5 (gpointer this_arg, ARGLIST5) INTERP_ENTRY5 (
											this_arg, NULL,
											rmethod) static void interp_entry_instance_6 (gpointer
                                                                                              this_arg,
                                                                                          ARGLIST6)
											INTERP_ENTRY6 (
												this_arg, NULL,
												rmethod) static void interp_entry_instance_7 (gpointer
                                                                                                  this_arg,
                                                                                              ARGLIST7)
												INTERP_ENTRY7 (
													this_arg, NULL,
													rmethod) static void interp_entry_instance_8 (gpointer
                                                                                                      this_arg,
                                                                                                  ARGLIST8)
													INTERP_ENTRY8 (this_arg, NULL, rmethod) static void interp_entry_instance_ret_0 (
														gpointer this_arg,
														gpointer res, ARGLIST0)
														INTERP_ENTRY0 (this_arg, res, rmethod) static void interp_entry_instance_ret_1 (
															gpointer this_arg,
															gpointer res, ARGLIST1)
															INTERP_ENTRY1 (this_arg, res, rmethod) static void interp_entry_instance_ret_2 (
																gpointer this_arg,
																gpointer res, ARGLIST2)
																INTERP_ENTRY2 (this_arg, res, rmethod) static void interp_entry_instance_ret_3 (
																	gpointer this_arg, gpointer res,
																	ARGLIST3)
																	INTERP_ENTRY3 (this_arg, res, rmethod) static void interp_entry_instance_ret_4 (
																		gpointer this_arg,
																		gpointer res, ARGLIST4)
																		INTERP_ENTRY4 (this_arg, res, rmethod) static void interp_entry_instance_ret_5 (
																			gpointer this_arg,
																			gpointer res, ARGLIST5)
																			INTERP_ENTRY5 (this_arg, res, rmethod) static void interp_entry_instance_ret_6 (
																				gpointer this_arg,
																				gpointer res,
																				ARGLIST6)
																				INTERP_ENTRY6 (this_arg, res, rmethod) static void interp_entry_instance_ret_7 (
																					gpointer
																						this_arg,
																					gpointer res,
																					ARGLIST7)
																					INTERP_ENTRY7 (this_arg, res, rmethod) static void interp_entry_instance_ret_8 (
																						gpointer
																							this_arg,
																						gpointer
																							res,
																						ARGLIST8)
																						INTERP_ENTRY8 (
																							this_arg,
																							res,
																							rmethod)

#define INTERP_ENTRY_FUNCLIST(type)                                             \
	(gpointer) interp_entry_##type##_0, (gpointer) interp_entry_##type##_1,     \
		(gpointer) interp_entry_##type##_2, (gpointer) interp_entry_##type##_3, \
		(gpointer) interp_entry_##type##_4, (gpointer) interp_entry_##type##_5, \
		(gpointer) interp_entry_##type##_6, (gpointer) interp_entry_##type##_7, \
		(gpointer) interp_entry_##type##_8

																							static gpointer
	const entry_funcs_static[MAX_INTERP_ENTRY_ARGS + 1] = {INTERP_ENTRY_FUNCLIST (static)};
static gpointer const entry_funcs_static_ret[MAX_INTERP_ENTRY_ARGS + 1] = {
	INTERP_ENTRY_FUNCLIST (static_ret)};
static gpointer const entry_funcs_instance[MAX_INTERP_ENTRY_ARGS + 1] = {
	INTERP_ENTRY_FUNCLIST (instance)};
static gpointer const entry_funcs_instance_ret[MAX_INTERP_ENTRY_ARGS + 1] = {
	INTERP_ENTRY_FUNCLIST (instance_ret)};

/// Fills in del->interp_method and del->method from whichever of them, or of
/// del->method_ptr, the delegate already carries.
///
/// del->interp_method can come back naming another method than del->method: a
/// delegate Invoke gets its invoke wrapper, and an abstract virtual gets the
/// target's implementation.
void
interp_init_delegate (MonoDelegate *del, MonoError *error)
{
	MonoDomain *domain = del->object.vtable->domain;
	MonoMethod *method;

	if (del->interp_method) {
		/* A remoting invoke, already resolved by mini_init_delegate (). */
		del->method = (static_cast<InterpMethod *> (del->interp_method))->method;
	} else if (del->method) {
		del->interp_method = mono_interp_get_imethod (domain, del->method, error);
		return_if_nok (error);
	} else if (del->method_ptr) {
		/*
		 * del->method_ptr is only an entry point - ldftn's product, or
		 * MethodHandle.GetFunctionPointer's. Whichever engine published it knows
		 * which method it stands for.
		 */
		del->interp_method = imethod_for_entry (domain, del->method_ptr, error);
		return_if_nok (error);
		g_assert (del->interp_method);
		del->method = (static_cast<InterpMethod *> (del->interp_method))->method;
	} else {
		g_assert_not_reached ();
	}

	method = (static_cast<InterpMethod *> (del->interp_method))->method;
	if (del->target && method && method->flags & METHOD_ATTRIBUTE_VIRTUAL
	    && method->flags & METHOD_ATTRIBUTE_ABSTRACT && mono_class_is_abstract (method->klass))
		del->interp_method = get_virtual_method (static_cast<InterpMethod *> (del->interp_method),
		                                         del->target->vtable);

	method = (static_cast<InterpMethod *> (del->interp_method))->method;
	if (method && m_class_get_parent (method->klass) == mono_defaults.multicastdelegate_class) {
		const char *name = method->name;
		if (*name == 'I' && (strcmp (name, "Invoke") == 0)) {
			/*
			 * When invoking the delegate, interp_method runs directly. If it is an
			 * invoke, replace it with the matching delegate invoke wrapper.
			 *
			 * FIXME: move this later, once we also know which delegate the target
			 * method is called on.
			 */
			del->interp_method = mono_interp_get_imethod (
				domain, mono_marshal_get_delegate_invoke (method, NULL), error);
			mono_error_assert_ok (error);
		}
	}

	if (!(static_cast<InterpMethod *> (del->interp_method))->transformed
	    && method_is_dynamic (method)) {
		/* Return any errors from method compilation */
		mono_interp_transform_method (static_cast<InterpMethod *> (del->interp_method),
		                              mono_interp_get_context (), error);
		return_if_nok (error);
	}
}

gpointer
interp_get_imethod (MonoMethod *method, MonoError *error)
{
	return mono_interp_get_imethod (mono_domain_get (), method, error);
}

/// Returns the interpreter's own entry for imethod, minting one on first use.
static gpointer
entry_for_imethod (InterpMethod *imethod, MonoError *error)
{
	if (imethod->jit_entry)
		return imethod->jit_entry;

	return interp_create_method_pointer (imethod->method, FALSE, error);
}

gpointer
native_entry_for_imethod (InterpMethod *imethod, MonoError *error)
{
	if (mono_ee_features.force_use_interpreter)
		return entry_for_imethod (imethod, error);

	if (imethod->native_entry)
		return imethod->native_entry;

	gpointer addr = mono_llvm_jit_stub_for (imethod->method, imethod->domain, error);

	if (addr == nullptr)
		return nullptr;

	/* Two threads asking at once get the one stub the record holds. */
	mono_memory_barrier ();
	imethod->native_entry = addr;

	return addr;
}

gpointer
native_entry_for_method (MonoMethod *method, MonoDomain *domain, MonoError *error)
{
	if (!mono_ee_features.force_use_interpreter)
		return mono_llvm_jit_stub_for (method, domain, error);

	InterpMethod *imethod = mono_interp_imethod_named (domain, method, error);

	return_val_if_nok (error, nullptr);
	return entry_for_imethod (imethod, error);
}

/// Returns the method an entry point stands for, or NULL if this domain
/// published no such entry.
///
/// ldftn's product is an entry point in both engines. A delegate constructor
/// - or anything else handed one - recovers the method by asking the engine
/// that published it. The jit-info table answers for a compiled entry, this
/// function for an interpreted one.
MonoMethod *
interp_method_from_entry (MonoDomain *domain, gpointer addr)
{
	InterpMethod *imethod = lookup_method_pointer (domain, addr);

	return imethod ? imethod->method : NULL;
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

/// Returns an ftndesc for entering the interpreter to run method.
///
/// \param unbox  when true, the entry steps the receiver past the object
///     header before it runs the method.
MonoFtnDesc *
interp_create_method_pointer_llvmonly (MonoMethod *method, gboolean unbox, MonoError *error)
{
	MonoDomain *domain = mono_domain_get ();
	gpointer addr, entry_func, entry_wrapper;
	MonoMethodSignature *sig;
	MonoMethod *wrapper;
	InterpMethod *imethod;

	imethod = mono_interp_get_imethod (domain, method, error);
	return_val_if_nok (error, NULL);

	if (unbox) {
		if (imethod->llvmonly_unbox_entry)
			return static_cast<MonoFtnDesc *> (imethod->llvmonly_unbox_entry);
	} else {
		if (imethod->jit_entry)
			return static_cast<MonoFtnDesc *> (imethod->jit_entry);
	}

	sig = mono_method_signature_internal (method);

	/*
	 * The entry functions need the method to call, so this passes an ftndesc.
	 *
	 * The caller uses method's normal signature, while the entry functions take a
	 * gsharedvt_in signature. So this wraps the entry function in a
	 * gsharedvt_in_sig wrapper rather than an interp_in wrapper. The two wrapper
	 * kinds are mostly the same, and the gsharedvt_in_sig ones already exist. The
	 * exception is a method with more than MAX_INTERP_ENTRY_ARGS arguments, whose
	 * wrapper differs.
	 */
	if (sig->param_count > MAX_INTERP_ENTRY_ARGS)
		wrapper = mini_get_interp_in_wrapper (sig);
	else
		wrapper = mini_get_gsharedvt_in_sig_wrapper (sig);

	entry_wrapper = mono_jit_compile_method_jit_only (wrapper, error);
	mono_error_assertf_ok (
		error, "couldn't compile wrapper \"%s\" for \"%s\"",
		mono_method_get_name_full (wrapper, TRUE, TRUE, MONO_TYPE_NAME_FORMAT_IL),
		mono_method_get_name_full (method, TRUE, TRUE, MONO_TYPE_NAME_FORMAT_IL));

	if (sig->param_count > MAX_INTERP_ENTRY_ARGS) {
		entry_func = (gpointer) mono_interp_entry_general;
	} else if (sig->hasthis) {
		if (sig->ret->type == MONO_TYPE_VOID)
			entry_func = entry_funcs_instance[sig->param_count];
		else
			entry_func = entry_funcs_instance_ret[sig->param_count];
	} else {
		if (sig->ret->type == MONO_TYPE_VOID)
			entry_func = entry_funcs_static[sig->param_count];
		else
			entry_func = entry_funcs_static_ret[sig->param_count];
	}
	g_assert (entry_func);

	/* Encode unbox in the lower bit of imethod */
	gpointer entry_arg = imethod;
	if (unbox)
		entry_arg = (gpointer) (((gsize) entry_arg) | 1);
	MonoFtnDesc *entry_ftndesc =
		mini_llvmonly_create_ftndesc (mono_domain_get (), entry_func, entry_arg);

	addr = mini_llvmonly_create_ftndesc (mono_domain_get (), entry_wrapper, entry_ftndesc);

	register_method_pointer (domain, addr, imethod);

	mono_memory_barrier ();
	if (unbox)
		imethod->llvmonly_unbox_entry = addr;
	else
		imethod->jit_entry = addr;

	return static_cast<MonoFtnDesc *> (addr);
}

/// Returns a function pointer that calls method through the interpreter.
///
/// \param compile  transform method now if it has not been already, so a
///     translation failure is caught here rather than on first call.
///
/// Returns NULL with the error set for a method it cannot support: one that
/// fails to transform when compile is set, and a native-to-managed wrapper on
/// a platform that has no such transition.
gpointer
interp_create_method_pointer (MonoMethod *method, gboolean compile, MonoError *error)
{
	gpointer addr, entry_func, entry_wrapper = NULL;
	MonoDomain *domain = mono_domain_get ();
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
		MonoMethodSignature *newsig = (MonoMethodSignature *) g_alloca (
			MONO_SIZEOF_METHOD_SIGNATURE + ((sig->param_count + 2) * sizeof (MonoType *)));
		memcpy (newsig, sig, mono_metadata_signature_size (sig));
		newsig->ret = m_class_get_byval_arg (mono_defaults.string_class);
		sig = newsig;
	}

	if (sig->param_count > MAX_INTERP_ENTRY_ARGS) {
		entry_func = (gpointer) mono_interp_entry_general;
	} else if (sig->hasthis) {
		if (sig->ret->type == MONO_TYPE_VOID)
			entry_func = entry_funcs_instance[sig->param_count];
		else
			entry_func = entry_funcs_instance_ret[sig->param_count];
	} else {
		if (sig->ret->type == MONO_TYPE_VOID)
			entry_func = entry_funcs_static[sig->param_count];
		else
			entry_func = entry_funcs_static_ret[sig->param_count];
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
		 * The runtime expects a function pointer unique to method, and the
		 * native caller expects a function pointer with the right signature.
		 * This case fails right away instead of trying to satisfy both.
		 */
		mono_error_set_platform_not_supported (
			error, "No native to managed transitions on this platform.");
		return NULL;
#endif
	}
#endif
	return (gpointer) interp_no_native_to_managed;
#endif

	if (mono_llvm_only) {
		/* The caller must call interp_create_method_pointer_llvmonly */
		return (gpointer) no_llvmonly_interp_method_pointer;
	}

	if (method->wrapper_type && method->wrapper_type == MONO_WRAPPER_MANAGED_TO_NATIVE)
		return imethod;

#ifndef MONO_ARCH_HAVE_FTNPTR_ARG_TRAMPOLINE
	/*
	 * Interp in wrappers get the argument in the rgctx register.
	 * MONO_ARCH_HAVE_FTNPTR_ARG_TRAMPOLINE is defined on architectures where
	 * rgctx is not scratch, so those use a separate temp register instead. The
	 * interp-in wrapper generator has not been updated for those architectures
	 * (arm).
	 */
	MonoMethod *wrapper = mini_get_interp_in_wrapper (sig);

	entry_wrapper = mono_jit_compile_method_jit_only (wrapper, error);
#endif
	if (!entry_wrapper) {
#ifndef MONO_ARCH_HAVE_INTERP_ENTRY_TRAMPOLINE
		g_assertion_message (
			"couldn't compile wrapper \"%s\" for \"%s\"",
			mono_method_get_name_full (wrapper, TRUE, TRUE, MONO_TYPE_NAME_FORMAT_IL),
			mono_method_get_name_full (method, TRUE, TRUE, MONO_TYPE_NAME_FORMAT_IL));
#else
		mono_interp_error_cleanup (error);
		if (!mono_native_to_interp_trampoline) {
			if (mono_aot_only) {
				mono_native_to_interp_trampoline =
					(MonoFuncV) mono_aot_get_trampoline ("native_to_interp_trampoline");
			} else {
				MonoTrampInfo *info;
				mono_native_to_interp_trampoline =
					(MonoFuncV) mono_arch_get_native_to_interp_trampoline (&info);
				mono_tramp_info_register (info, NULL);
			}
		}
		entry_wrapper = (gpointer) mono_native_to_interp_trampoline;
		/* We need the lmf wrapper only when being called from mixed mode */
		if (sig->pinvoke)
			entry_func = (gpointer) mono_interp_entry_from_ccontext;
		else {
			static gpointer cached_func = NULL;
			if (!cached_func) {
				cached_func = mono_jit_compile_method_jit_only (
					mini_get_interp_lmf_wrapper ("mono_interp_entry_from_trampoline",
				                                 (gpointer) mono_interp_entry_from_trampoline),
					error);
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
	 * The wrapper is called by compiled code, which does not pass the extra
	 * argument, so we pass it in the rgctx register using a trampoline.
	 */

	addr = mono_create_ftnptr_arg_trampoline (ftndesc, entry_wrapper);

	register_method_pointer (domain, addr, imethod);

	mono_memory_barrier ();
	imethod->jit_entry = addr;

	return addr;
}

} // namespace mono::interp

#ifndef __MONO_INTERP_INTERP_ENTRY_HPP__
#define __MONO_INTERP_INTERP_ENTRY_HPP__

/**
 * \file
 * \brief The address a caller outside this engine uses to reach a method.
 */

#include "interp-internals.hpp"
#include "mono/utils/mono-error-internals.h"

namespace mono::interp {

/* The address that stands for imethod, minted if this is the first ask. */
gpointer entry_for_imethod (InterpMethod *imethod, MonoError *error);

/*
 * The address that stands for imethod outside this engine, recording that the
 * address is now in native hands. A patcher writes a jump over what it is given,
 * so both engines have to name the same address for a method.
 */
gpointer escaping_entry_for_imethod (InterpMethod *imethod, MonoError *error);

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

	/*
	 * A compiled entry: the method is the one whose code that address falls in.
	 * Executing it here rather than jumping to the code keeps a single engine in
	 * charge of the frame.
	 */
	MonoJitInfo *ji = mono_jit_info_table_find_internal (
		domain, mono_get_addr_from_ftnptr (MINI_FTNPTR_TO_ADDR (addr)), TRUE, TRUE);

	if (!ji)
		return nullptr;

	/*
	 * A method's published address is its stub, and a stub is registered as a
	 * trampoline. The record still says which method it stands for, so a
	 * trampoline is an answer rather than a refusal. Some trampolines belong to
	 * no method, and those are the ones with nothing to return.
	 */
	MonoMethod *method = ji->is_trampoline ? ji->d.tramp_info->method
	                                       : mono_jit_info_get_method (ji);

	if (!method)
		return nullptr;

	return mono_interp_get_imethod (domain, method, error);
}

} // namespace mono::interp

#endif

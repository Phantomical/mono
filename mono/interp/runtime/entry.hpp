#ifndef __MONO_INTERP_INTERP_ENTRY_HPP__
#define __MONO_INTERP_INTERP_ENTRY_HPP__

/**
 * \file
 * \brief The address a caller outside this engine uses to reach a method.
 */

#include "internals.hpp"
#include "mono/utils/mono-error-internals.h"

#ifdef TARGET_WASM
G_EXTERN_C gpointer
mono_wasm_get_native_to_interp_trampoline (MonoMethod *method, gpointer extra_arg);
#endif

namespace mono::interp {

/// Holds a domain's lock for a scope.
///
/// Only for a scope the thread runs out of. An exception resume unwinds
/// interpreted frames by restoring the stack pointer over them, so a guard
/// below the resumed frame never runs its destructor and the lock stays held.
class DomainLock {
public:
	explicit DomainLock (MonoDomain *domain) : domain_ (domain) { mono_domain_lock (domain_); }
	~DomainLock () { mono_domain_unlock (domain_); }

	DomainLock (const DomainLock &) = delete;
	DomainLock &operator= (const DomainLock &) = delete;

private:
	MonoDomain *domain_;
};

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
	DomainLock lock (domain);

	if (!info->interp_method_pointer_hash)
		return nullptr;

	return static_cast<InterpMethod *> (g_hash_table_lookup (info->interp_method_pointer_hash, addr));
}

/// Records that addr is the address outside this engine for imethod, so that a
/// later arrival at addr can find the method again.
inline void
register_method_pointer (MonoDomain *domain, gpointer addr, InterpMethod *imethod)
{
	MonoJitDomainInfo *info = domain_jit_info (domain);
	DomainLock lock (domain);

	if (!info->interp_method_pointer_hash)
		info->interp_method_pointer_hash = g_hash_table_new (NULL, NULL);

	g_hash_table_insert (info->interp_method_pointer_hash, addr, imethod);
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

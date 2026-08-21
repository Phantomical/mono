#ifndef __MONO_INTERP_INTERP_ENTRY_HPP__
#define __MONO_INTERP_INTERP_ENTRY_HPP__

/**
 * \file
 * \brief The address a caller outside this engine uses to reach a method.
 */

#include "internals.hpp"
#include "mono/utils/mono-error-internals.h"

#ifdef TARGET_WASM
G_EXTERN_C gpointer mono_wasm_get_native_to_interp_trampoline (MonoMethod *method,
                                                               gpointer extra_arg);
#endif

namespace mono::interp {

/// A scoped guard on a domain's lock.
///
/// Do not hold this across a scope an exception resume can unwind. Resume
/// restores the stack pointer directly instead of running interpreted
/// frames' destructors, so a guard below the resumed frame leaves the lock
/// held for good.
class DomainLock {
public:
	explicit DomainLock (MonoDomain *domain) : domain_ (domain) { mono_domain_lock (domain_); }
	~DomainLock () { mono_domain_unlock (domain_); }

	DomainLock (const DomainLock &) = delete;
	DomainLock &operator= (const DomainLock &) = delete;

private:
	MonoDomain *domain_;
};

/// Returns the address that stands for imethod outside this engine.
///
/// A patcher writes a jump over the address it is given, so a method needs
/// one address rather than one per engine. This returns the backend's
/// stub, the same address a compiled ldftn names, and mints it without
/// compiling the method. A call that arrives at the stub lands on whichever
/// tier owns the method.
///
/// With the interpreter as the whole engine there is no backend to ask, and
/// its own entry is the only address there is.
gpointer native_entry_for_imethod (InterpMethod *imethod, MonoError *error);

/// Returns the address that stands for method outside this engine.
///
/// Named rather than resolved: an overridden method answers its own entry,
/// the address its callers already hold and which the override redirected.
/// Going through the InterpMethod would answer the replacement's, and the two
/// engines would then hand out different addresses for one method.
gpointer native_entry_for_method (MonoMethod *method, MonoDomain *domain, MonoError *error);

/// Whether addr is an address imethod is published at.
///
/// A method has one such address per engine that has handed one out: the
/// backend's stub, and the interpreter's own entry. The interpreter hands
/// out an entry of its own where it is the whole engine, or where the
/// method is entered from native code.
inline gboolean
imethod_published_at (InterpMethod *imethod, gpointer addr)
{
	/* A null addr returns false whatever is unset. That way a calli through a null
	 * function pointer is resolved again - and refused - rather than taken for
	 * the address an engine has not handed out yet. */
	return addr != nullptr
	       && (imethod->native_entry == addr || imethod->jit_entry == addr);
}

inline InterpMethod *
lookup_method_pointer (MonoDomain *domain, gpointer addr)
{
	MonoJitDomainInfo *info = domain_jit_info (domain);
	DomainLock lock (domain);

	if (!info->interp_method_pointer_hash)
		return nullptr;

	return static_cast<InterpMethod *> (
		g_hash_table_lookup (info->interp_method_pointer_hash, addr));
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

/// Returns the InterpMethod for addr, or null if no method is published
/// there. Also returns null, with the error set, when the method is known
/// but its InterpMethod cannot be built.
inline InterpMethod *
imethod_for_entry (MonoDomain *domain, gpointer addr, MonoError *error)
{
	if (InterpMethod *imethod = lookup_method_pointer (domain, addr))
		return imethod;

	/*
	 * A compiled entry: the method is whichever one's code contains that
	 * address. Resolving it here, rather than jumping straight to the code,
	 * keeps one engine in charge of the frame.
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
	MonoMethod *method =
		ji->is_trampoline ? ji->d.tramp_info->method : mono_jit_info_get_method (ji);

	if (!method)
		return nullptr;

	InterpMethod *imethod = mono_interp_get_imethod (domain, method, error);

	/*
	 * A trampoline record covers only the stub, so addr is where the
	 * method is published rather than somewhere inside a body. Remembering it
	 * here is what lets a calli site recognise the pointer next time. The
	 * address can arrive from a frame that never asked this engine for it.
	 */
	if (imethod != nullptr && ji->is_trampoline)
		imethod->native_entry = addr;

	return imethod;
}

} // namespace mono::interp

#endif

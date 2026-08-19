/**
 * \file
 * \brief What mono's C runtime sees of the per-(domain, method) record.
 *
 * The record itself is C++ - see domain-method.hpp. This is the part the
 * runtime's C files can reach.
 */

#ifndef MONO_MINI_DOMAIN_METHOD_H
#define MONO_MINI_DOMAIN_METHOD_H

#include <mono/utils/mono-publib.h>

MONO_BEGIN_DECLS

typedef struct _MonoDomain MonoDomain;
typedef struct _MonoMethod MonoMethod;

/// Asks for a method to be run by the next tier up.
///
/// Whichever engine's call count ran out calls this, and only one request goes
/// out however many of them run out at once. It returns as soon as the work is
/// queued, never once it is done.
///
/// Answers FALSE only when the request was refused and nothing will retry it,
/// which is the caller's signal to count another threshold of calls.
mono_bool mono_promote_method (MonoMethod *method, MonoDomain *domain);

/// Hands the address a method is entered at to native code at \p target.
///
/// Every caller that goes through the entry reaches \p target from here on. The
/// method never promotes again, and a compile already running for it does not
/// take the entry when it lands. A later detour on the same method is allowed
/// and replaces this one; what nothing takes back is the entry returning to a
/// tier the runtime owns.
///
/// There is no failure to report. A patcher that is told no has nothing to fall
/// back on, and a method with no record has no entry for one to hold.
void mono_install_method_detour (MonoMethod *method, MonoDomain *domain, void *target);

/// Makes \p replacement run wherever \p method was called.
///
/// Both engines are covered, which is what tells this apart from a detour. A
/// compiled caller reaches \p replacement through the entry, and an interpreted
/// one runs \p replacement's bytecode instead of \p method's. A detour only
/// covers the second where the signature can be marshalled into a call, which
/// rules out generics and more than six parameters.
///
/// A caller the interpreter transformed before this call keeps whatever it
/// copied of \p method's body. One transformed after it names \p replacement,
/// inlined or not.
///
/// A later override replaces this one. There is no failure to report.
void mono_install_method_override (MonoMethod *method, MonoDomain *domain,
                                   MonoMethod *replacement);

/// Builds the table of records a domain keeps.
///
/// Call this while the domain's jit info is being set up. The domain has no
/// records before it, and asking for one fails.
void mono_domain_method_table_init (MonoDomain *domain);

/// Drops every record the domain holds.
///
/// Call this once nothing can execute in the domain any more, and before the
/// engine's own state for the domain goes: a record names stubs carved out of
/// that state.
void mono_domain_method_table_free (MonoDomain *domain);

MONO_END_DECLS

#endif /* MONO_MINI_DOMAIN_METHOD_H */

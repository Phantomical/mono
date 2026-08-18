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

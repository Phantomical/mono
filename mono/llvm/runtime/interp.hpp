/**
 * \file
 * \brief The entries an interpreted method is reached through.
 *
 * Engine-independent: the hand-written thunk every interpreted method shares is
 * keyed only by the MonoMethod, so what it needs to find - the argument layout
 * for the prototype, and the interpreter's own handle for the method - belongs
 * to the process rather than to whichever engine compiled the caller.
 */

#ifndef MONO_LLVM_RUNTIME_INTERP_HPP
#define MONO_LLVM_RUNTIME_INTERP_HPP

#include "arch/arch.hpp"

#include <llvm/Support/Error.h>

typedef struct _MonoDomain MonoDomain;
typedef struct _MonoMethod MonoMethod;

namespace mono {

/// The entry a method is interpreted through in a domain, built on first ask.
llvm::Expected<const arch::InterpEntryPoint *> interp_entry (MonoDomain *domain,
                                                             MonoMethod *method);

/// Drop everything recorded for a domain on its way out.
void forget_interp_entries (MonoDomain *domain);

/// Drop what is recorded for a method on its way out, in every domain.
///
/// Every domain rather than the one that compiled it: a call that arrived having
/// switched domains resolves its entry against the domain it switched to, which
/// need never have published the method itself.
void forget_interp_entry (MonoMethod *method);

} // namespace mono

#endif

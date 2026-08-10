/**
 * \file
 * \brief Turning what the translator recorded into addresses the linker can
 * bind.
 */

#ifndef MONO_LLVM_RUNTIME_EXTERNALS_HPP
#define MONO_LLVM_RUNTIME_EXTERNALS_HPP

#include "method-to-llvm.hpp"

#include <llvm/ADT/STLFunctionalExtras.h>
#include <llvm/Support/Error.h>

#include <vector>

typedef struct _MonoDomain MonoDomain;
typedef struct _MonoMethod MonoMethod;

namespace mono {

class MonoJit;

/// Register an address for everything a translated module refers to, against the
/// domain the code will run as.
///
/// That domain is the owning linker's, never the thread's current one: a vtable
/// or statics address baked into this linker's code has to belong to the domain
/// the code will run as, and the two differ whenever a lazy stub fires under a
/// thread that has switched domain.
///
/// A reference to another method's code is not registered here. It is handed to
/// publish_callee, which publishes that method and defines the symbol itself -
/// the full publish rather than a bare definition, because an ldftn'd pointer
/// escapes into delegates and recovering the method from one needs the jit info
/// that publishing registers.
///
/// Called with no engine lock held. Laying out a class to create its vtable is
/// the last place metadata gets loaded, so a callee whose class will not load
/// fails here rather than during translation.
llvm::Error resolve_externals (MonoJit &jit, MonoDomain *domain,
                               const std::vector<ExternalSymbol> &externals,
                               llvm::function_ref<llvm::Error (MonoMethod *)> publish_callee);

} // namespace mono

#endif

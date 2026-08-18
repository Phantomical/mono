/**
 * \file
 * \brief Turning what the translator recorded into addresses the linker can
 * bind.
 */

#ifndef MONO_LLVM_RUNTIME_EXTERNALS_HPP
#define MONO_LLVM_RUNTIME_EXTERNALS_HPP

#include "method-to-llvm.hpp"

#include <llvm/ADT/STLFunctionalExtras.h>
#include <llvm/ADT/StringRef.h>
#include <llvm/Support/Error.h>

#include <utility>
#include <vector>

typedef struct _MonoDomain MonoDomain;
typedef struct _MonoMethod MonoMethod;

namespace mono {

class MonoJit;
class MonoDomainMethod;

/// Register an address for everything a translated module refers to, against the
/// domain the code will run as.
///
/// That domain is the owning linker's, never the thread's current one: a vtable
/// or statics address baked into this linker's code has to belong to the domain
/// the code will run as, and the two differ whenever a lazy stub fires under a
/// thread that has switched domain.
///
/// A reference to another method's code is handed to publish_callee, which
/// publishes that method - the full publish rather than a bare definition,
/// because an ldftn'd pointer escapes into delegates and recovering the method
/// from one needs the jit info that publishing registers - and its published
/// name and stub address are appended to module_symbols rather than registered
/// here: the caller's module is not linked yet, so there is nowhere to define
/// them into until compile () creates it.
///
/// Called with no engine lock held. Laying out a class to create its vtable is
/// the last place metadata gets loaded, so a callee whose class will not load
/// fails here rather than during translation.
llvm::Error resolve_externals (MonoJit &jit, MonoDomain *domain,
                               const std::vector<ExternalSymbol> &externals,
                               llvm::function_ref<llvm::Expected<MonoDomainMethod *> (MonoMethod *)> publish_callee,
                               std::vector<std::pair<llvm::StringRef, void *>> &module_symbols);

} // namespace mono

#endif

#ifndef MONO_LLVM_RUNTIME_EXTERNALS_HPP
#define MONO_LLVM_RUNTIME_EXTERNALS_HPP

#include "method-to-llvm.hpp"

#include <llvm/ADT/ArrayRef.h>
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

/// Registers an address for everything a translated module refers to, against
/// the domain the code will run as.
///
/// \p domain is the owning linker's, never the thread's current one. A vtable
/// or statics address baked into this linker's code must match the domain the
/// code runs in. The two differ when a lazy stub fires after its thread
/// switched domains.
///
/// A Code external's method is handed to publish_callee, which does the full
/// publish rather than a bare definition. An ldftn'd pointer escapes into
/// delegates, and recovering the method from one needs the jit info a full
/// publish registers.
///
/// The callee's published name and stub address are appended to
/// module_symbols instead of being registered directly here. The caller's
/// module is not linked yet, so there is nowhere to define them until
/// compile () creates it.
///
/// Called with no engine lock held. Laying out a class to create its vtable
/// is the last place metadata gets loaded. A callee whose class will not
/// load therefore fails here, not during translation.
llvm::Error resolve_externals (MonoJit &jit, MonoDomain *domain,
                               llvm::ArrayRef<ExternalSymbol> externals,
                               llvm::function_ref<llvm::Expected<MonoDomainMethod *> (MonoMethod *)> publish_callee,
                               std::vector<std::pair<llvm::StringRef, void *>> &module_symbols);

} // namespace mono

#endif

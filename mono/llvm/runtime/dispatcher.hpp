#ifndef MONO_LLVM_RUNTIME_DISPATCHER_HPP
#define MONO_LLVM_RUNTIME_DISPATCHER_HPP

#include "thrower.hpp"

#include <llvm/Support/Error.h>

typedef struct _MonoDomain MonoDomain;
typedef struct _MonoMethod MonoMethod;

namespace mono {

class MonoJit;

/// Whether a method's body can be bound straight into a stub, rather than
/// reached through a dispatcher.
///
/// A stub is only reachable from its own domain's code, but the thread that
/// fires one need not be running as that domain. AppDomain:InvokeInDomain
/// switches the domain and then calls, so the first caller through a stub can
/// arrive from the far side of that switch. Binding then welds one domain's copy
/// into another domain's code - wrong statics and vtables while it runs, and a
/// dangling call once the bound domain unloads. Methods with no body of their
/// own bind directly: what stands behind them is mini's, one copy for the whole
/// process.
bool bindable (MonoDomain *owner, MonoMethod *method);

/// Builds the per-call dispatcher for a method. The dispatcher is a function
/// of the body's exact prototype. It asks for the current domain's body on
/// every call and tails into it, instead of baking one domain's copy into
/// another's code.
llvm::Expected<void *> build_dispatcher (MonoJit &jit, MonoDomain *domain,
                                         MonoMethod *method, RememberFn remember);

} // namespace mono

#endif

/**
 * \file
 * \brief The callees a compile folds in without costing them.
 */

#ifndef MONO_LLVM_RUNTIME_TRIVIAL_INLINES_HPP
#define MONO_LLVM_RUNTIME_TRIVIAL_INLINES_HPP

#include "inline-scope.hpp"
#include "method-to-llvm.hpp"

#include <vector>

typedef struct _MonoDomain MonoDomain;
typedef struct _MonoMethod MonoMethod;

namespace llvm {
class Function;
class Module;
} // namespace llvm

namespace mono {

/// Translate into a body's own module each callee whose IL already says the
/// inline pays, and mark it always-inline for the pipeline to fold in.
///
/// These shapes have nothing to weigh: a constant, a chain of field accesses,
/// one forward to another method, a throw, and an object made and returned. A
/// callee outside them keeps its call, and so does one that will not translate.
/// A candidate declined costs the caller nothing.
///
/// The methods scope already names stay reachable through their thunks, so a
/// materialized body that calls one of them reaches its published entry. This
/// walks the chain under body, so a forwarder that forwards to a forwarder
/// collapses in one call.
///
/// scope.defined must already name every method the module holds a body for.
/// A module built for a batch holds several, and each is declared to the others
/// under a name of its own that no body can be built into. A member left out of
/// scope.defined is folded in a second time, as a copy nothing calls.
///
/// externals collects what the new bodies name, so resolve them after this
/// rather than before it. types is the module's struct-type cache, which every
/// translation into one module shares.
void materialize_trivial_callees (llvm::Module &module, MonoDomain *domain,
                                  MonoMethod *root, llvm::Function &body,
                                  std::vector<ExternalSymbol> &externals,
                                  ModuleTypes &types, InlineScope &scope);

} // namespace mono

#endif

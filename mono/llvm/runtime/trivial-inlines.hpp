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
/// A copy belongs to this compile, and only the calls in body move onto it. What
/// one method folds in therefore does not depend on what else the module holds.
/// That is what lets a batched tier-1 body and the tier-2 body behind it hash the
/// same CFG. This walks the chain under body, so a forwarder that forwards to a
/// forwarder collapses in one call.
///
/// scope.folded must already name root, or the compile folds root into its own
/// callee. scope.defined must name every method the module publishes a body
/// for, so that a copy calling one of them reaches its entry rather than the
/// body beside it.
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

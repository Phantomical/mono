/**
 * \file
 * \brief The callees a compile folds in without costing them.
 */

#ifndef MONO_LLVM_RUNTIME_TRIVIAL_INLINES_HPP
#define MONO_LLVM_RUNTIME_TRIVIAL_INLINES_HPP

#include "inline-scope.hpp"
#include "method-to-llvm.hpp"

#include <llvm/ADT/ArrayRef.h>
#include <llvm/ADT/STLFunctionalExtras.h>
#include <llvm/Support/Error.h>

#include <vector>

typedef struct _MonoDomain MonoDomain;
typedef struct _MonoMethod MonoMethod;

namespace llvm {
class Function;
class Module;
} // namespace llvm

namespace mono {

/// Resolves the addresses a newly translated body names, and reports what the
/// runtime cannot resolve.
using ResolveExternals = llvm::function_ref<llvm::Error (llvm::ArrayRef<ExternalSymbol>)>;

/// Translate into a body's own module each callee whose IL already says the
/// inline pays, and mark it always-inline for the pipeline to fold in.
///
/// These shapes have nothing to weigh: a constant, a chain of field accesses,
/// one forward to another method, a throw, and an object made and returned. A
/// callee outside them keeps its call, and so does one that will not translate.
/// A candidate declined costs the caller nothing.
///
/// This walks the chain under body, so a forwarder that forwards to a
/// forwarder collapses in one call.
///
/// scope.folded must already name root, or the compile folds root into its own
/// callee. scope.defined must name every method the module publishes a body
/// for, so that a copy calling one of them reaches its entry rather than the
/// body beside it.
///
/// externals collects what the new bodies name. resolve is asked about each
/// copy's own share of it while that copy is built, so the caller resolves
/// only what it recorded before this call. types is the module's struct-type
/// cache, which every translation into one module shares.
void materialize_trivial_callees (llvm::Module &module, MonoDomain *domain,
                                  MonoMethod *root, llvm::Function &body,
                                  std::vector<ExternalSymbol> &externals,
                                  ModuleTypes &types, InlineScope &scope,
                                  ResolveExternals resolve);

} // namespace mono

#endif

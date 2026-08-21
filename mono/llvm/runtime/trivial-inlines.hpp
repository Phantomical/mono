/**
 * \file
 * \brief The callees a tier-2 compile folds in without costing them.
 */

#ifndef MONO_LLVM_RUNTIME_TRIVIAL_INLINES_HPP
#define MONO_LLVM_RUNTIME_TRIVIAL_INLINES_HPP

#include "method-to-llvm.hpp"

#include <llvm/Support/Error.h>

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
/// The methods the module already defines stay reachable through their thunks,
/// so a materialized body that calls one of them reaches its published entry.
///
/// externals collects what the new bodies name, so resolve them after this
/// rather than before it. types is the module's struct-type cache, which every
/// translation into one module shares.
void materialize_trivial_callees (llvm::Module &module, MonoDomain *domain,
                                  MonoMethod *root, llvm::Function &body,
                                  std::vector<ExternalSymbol> &externals,
                                  ModuleTypes &types);

/// Answers an error naming a materialized body the pipeline did not fold in.
///
/// Such a body is entered by a direct call, so it sits off its method's thunk
/// and has no jit info of its own. A stack walk over its frame finds nothing,
/// and a later detour misses it. So the compile that produced it must fail,
/// which at tier 2 leaves the method on the tier it already runs at.
llvm::Error trivial_inlines_landed (const llvm::Module &module);

} // namespace mono

#endif

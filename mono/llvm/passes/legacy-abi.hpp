/**
 * \file
 * \brief Lowering calls that cross into code compiled by mini.
 *
 * Generated code speaks fastcc with every value in its natural IR type; only
 * the boundary with the rest of the runtime - code mini compiled, raw C entry
 * points, and every function pointer the runtime hands out - still speaks
 * mini's amd64 convention. The translator marks those boundary calls with the
 * `mono-legacycc` attribute and emits them naturally; LegacyAbiPass rewrites
 * them into the legacy convention after the optimization pipeline has run, so
 * nothing upstream of it ever sees a lowered call.
 *
 * Everything here classifies from IR types and the DataLayout alone - the
 * translator emits value types with their real field layout (padding spelled
 * as [n x i1], which no real field ever is), which is exactly what makes the
 * classification expressible without asking the runtime anything.
 */

#ifndef MONO_LLVM_PASSES_LEGACY_ABI_HPP
#define MONO_LLVM_PASSES_LEGACY_ABI_HPP

#include <llvm/ADT/StringRef.h>
#include <llvm/IR/PassManager.h>

namespace mono {

/// The attribute naming a call (or a declaration every call to which) crosses
/// the legacy boundary. Its value is one of the flavor strings below.
constexpr llvm::StringRef legacy_cc_attribute = "mono-legacycc";

/// Which side of the boundary the callee is, and where mini's convention puts
/// a hidden return pointer if the return needs one.
enum class LegacyFlavor {
	/// Code compiled by mini for a managed signature: value types ride the
	/// integer file, a big return travels through a pointer at argument 0.
	Managed,
	/// Managed, but the hidden return pointer sits at argument 1: the first
	/// argument is a this (or a reference the trampolines treat as one) that
	/// the runtime insists on finding in the first register.
	ManagedVret1,
	/// A native C function: the SysV classification, floats in the SSE file.
	Pinvoke,
};

llvm::StringRef legacy_flavor_value (LegacyFlavor flavor);

/// Rewrites every `mono-legacycc` call into mini's convention. Runs after the
/// optimization pipeline; the marked calls are opaque to it either way, but
/// the natural-typed IR is what it should be optimizing.
class LegacyAbiPass : public llvm::PassInfoMixin<LegacyAbiPass> {
public:
	llvm::PreservedAnalyses run (llvm::Module &m,
	                             llvm::ModuleAnalysisManager &mam);
};

/// Create NAME in M: a legacy-convention entry point that unpacks its
/// arguments out of the convention into natural values and calls TARGET (a
/// fastcc declaration in M) with them. This is what the runtime publishes for
/// a method - every caller that is not generated code enters through it.
llvm::Function *create_legacy_entry_thunk (llvm::Module &m, llvm::StringRef name,
                                           llvm::Function *target,
                                           LegacyFlavor flavor);

} // namespace mono

#endif

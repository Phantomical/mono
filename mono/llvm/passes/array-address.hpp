/**
 * \file
 * \brief Lowering array element addressing the translator left symbolic.
 *
 * Get/Set/Address on an array class have no IL body: mini expanded them at
 * every call site, and the marshal wrapper the runtime offers instead calls
 * the accessor again. The translator emits the bounds-checked address
 * computation as a call to a `mono.array.address.*` declaration and does the
 * load or store itself. This pass rewrites each such call into the inline
 * arithmetic, so the optimizer sees it and nothing symbolic survives to the
 * linker.
 */

#ifndef MONO_LLVM_PASSES_ARRAY_ADDRESS_HPP
#define MONO_LLVM_PASSES_ARRAY_ADDRESS_HPP

#include <llvm/ADT/StringRef.h>
#include <llvm/IR/PassManager.h>

namespace mono {

/// The name prefix of the address declarations, and the attribute on each
/// carrying the numbers the lowering needs (element size, array layout
/// offsets, the bounds-failure exception token) as `key=value` pairs. Only
/// the translator writes either.
constexpr llvm::StringRef array_address_prefix = "mono.array.address.";
constexpr llvm::StringRef array_address_attribute = "mono-array-address";

/// Rewrites every call to a `mono.array.address.*` declaration into the
/// bounds-checked element address computation, throwing the corlib exception
/// named in the declaration's attribute on a failed check. Runs before the
/// optimization pipeline.
class ArrayAddressPass : public llvm::PassInfoMixin<ArrayAddressPass> {
public:
	llvm::PreservedAnalyses run (llvm::Module &m,
	                             llvm::ModuleAnalysisManager &mam);
};

} // namespace mono

#endif

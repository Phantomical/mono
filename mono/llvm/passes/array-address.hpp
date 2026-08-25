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
#include <llvm/IR/Instructions.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Metadata.h>
#include <llvm/IR/PassManager.h>

namespace mono {

/// The name prefix of the address declarations, and the attribute on each
/// carrying what no header states — the rank, the element size, and whether
/// the array carries a bounds vector — as `key=value` pairs. Only the
/// translator writes either.
constexpr llvm::StringRef array_address_prefix = "mono.array.address.";
constexpr llvm::StringRef array_address_attribute = "mono-array-address";

/// Marks a read of the array header as `!invariant.load`.
///
/// The header is the `bounds` pointer, `max_length`, and the length and the
/// lower bound of each dimension. `mono_gc_alloc_vector ()` and
/// `mono_gc_alloc_array ()` write the first two, and
/// `mono_array_new_full_checked ()` fills the bounds vector. Each write happens
/// before managed code can reach the array. A bounds check can then keep the
/// value it read across a store to any managed field.
///
/// When it moves an array, SGen writes the `bounds` pointer again, because the
/// bounds vector sits inside the object. It writes that pointer in the copy
/// (`sgen_client_update_copied_object ()`). No `thread_mark_func` is
/// registered, so the stack scan is conservative and an array a compiled frame
/// holds is pinned rather than moved.
///
/// The tag does not make the load speculatable. The load case of
/// `isSafeToSpeculativelyExecute ()` asks for a dereferenceable pointer and
/// never reads `!invariant.load`, so each load stays under the null check on
/// the array.
inline void
mark_array_header_load (llvm::LoadInst *load)
{
	load->setMetadata (llvm::LLVMContext::MD_invariant_load,
	                   llvm::MDNode::get (load->getContext (), {}));
}

/// Rewrites every call to a `mono.array.address.*` declaration into the
/// bounds-checked element address computation, throwing the corlib exception
/// the site names on a failed check. Runs before the optimization pipeline.
class ArrayAddressPass : public llvm::PassInfoMixin<ArrayAddressPass> {
public:
	llvm::PreservedAnalyses run (llvm::Module &m,
	                             llvm::ModuleAnalysisManager &mam);
};

} // namespace mono

#endif

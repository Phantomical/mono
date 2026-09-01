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

#include "../runtime/options.hpp"

#include <llvm/ADT/StringRef.h>
#include <llvm/IR/DerivedTypes.h>
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

/// Marks a read of the array header as `!invariant.group`, unless it is a
/// non-pointer read and `tag_non_pointer_invariant_group ()` is off.
///
/// The header is the `bounds` pointer, `max_length`, and the length and the
/// lower bound of each dimension. `mono_gc_alloc_vector ()` and
/// `mono_gc_alloc_array ()` write the first two, and
/// `mono_array_new_full_checked ()` fills the bounds vector, all before managed
/// code can reach the array.
///
/// SGen re-writes the `bounds` pointer when it copies an array, but no
/// `thread_mark_func` is registered, so a compiled frame's array is pinned
/// rather than moved.
///
/// The tag does not make the load speculatable. The load case of
/// `isSafeToSpeculativelyExecute ()` asks for a dereferenceable pointer and
/// reads neither invariant tag, so the load stays under the null check on the
/// array.
inline void
mark_array_header_load (llvm::LoadInst *load)
{
	if (!load->getType ()->isPointerTy () && !tag_non_pointer_invariant_group ())
		return;

	load->setMetadata (llvm::LLVMContext::MD_invariant_group,
	                   llvm::MDNode::get (load->getContext (), {}));
}

/// Rewrites every call to a `mono.array.address.*` declaration into the
/// bounds-checked element address computation, and says whether it changed
/// anything. A failed check throws the corlib exception the site names.
bool lower_array_addresses (llvm::Module &m);

} // namespace mono

#endif

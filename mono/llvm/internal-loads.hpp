/**
 * \file
 * \brief TBAA metadata for loads and stores of runtime-owned memory.
 *
 * These locations are not named by IL stores, so they have leaves beside the
 * managed-memory leaves. Untagged raw-pointer stores still alias them.
 */

#ifndef MONO_LLVM_INTERNAL_LOADS_HPP
#define MONO_LLVM_INTERNAL_LOADS_HPP

#include <llvm/ADT/StringRef.h>
#include <llvm/IR/Instructions.h>

namespace mono {

/// Common root for managed and runtime-owned memory leaves.
constexpr llvm::StringRef managed_memory_tbaa_root = "mono managed memory";

/// Object header fields: vtable, lock, and string length.
constexpr llvm::StringRef object_header_tbaa_leaf = "mono object header";

/// The array header, which is the rest of that object for an array.
constexpr llvm::StringRef array_header_tbaa_leaf = "mono array header";

/// MonoVTable fields and interface bitmap (not patchable vtable slots).
constexpr llvm::StringRef vtable_tbaa_leaf = "mono vtable";

/// MonoClass fields and its supertypes array.
constexpr llvm::StringRef class_tbaa_leaf = "mono class";

/// Lifetime of the value read by an internal load.
enum class InternalLife {
	/// The program can change it, so the load takes no invariance at all.
	varies,
	/// Written once per object; use invariant.group where applicable.
	per_object,
	/// Written before compiled code can name the non-heap structure.
	fixed,
};

/// Adds the `!tbaa` leaf and any invariance allowed by \p life.
llvm::LoadInst *mark_internal_load (llvm::LoadInst *load, llvm::StringRef leaf,
                                    InternalLife life);

/// Adds the `!tbaa` leaf \p leaf to a runtime-owned store.
llvm::StoreInst *mark_internal_store (llvm::StoreInst *store, llvm::StringRef leaf);

} // namespace mono

#endif

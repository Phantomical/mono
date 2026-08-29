/**
 * \file
 * \brief Marks a vtable symbol with what its class settles, and reads it back.
 *
 * A vtable is an external symbol, so the optimizer can read no field behind it.
 * What the class settles on its own goes beside the symbol instead.
 */

#ifndef MONO_LLVM_ANALYSIS_VTABLE_INFO_HPP
#define MONO_LLVM_ANALYSIS_VTABLE_INFO_HPP

#include <llvm/ADT/StringRef.h>

#include <cstdint>
#include <optional>

namespace llvm {
class Constant;
class GlobalObject;
} // namespace llvm

typedef struct _MonoClass MonoClass;

namespace mono {

/// The fields `mono_class_create_runtime_vtable ()` takes off the class alone.
struct VTableInfo {
	/// The `mono_class_` symbol for the class the vtable stands for.
	llvm::Constant *klass = nullptr;

	/// The `System.Type` object for that class.
	llvm::Constant *type = nullptr;

	uint8_t rank = 0;
};

constexpr llvm::StringRef vtable_info_metadata = "mono.vtable";

void mark_vtable_info (llvm::GlobalObject &vtable, const VTableInfo &info);

/// Returns what \p vtable was marked with, or nothing where it carries no mark.
///
/// Every field of the answer is set. The translator marks nothing where it
/// cannot state them all.
std::optional<VTableInfo> vtable_info (const llvm::GlobalObject &vtable);

} // namespace mono

#endif

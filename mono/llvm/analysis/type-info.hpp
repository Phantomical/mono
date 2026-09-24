/**
 * \file
 * \brief Stores compile-time information about a `System.Type` symbol.
 *
 * For enum types, this includes the symbols needed to fold type operations.
 */

#ifndef MONO_LLVM_ANALYSIS_TYPE_INFO_HPP
#define MONO_LLVM_ANALYSIS_TYPE_INFO_HPP

#include <llvm/ADT/StringRef.h>

#include <cstdint>
#include <optional>

namespace llvm {
class Constant;
class Function;
class GlobalObject;
} // namespace llvm

typedef struct _MonoClass MonoClass;
typedef struct _MonoType MonoType;

namespace mono {

/// Information needed to emit a `mono.alloc.object` call.
struct ObjectAlloc {
	llvm::Constant *vtable = nullptr;
	int32_t size = 0;
	llvm::Function *allocator = nullptr;

	/// Whether the allocation takes the declaration LLVM erases once nothing
	/// reads the object.
	bool erasable = false;

	/// Whether the allocator raises rather than returns null.
	bool raises = false;
};

/// What a `System.Type` object's own type settles.
struct TypeInfo {
	MonoType *type = nullptr;

	/// The class of the object itself: System.RuntimeType, or a
	/// Reflection.Emit builder's.
	MonoClass *object_class = nullptr;

	/// For an enum type, and null otherwise: the `System.Type` objects of its
	/// underlying type and of System.Enum, which is its base type.
	llvm::Constant *underlying = nullptr;
	llvm::Constant *base = nullptr;

	/// For an enum type: how to allocate a boxed instance. A null vtable where
	/// the compile cannot name one.
	ObjectAlloc box;
};

constexpr llvm::StringRef type_info_metadata = "mono.type";

void mark_type_info (llvm::GlobalObject &type_object, const TypeInfo &info);

/// Returns what \p type_object was marked with, or nothing where it carries no
/// mark.
std::optional<TypeInfo> type_info (const llvm::GlobalObject &type_object);

} // namespace mono

#endif

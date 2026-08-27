/**
 * \file
 * \brief What a compile knows about a class's vtable without reading one.
 *
 * A vtable is an external symbol, so every field behind it is memory the
 * optimizer cannot read. The translator writes the fields that come off the
 * class alone beside the symbol instead. A fold reads them back once the IR
 * settles which vtable a site names.
 *
 * The values are symbols rather than addresses, so a comparison against the
 * same symbol elsewhere folds.
 */

#ifndef MONO_LLVM_VTABLE_FACTS_HPP
#define MONO_LLVM_VTABLE_FACTS_HPP

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
struct VTableFacts {
	/// The `mono_class_` symbol for the class the vtable stands for.
	llvm::Constant *klass = nullptr;

	/// The `System.Type` object for that class, which
	/// `mono_type_get_object_checked ()` allocates pinned.
	llvm::Constant *type = nullptr;

	uint8_t rank = 0;
};

/// Names the metadata a marked vtable symbol carries its facts in.
constexpr llvm::StringRef vtable_facts_metadata = "mono.vtable.facts";

/// Says what \p vtable's own class settles about it.
void mark_vtable_facts (llvm::GlobalObject &vtable, const VTableFacts &facts);

/// What \p vtable was marked with, or nothing where it carries no mark.
///
/// A class the translator could not state every field of gets no mark at all,
/// so a reader never sees a hole.
std::optional<VTableFacts> vtable_facts (const llvm::GlobalObject &vtable);

} // namespace mono

#endif

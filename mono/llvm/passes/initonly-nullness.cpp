#include "initonly-nullness.hpp"

#include "analysis/operand-class.hpp"
#include "class-init-elision.hpp"
#include "compile-state.hpp"

#include "mono/metadata/class-internals.h"
#include "mono/metadata/object-internals.h"

#include <llvm/ADT/SmallVector.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Metadata.h>

#include <cstdint>
#include <optional>

using namespace llvm;

namespace mono {
namespace {

/// Whether the reference in \p field is null. Returns std::nullopt if the
/// field is not a reference or its declaring class has not finished
/// initializing.
std::optional<bool>
initialized_reference_is_null (MonoDomain *domain, MonoClassField *field, int64_t offset)
{
	if (!MONO_TYPE_IS_REFERENCE (mono_field_get_type_internal (field)))
		return std::nullopt;

	MonoClass *klass = field->parent;
	if (!is_class_initialized (domain, klass))
		return std::nullopt;

	MonoVTable *vtable = mono_class_try_get_vtable (domain, klass);
	auto *held = *(MonoObject **) ((char *) mono_vtable_get_static_field_data (vtable) + offset);

	return held == nullptr;
}

} // namespace

PreservedAnalyses
InitonlyNullnessPass::run (Function &f, FunctionAnalysisManager &)
{
	MonoDomain *domain = current_compile ().domain;
	if (domain == nullptr)
		return PreservedAnalyses::all ();

	SmallVector<LoadInst *, 8> known_null;
	bool changed = false;

	for (Instruction &i : instructions (f)) {
		auto *load = dyn_cast<LoadInst> (&i);
		if (load == nullptr || !load->getType ()->isPointerTy ())
			continue;

		auto [field, offset] = initonly_static_field (load);
		if (field == nullptr)
			continue;

		std::optional<bool> is_null = initialized_reference_is_null (domain, field, offset);
		if (!is_null)
			continue;

		if (*is_null) {
			known_null.push_back (load);
		} else {
			load->setMetadata (LLVMContext::MD_nonnull, MDNode::get (f.getContext (), {}));
			changed = true;
		}
	}

	for (LoadInst *load : known_null) {
		load->replaceAllUsesWith (ConstantPointerNull::get (cast<PointerType> (load->getType ())));
		load->eraseFromParent ();
	}

	return (changed || !known_null.empty ()) ? PreservedAnalyses::none () : PreservedAnalyses::all ();
}

} // namespace mono

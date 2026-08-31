#include "static-const-fold.hpp"

#include "analysis/operand-class.hpp"
#include "class-init-warm.hpp"
#include "compile-state.hpp"

#include <llvm/ADT/APFloat.h>
#include <llvm/ADT/APInt.h>
#include <llvm/ADT/SmallVector.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Type.h>

#include <cstdint>
#include <cstring>

#include "mono/metadata/class-internals.h"
#include "mono/metadata/object-internals.h"

using namespace llvm;

namespace mono {
namespace {

/// The constant \p bytes holds when read as \p type, or null where \p type is
/// not one this can build a constant for.
///
/// \p type is the load's own type rather than anything read off MonoType: a
/// field held in memory - a value type bigger than a register - reaches its
/// caller through a memcpy, not a load, and never reaches here at all.
Constant *
constant_of (Type *type, const char *bytes)
{
	if (auto *ints = dyn_cast<IntegerType> (type)) {
		unsigned width = ints->getBitWidth ();

		if (width > 64)
			return nullptr;

		uint64_t raw = 0;

		memcpy (&raw, bytes, (width + 7) / 8);
		return ConstantInt::get (ints, raw);
	}

	if (type->isFloatTy ()) {
		float raw;

		memcpy (&raw, bytes, sizeof (raw));
		return ConstantFP::get (type, raw);
	}

	if (type->isDoubleTy ()) {
		double raw;

		memcpy (&raw, bytes, sizeof (raw));
		return ConstantFP::get (type, raw);
	}

	return nullptr;
}

/// The constant load reads, or null where this compile cannot state one: the
/// field is not both initonly and a scalar, or its class is not yet warm.
///
/// A reference field is left alone. Its value is safe from the class
/// initializer once read - `initonly_static_value ()`'s own subject - but not
/// from the collector, which is free to move what it names between this
/// compile and the object's every later use.
Constant *
warm_static_constant (const LoadInst &load)
{
	MonoClassField *field = initonly_static_field (&load);

	if (field == nullptr || MONO_TYPE_IS_REFERENCE (mono_field_get_type_internal (field)))
		return nullptr;

	MonoDomain *domain = current_compile ().domain;
	MonoClass *klass = field->parent;

	if (!class_is_initialized (domain, klass))
		return nullptr;

	MonoVTable *vtable = mono_class_try_get_vtable (domain, klass);
	const char *bytes =
		(const char *) mono_vtable_get_static_field_data (vtable) + field->offset;

	return constant_of (load.getType (), bytes);
}

} // namespace

PreservedAnalyses
StaticConstFoldPass::run (Function &f, FunctionAnalysisManager &)
{
	if (current_compile ().domain == nullptr)
		return PreservedAnalyses::all ();

	SmallVector<std::pair<LoadInst *, Constant *>, 4> found;

	for (Instruction &i : instructions (f))
		if (auto *load = dyn_cast<LoadInst> (&i))
			if (Constant *value = warm_static_constant (*load))
				found.push_back ({ load, value });

	if (found.empty ())
		return PreservedAnalyses::all ();

	for (auto &[load, value] : found) {
		load->replaceAllUsesWith (value);
		load->eraseFromParent ();
	}

	return PreservedAnalyses::none ();
}

} // namespace mono

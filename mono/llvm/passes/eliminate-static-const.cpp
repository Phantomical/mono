#include "eliminate-static-const.hpp"

#include "analysis/operand-class.hpp"
#include "class-init-elision.hpp"
#include "compile-state.hpp"
#include "method-symbols.hpp"

#include <llvm/ADT/APFloat.h>
#include <llvm/ADT/APInt.h>
#include <llvm/ADT/SmallVector.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DataLayout.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/GlobalValue.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Type.h>

#include <cstdint>
#include <cstring>

#include "mono/metadata/abi-details.h"
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

/// Whether a value of \p type has a GC reference anywhere in it.
bool
holds_a_reference (MonoType *type)
{
	if (MONO_TYPE_IS_REFERENCE (type))
		return true;

	MonoClass *klass = mono_class_from_mono_type_internal (type);

	return klass != nullptr && m_class_is_valuetype (klass)
	       && m_class_has_references (klass);
}

/// The constant load reads, or null where this compile cannot state one: the
/// field is not both initonly and a scalar, or its class is not yet warm.
///
/// A field with a reference among its bytes is left alone. Its value is safe
/// from the class initializer once read - `initonly_static_value ()`'s own
/// subject - but not from the collector, which is free to move what it names
/// between this compile and the object's every later use.
///
/// Asking that of the field's whole type rather than of the bytes this load
/// covers is what keeps the answer right for a value type: a struct wrapping
/// one reference, `ImmutableArray<T>`'s shape, is not itself a reference type,
/// though a body that only copies it on - returning it, storing it, passing
/// it - leaves SROA a plain scalar for `constant_of ()` to fold. What it folds
/// is the address the object had while this compiled, and nothing rewrites an
/// immediate when the object moves.
Constant *
warm_static_constant (const LoadInst &load)
{
	auto [field, offset] = initonly_static_field (&load);

	if (field == nullptr || holds_a_reference (mono_field_get_type_internal (field)))
		return nullptr;

	MonoDomain *domain = current_compile ().domain;
	MonoClass *klass = field->parent;

	if (!is_class_initialized (domain, klass))
		return nullptr;

	MonoVTable *vtable = mono_class_try_get_vtable (domain, klass);
	// offset, not field->offset: a load into a struct-typed field's member
	// reads that member's own byte, not the struct's first one.
	const char *bytes = (const char *) mono_vtable_get_static_field_data (vtable) + offset;

	return constant_of (load.getType (), bytes);
}

/// The length \p load reads off an interned string literal, or null where
/// \p load is not a load of `MonoString::length` off a symbol `emit_ldstr ()`
/// marked.
///
/// Unlike a static field, this waits on no class warmth. `emit_ldstr ()`
/// already interned the literal, and interning fixes its length for the
/// object's whole life.
Constant *
ldstr_length_constant (const LoadInst &load)
{
	const DataLayout &layout = load.getModule ()->getDataLayout ();
	const Value *address = load.getPointerOperand ();
	APInt offset (layout.getIndexTypeSizeInBits (address->getType ()), 0);
	const auto *block = dyn_cast<GlobalValue> (address->stripAndAccumulateConstantOffsets (
		layout, offset, /*AllowNonInbounds=*/true));

	if (block == nullptr || offset.isNegative () || !offset.isSignedIntN (32)
	    || offset.getSExtValue () != MONO_STRUCT_OFFSET (MonoString, length))
		return nullptr;

	MonoString *literal = get_ldstr (*block);

	if (literal == nullptr)
		return nullptr;

	return constant_of (load.getType (), (const char *) literal + offset.getSExtValue ());
}

} // namespace

PreservedAnalyses
EliminateStaticConstPass::run (Function &f, FunctionAnalysisManager &)
{
	if (current_compile ().domain == nullptr)
		return PreservedAnalyses::all ();

	SmallVector<std::pair<LoadInst *, Constant *>, 4> found;

	for (Instruction &i : instructions (f))
		if (auto *load = dyn_cast<LoadInst> (&i)) {
			if (Constant *value = warm_static_constant (*load))
				found.push_back ({ load, value });
			else if (Constant *value = ldstr_length_constant (*load))
				found.push_back ({ load, value });
		}

	if (found.empty ())
		return PreservedAnalyses::all ();

	for (auto &[load, value] : found) {
		load->replaceAllUsesWith (value);
		load->eraseFromParent ();
	}

	return PreservedAnalyses::none ();
}

} // namespace mono

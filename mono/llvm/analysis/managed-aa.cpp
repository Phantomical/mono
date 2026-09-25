#include "managed-aa.hpp"

#include "internal-loads.hpp"
#include "method-symbols.hpp"
#include "operand-class.hpp"

#include "mono/metadata/class-inlines.h"
#include "mono/metadata/class-internals.h"
#include "mono/metadata/metadata.h"

#include <llvm/ADT/APInt.h>
#include <llvm/IR/Argument.h>
#include <llvm/IR/DataLayout.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/GlobalValue.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Metadata.h>
#include <llvm/IR/Module.h>

#include <cstdint>

using namespace llvm;

namespace mono {

AnalysisKey ManagedAA::Key;

namespace {

/// What a location's base is known to be.
struct Base {
	enum class Kind {
		unknown,
		/// A class's statics block.
		statics,
		/// A managed object, or null.
		heap,
	};

	Kind kind = Kind::unknown;
	/// For a heap base, a class every object it can be is an instance of, or
	/// null where the IR states none.
	MonoClass *bound = nullptr;
};

/// Returns the statics field containing \p address and sets \p offset.
MonoClassField *
static_field_addressed (const Value *address, const DataLayout &layout, int64_t &offset)
{
	APInt at (layout.getIndexTypeSizeInBits (address->getType ()), 0);
	const auto *block = dyn_cast<GlobalValue> (
		address->stripAndAccumulateConstantOffsets (layout, at, /*AllowNonInbounds=*/true));

	if (block == nullptr || at.isNegative () || !at.isSignedIntN (32))
		return nullptr;

	MonoClass *klass = get_statics_class (*block);

	if (klass == nullptr)
		return nullptr;

	offset = at.getSExtValue ();
	MonoClassField *field = static_field_at (klass, static_cast<int> (offset));

	// A special static has no storage in the block.
	return field != nullptr && m_field_get_offset (field) >= 0 ? field : nullptr;
}

bool
loads_reference (const LoadInst &load)
{
	const MDNode *tag = load.getMetadata (LLVMContext::MD_tbaa);

	if (tag == nullptr || tag->getNumOperands () < 2)
		return false;

	const auto *type = dyn_cast<MDNode> (tag->getOperand (1));

	if (type == nullptr || type->getNumOperands () < 1)
		return false;

	const auto *name = dyn_cast<MDString> (type->getOperand (0));

	return name != nullptr && name->getString () == managed_reference_tbaa_leaf;
}

/// Returns the declared class of a reference loaded from a static field.
MonoClass *
static_field_class (const LoadInst &load, const DataLayout &layout)
{
	int64_t offset = 0;
	MonoClassField *field = static_field_addressed (load.getPointerOperand (), layout, offset);

	if (field == nullptr || m_field_get_offset (field) != offset)
		return nullptr;

	MonoType *type = mono_field_get_type_internal (field);

	if (type->byref || !MONO_TYPE_IS_REFERENCE (type))
		return nullptr;

	return mono_class_from_mono_type_internal (type);
}

Base
classify (const Value *pointer, const Function &f)
{
	const DataLayout &layout = f.getParent ()->getDataLayout ();
	int64_t offset = 0;

	if (static_field_addressed (pointer, layout, offset) != nullptr)
		return { Base::Kind::statics };

	const Value *root = pointer->stripInBoundsOffsets ();

	if (MonoClass *klass = stated_class (root, f).first)
		return { Base::Kind::heap, klass };

	// A managed slot holds an object reference and never an interior pointer.
	if (const auto *load = dyn_cast<LoadInst> (root); load != nullptr && loads_reference (*load))
		return { Base::Kind::heap, static_field_class (*load, layout) };

	return {};
}

const Function *
function_of (const Value *v, const Instruction *at)
{
	if (const auto *arg = dyn_cast<Argument> (v))
		return arg->getParent ();
	if (const auto *inst = dyn_cast<Instruction> (v))
		return inst->getFunction ();
	return at != nullptr ? at->getFunction () : nullptr;
}

} // namespace

bool
classes_share_no_instance (MonoClass *a, MonoClass *b)
{
	if (a == b)
		return false;

	for (MonoClass *klass : { a, b }) {
		if (MONO_CLASS_IS_INTERFACE (klass) || m_class_is_delegate (klass)
		    || mono_class_is_marshalbyref (klass) || mono_class_is_contextbound (klass)
		    || mono_class_is_com_object (klass))
			return false;
	}

	if (m_class_get_rank (a) != 0 && m_class_get_rank (b) != 0)
		return false;

	return !mono_class_has_parent (a, b) && !mono_class_has_parent (b, a);
}

AliasResult
ManagedAAResult::alias (const MemoryLocation &a, const MemoryLocation &b, AAQueryInfo &,
                        const Instruction *at)
{
	const Function *f = function_of (a.Ptr, at);

	if (f == nullptr)
		f = function_of (b.Ptr, at);
	if (f == nullptr)
		return AliasResult::MayAlias;

	Base left = classify (a.Ptr, *f);

	if (left.kind == Base::Kind::unknown)
		return AliasResult::MayAlias;

	Base right = classify (b.Ptr, *f);

	if (right.kind == Base::Kind::unknown)
		return AliasResult::MayAlias;

	if (left.kind != right.kind)
		return AliasResult::NoAlias;

	if (left.kind == Base::Kind::heap && left.bound != nullptr && right.bound != nullptr
	    && classes_share_no_instance (left.bound, right.bound))
		return AliasResult::NoAlias;

	return AliasResult::MayAlias;
}

} // namespace mono

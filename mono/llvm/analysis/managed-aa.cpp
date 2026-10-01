#include "managed-aa.hpp"

#include "internal-loads.hpp"
#include "method-symbols.hpp"
#include "operand-class.hpp"
#include "strip-casts.hpp"

#include "mono/metadata/abi-details.h"
#include "mono/metadata/class-inlines.h"
#include "mono/metadata/class-internals.h"
#include "mono/metadata/metadata.h"
#include "mono/metadata/tabledefs.h"

#include <llvm/ADT/APInt.h>
#include <llvm/ADT/ArrayRef.h>
#include <llvm/ADT/DenseMap.h>
#include <llvm/ADT/SmallVector.h>
#include <llvm/IR/Argument.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DataLayout.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/GlobalValue.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Metadata.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Operator.h>

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
	/// Object class bound, if any.
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

	// The access type is the reference node or a node below it.
	for (const auto *type = dyn_cast<MDNode> (tag->getOperand (1));
	     type != nullptr && type->getNumOperands () >= 2;
	     type = dyn_cast<MDNode> (type->getOperand (1))) {
		const auto *name = dyn_cast<MDString> (type->getOperand (0));

		if (name != nullptr && name->getString () == managed_reference_tbaa_leaf)
			return true;
	}

	return false;
}

/// The class declared by a reference slot, or null.
MonoClass *
reference_class (MonoType *type)
{
	// An open type has no single runtime class.
	if (type->byref || !MONO_TYPE_IS_REFERENCE (type) || mono_class_is_open_constructed_type (type))
		return nullptr;

	return mono_class_from_mono_type_internal (type);
}

bool
has_explicit_layout (MonoClass *klass)
{
	return (mono_class_get_flags (klass) & TYPE_ATTRIBUTE_LAYOUT_MASK)
	       == TYPE_ATTRIBUTE_EXPLICIT_LAYOUT;
}

/// The class of the reference field at \p offset in \p klass, or null.
MonoClass *
instance_field_class (MonoClass *klass, int64_t offset)
{
	// Only this class and its parents have fields at a shared instance offset.
	for (MonoClass *owner = klass; owner != nullptr; owner = m_class_get_parent (owner)) {
		// Explicit layout may overlap incompatible reference fields.
		if (has_explicit_layout (owner))
			return nullptr;

		gpointer iter = nullptr;

		while (MonoClassField *field = mono_class_get_fields_internal (owner, &iter)) {
			if ((mono_field_get_flags (field) & FIELD_ATTRIBUTE_STATIC) != 0
			    || mono_field_is_deleted (field))
				continue;

			MonoType *type = mono_field_get_type_internal (field);
			int64_t start = m_field_get_offset (field);

			if (offset == start)
				return reference_class (type);

			if (offset < start || !MONO_TYPE_ISSTRUCT (type))
				continue;

			int align = 0;

			if (offset >= start + mono_type_size (type, &align))
				continue;

			// A value type's field offsets count its boxed header.
			return instance_field_class (mono_class_from_mono_type_internal (type),
			                             offset - start + MONO_ABI_SIZEOF (MonoObject));
		}
	}

	return nullptr;
}

constexpr unsigned max_bound_depth = 4;

/// Classes assumed for phis being evaluated.
using Assumed = SmallDenseMap<const PHINode *, MonoClass *, 4>;

MonoClass *bound_of (const Value *object, const Function &f, unsigned depth, Assumed &assumed);

/// The class bounding the reference \p load reads, or null.
///
/// This assumes typed slots are not mutated through `Unsafe.As`.
MonoClass *
loaded_class (const LoadInst &load, const Function &f, unsigned depth, Assumed &assumed)
{
	const DataLayout &layout = f.getParent ()->getDataLayout ();
	const Value *address = load.getPointerOperand ();
	int64_t offset = 0;

	if (MonoClassField *field = static_field_addressed (address, layout, offset))
		return m_field_get_offset (field) == offset
		               ? reference_class (mono_field_get_type_internal (field))
		               : nullptr;

	APInt constant (layout.getIndexTypeSizeInBits (address->getType ()), 0);
	bool variable = false;

	address = address->stripPointerCasts ();

	while (const auto *gep = dyn_cast<GEPOperator> (address)) {
		// Only an inbounds step stays inside the object it starts from.
		if (!gep->isInBounds ())
			return nullptr;

		APInt step (constant.getBitWidth (), 0);

		if (gep->accumulateConstantOffset (layout, step))
			constant += step;
		else
			variable = true;

		address = gep->getPointerOperand ()->stripPointerCasts ();
	}

	MonoClass *object = bound_of (address, f, depth + 1, assumed);

	if (object == nullptr)
		return nullptr;

	// Reference-tagged array loads access elements, not the header.
	if (m_class_get_rank (object) != 0) {
		MonoClass *element = m_class_get_element_class (object);

		return m_class_is_valuetype (element) ? nullptr
		                                      : reference_class (m_class_get_byval_arg (element));
	}

	if (variable || !constant.isSignedIntN (32))
		return nullptr;

	return instance_field_class (object, constant.getSExtValue ());
}

/// The class shared by non-null \p arms, or null.
MonoClass *
agreed_bound (ArrayRef<const Value *> arms, MonoClass *expected, const Function &f,
              unsigned depth, Assumed &assumed)
{
	MonoClass *agreed = expected;

	for (const Value *arm : arms) {
		if (isa<ConstantPointerNull> (arm))
			continue;

		MonoClass *klass = bound_of (arm, f, depth + 1, assumed);

		if (klass == nullptr || (agreed != nullptr && klass != agreed))
			return nullptr;

		agreed = klass;
	}

	return agreed;
}

/// The class bounding the object \p object points at, or null.
MonoClass *
bound_of (const Value *object, const Function &f, unsigned depth, Assumed &assumed)
{
	if (depth > max_bound_depth)
		return nullptr;

	object = strip_casts (object->stripPointerCasts ());

	if (MonoClass *klass = stated_class (object, f).first)
		return klass;

	if (const auto *load = dyn_cast<LoadInst> (object))
		return loads_reference (*load) ? loaded_class (*load, f, depth, assumed) : nullptr;

	if (const auto *select = dyn_cast<SelectInst> (object))
		return agreed_bound ({ select->getTrueValue (), select->getFalseValue () }, nullptr, f,
		                     depth, assumed);

	const auto *phi = dyn_cast<PHINode> (object);

	if (phi == nullptr)
		return nullptr;

	if (auto held = assumed.find (phi); held != assumed.end ())
		return held->second;

	// Assume a class for a recursive phi, then verify every incoming value.
	SmallVector<const Value *, 4> arms (phi->incoming_values ().begin (),
	                                    phi->incoming_values ().end ());
	MonoClass *candidate = nullptr;

	assumed [phi] = nullptr;

	for (const Value *arm : arms) {
		if (!isa<ConstantPointerNull> (arm))
			candidate = bound_of (arm, f, depth + 1, assumed);
		if (candidate != nullptr)
			break;
	}

	if (candidate != nullptr) {
		assumed [phi] = candidate;
		candidate = agreed_bound (arms, candidate, f, depth, assumed);
	}

	assumed.erase (phi);
	return candidate;
}

Base
classify (const Value *pointer, const Function &f)
{
	const DataLayout &layout = f.getParent ()->getDataLayout ();
	int64_t offset = 0;

	if (static_field_addressed (pointer, layout, offset) != nullptr)
		return { Base::Kind::statics };

	// Plain GEPs can escape their source object.
	const Value *root = pointer->stripInBoundsOffsets ();

	Assumed assumed;

	if (MonoClass *klass = bound_of (root, f, 0, assumed))
		return { Base::Kind::heap, klass };

	// A managed slot holds an object reference and never an interior pointer.
	if (const auto *load = dyn_cast<LoadInst> (root); load != nullptr && loads_reference (*load))
		return { Base::Kind::heap };

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
		if (MONO_CLASS_IS_INTERFACE_INTERNAL (klass) || m_class_is_delegate (klass)
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

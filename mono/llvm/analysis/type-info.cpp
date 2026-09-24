#include "type-info.hpp"

#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/GlobalObject.h>
#include <llvm/IR/Metadata.h>

using namespace llvm;

namespace mono {
namespace {

enum InfoIndex {
	info_type,
	info_object_class,
	info_underlying,
	info_base,
	info_vtable,
	info_size,
	info_allocator,
	info_flags,
	info_count,
};

enum InfoFlag : uint8_t {
	flag_erasable = 1,
	flag_raises = 2,
};

Metadata *
held (Constant *c)
{
	return c != nullptr ? ConstantAsMetadata::get (c) : nullptr;
}

} // namespace

void
mark_type_info (GlobalObject &type_object, const TypeInfo &info)
{
	LLVMContext &c = type_object.getContext ();
	Type *i64 = Type::getInt64Ty (c);
	uint8_t flags = (info.box.erasable ? flag_erasable : 0) | (info.box.raises ? flag_raises : 0);
	Metadata *operands[info_count] = {
		held (ConstantInt::get (i64, reinterpret_cast<uintptr_t> (info.type))),
		held (ConstantInt::get (i64, reinterpret_cast<uintptr_t> (info.object_class))),
		held (info.underlying),
		held (info.base),
		held (info.box.vtable),
		held (ConstantInt::get (Type::getInt32Ty (c), info.box.size)),
		held (info.box.allocator),
		held (ConstantInt::get (Type::getInt8Ty (c), flags)),
	};

	type_object.setMetadata (type_info_metadata, MDNode::get (c, operands));
}

std::optional<TypeInfo>
type_info (const GlobalObject &type_object)
{
	const MDNode *node = type_object.getMetadata (type_info_metadata);

	if (node == nullptr || node->getNumOperands () != info_count)
		return std::nullopt;

	auto *type = mdconst::dyn_extract_or_null<ConstantInt> (node->getOperand (info_type));
	auto *object_class =
		mdconst::dyn_extract_or_null<ConstantInt> (node->getOperand (info_object_class));

	if (type == nullptr || object_class == nullptr)
		return std::nullopt;

	TypeInfo info;
	info.type = reinterpret_cast<MonoType *> (type->getZExtValue ());
	info.object_class = reinterpret_cast<MonoClass *> (object_class->getZExtValue ());

	// Metadata keeps no symbol alive, so GlobalDCE can drop one only this mark
	// refers to, and LLVM then clears the operand. Every enum field is read
	// back together or not at all.
	auto *underlying = mdconst::dyn_extract_or_null<Constant> (node->getOperand (info_underlying));
	auto *base = mdconst::dyn_extract_or_null<Constant> (node->getOperand (info_base));

	if (underlying != nullptr && base != nullptr) {
		info.underlying = underlying;
		info.base = base;
	}

	auto *vtable = mdconst::dyn_extract_or_null<Constant> (node->getOperand (info_vtable));
	auto *size = mdconst::dyn_extract_or_null<ConstantInt> (node->getOperand (info_size));
	auto *allocator = mdconst::dyn_extract_or_null<Function> (node->getOperand (info_allocator));
	auto *flags = mdconst::dyn_extract_or_null<ConstantInt> (node->getOperand (info_flags));

	if (vtable != nullptr && size != nullptr && allocator != nullptr && flags != nullptr) {
		info.box.vtable = vtable;
		info.box.size = int32_t (size->getSExtValue ());
		info.box.allocator = allocator;
		info.box.erasable = (flags->getZExtValue () & flag_erasable) != 0;
		info.box.raises = (flags->getZExtValue () & flag_raises) != 0;
	}

	return info;
}

} // namespace mono

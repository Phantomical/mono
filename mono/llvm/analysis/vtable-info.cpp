#include "vtable-info.hpp"

#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/GlobalObject.h>
#include <llvm/IR/Metadata.h>

using namespace llvm;

namespace mono {
namespace {

enum InfoIndex { info_klass, info_type, info_rank, info_count };

} // namespace

void
mark_vtable_info (GlobalObject &vtable, const VTableInfo &info)
{
	LLVMContext &c = vtable.getContext ();
	Metadata *held[info_count] = {
		ConstantAsMetadata::get (info.klass),
		ConstantAsMetadata::get (info.type),
		ConstantAsMetadata::get (
			ConstantInt::get (Type::getInt8Ty (c), info.rank)),
	};

	vtable.setMetadata (vtable_info_metadata, MDNode::get (c, held));
}

std::optional<VTableInfo>
vtable_info (const GlobalObject &vtable)
{
	const MDNode *node = vtable.getMetadata (vtable_info_metadata);

	if (node == nullptr || node->getNumOperands () != info_count)
		return std::nullopt;

	// Metadata keeps no symbol alive, so GlobalDCE drops a symbol this mark
	// alone refers to, and LLVM clears the operand. Reading the mark back
	// after that would name a symbol the module no longer defines.
	auto *klass = mdconst::dyn_extract_or_null<Constant> (node->getOperand (info_klass));
	auto *type = mdconst::dyn_extract_or_null<Constant> (node->getOperand (info_type));
	auto *rank = mdconst::dyn_extract_or_null<ConstantInt> (node->getOperand (info_rank));

	if (klass == nullptr || type == nullptr || rank == nullptr)
		return std::nullopt;

	return VTableInfo { klass, type, uint8_t (rank->getZExtValue ()) };
}

} // namespace mono

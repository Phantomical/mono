#include "vtable-facts.hpp"

#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/GlobalObject.h>
#include <llvm/IR/Metadata.h>

using namespace llvm;

namespace mono {
namespace {

enum FactIndex { fact_klass, fact_type, fact_rank, fact_count };

} // namespace

void
mark_vtable_facts (GlobalObject &vtable, const VTableFacts &facts)
{
	LLVMContext &c = vtable.getContext ();
	Metadata *held[fact_count] = {
		ConstantAsMetadata::get (facts.klass),
		ConstantAsMetadata::get (facts.type),
		ConstantAsMetadata::get (
			ConstantInt::get (Type::getInt8Ty (c), facts.rank)),
	};

	vtable.setMetadata (vtable_facts_metadata, MDNode::get (c, held));
}

std::optional<VTableFacts>
vtable_facts (const GlobalObject &vtable)
{
	const MDNode *node = vtable.getMetadata (vtable_facts_metadata);

	if (node == nullptr || node->getNumOperands () != fact_count)
		return std::nullopt;

	auto *klass = mdconst::dyn_extract<Constant> (node->getOperand (fact_klass));
	auto *type = mdconst::dyn_extract<Constant> (node->getOperand (fact_type));
	auto *rank = mdconst::dyn_extract<ConstantInt> (node->getOperand (fact_rank));

	if (klass == nullptr || type == nullptr || rank == nullptr)
		return std::nullopt;

	return VTableFacts { klass, type, uint8_t (rank->getZExtValue ()) };
}

} // namespace mono

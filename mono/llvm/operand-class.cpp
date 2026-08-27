/**
 * \file
 * \brief Writing a value's class beside it, and reading it back.
 */

#include "operand-class.hpp"

#include "passes/strip-casts.hpp"

#include "mono/metadata/class-internals.h"

#include <llvm/ADT/SmallVector.h>
#include <llvm/IR/Argument.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instruction.h>
#include <llvm/IR/Metadata.h>

#include <cstdint>

using namespace llvm;

namespace mono {
namespace {

Metadata *
as_metadata (LLVMContext &c, MonoClass *klass)
{
	return ConstantAsMetadata::get (ConstantInt::get (
		Type::getInt64Ty (c), reinterpret_cast<uintptr_t> (klass)));
}

/// The class at \p at in \p node, or null where the node holds none there.
MonoClass *
class_in (const MDNode *node, unsigned at)
{
	if (node == nullptr || at >= node->getNumOperands ())
		return nullptr;

	auto *held = mdconst::dyn_extract<ConstantInt> (node->getOperand (at));

	if (held == nullptr)
		return nullptr;

	return reinterpret_cast<MonoClass *> (
		static_cast<uintptr_t> (held->getZExtValue ()));
}

} // namespace

void
mark_allocated_class (Instruction &site, MonoClass *klass)
{
	LLVMContext &c = site.getContext ();

	site.setMetadata (alloc_class_md, MDNode::get (c, { as_metadata (c, klass) }));
}

void
mark_parameter_classes (Function &f, ArrayRef<std::pair<unsigned, MonoClass *>> classes)
{
	if (classes.empty ())
		return;

	LLVMContext &c = f.getContext ();
	SmallVector<Metadata *, 8> pairs;

	for (const auto &entry : classes)
		pairs.push_back (MDNode::get (
			c, { ConstantAsMetadata::get (
				     ConstantInt::get (Type::getInt32Ty (c), entry.first)),
		             as_metadata (c, entry.second) }));

	f.setMetadata (param_classes_md, MDNode::get (c, pairs));
}

std::pair<MonoClass *, bool>
operand_class (const Value *v, const Function &f)
{
	v = strip_casts (v);

	if (const auto *site = dyn_cast<Instruction> (v))
		return { class_in (site->getMetadata (alloc_class_md), 0), true };

	const auto *arg = dyn_cast<Argument> (v);

	if (arg == nullptr || arg->getParent () != &f)
		return { nullptr, false };

	const MDNode *listed = f.getMetadata (param_classes_md);

	if (listed == nullptr)
		return { nullptr, false };

	for (const MDOperand &entry : listed->operands ()) {
		const auto *pair = dyn_cast<MDNode> (entry);

		if (pair == nullptr || pair->getNumOperands () != 2)
			continue;

		auto *index = mdconst::dyn_extract<ConstantInt> (pair->getOperand (0));

		if (index == nullptr || index->getZExtValue () != arg->getArgNo ())
			continue;

		// A declared type is an upper bound. Every class the slot admits is
		// assignable to it, and none of them has to be it.
		return { class_in (pair, 1), false };
	}

	return { nullptr, false };
}

MonoClass *
exact_class (const Value *v, const Function &f)
{
	auto [klass, exact] = operand_class (v, f);

	if (klass == nullptr || exact)
		return klass;

	if (!m_class_is_sealed (klass) || m_class_get_rank (klass) != 0
	    || mono_class_is_marshalbyref (klass))
		return nullptr;

	return klass;
}

} // namespace mono

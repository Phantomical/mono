#include "fold-vtable.hpp"

#include "builtins.hpp"
#include "vtable-facts.hpp"
#include "vtable-func.hpp"

#include <llvm/IR/Constants.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/GlobalObject.h>
#include <llvm/IR/InstrTypes.h>
#include <llvm/Support/ErrorHandling.h>

#include <optional>

using namespace llvm;

namespace mono {
namespace {

/// What a site of the declaration \p name reads, taken off \p facts.
Constant *
field_of (StringRef name, const VTableFacts &facts, Type *held)
{
	if (name == vtable_klass_name)
		return facts.klass;

	if (name == vtable_type_name)
		return facts.type;

	if (name == vtable_rank_name)
		return ConstantInt::get (held, facts.rank);

	llvm_unreachable ("a vtable field with no value");
}

bool
fold_field (Function &f, StringRef name)
{
	bool changed = false;

	for (CallBase *site : builtin_sites (f, name)) {
		auto *vtable = dyn_cast<GlobalObject> (site->getArgOperand (0));

		if (vtable == nullptr)
			continue;

		std::optional<VTableFacts> facts = vtable_facts (*vtable);

		if (!facts)
			continue;

		site->replaceAllUsesWith (field_of (name, *facts, site->getType ()));
		site->eraseFromParent ();
		changed = true;
	}

	return changed;
}

} // namespace

bool
fold_vtable_fields (Function &f)
{
	bool changed = fold_field (f, vtable_klass_name);

	changed |= fold_field (f, vtable_type_name);

	return fold_field (f, vtable_rank_name) || changed;
}

} // namespace mono

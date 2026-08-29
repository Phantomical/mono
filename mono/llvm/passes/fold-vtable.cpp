#include "fold-vtable.hpp"

#include "analysis/operand-class.hpp"
#include "analysis/vtable-facts.hpp"
#include "builtins.hpp"
#include "compile-state.hpp"
#include "vtable-func.hpp"

#include <llvm/ADT/SmallVector.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/GlobalObject.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/IR/InstrTypes.h>
#include <llvm/IR/Instructions.h>
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
fold_object_vtables (Function &f)
{
	const CompileState &compile = current_compile ();

	if (compile.domain == nullptr || !compile.vtable_of)
		return false;

	// The reads are collected before any is erased, because erasing one moves
	// the iterator this walks with.
	SmallVector<LoadInst *, 8> reads;

	for (Instruction &i : instructions (f))
		if (object_vtable_read (&i) != nullptr)
			reads.push_back (cast<LoadInst> (&i));

	bool changed = false;

	for (LoadInst *read : reads) {
		MonoClass *klass = exact_class (object_vtable_read (read), f);

		if (klass == nullptr)
			continue;

		Constant *vtable = compile.vtable_of (*f.getParent (), klass);

		if (vtable == nullptr)
			continue;

		read->replaceAllUsesWith (vtable);
		read->eraseFromParent ();
		changed = true;
	}

	return changed;
}

bool
fold_vtable_fields (Function &f)
{
	bool changed = fold_field (f, vtable_klass_name);

	changed |= fold_field (f, vtable_type_name);

	return fold_field (f, vtable_rank_name) || changed;
}

} // namespace mono

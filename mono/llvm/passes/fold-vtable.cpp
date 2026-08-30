#include "fold-vtable.hpp"

#include "analysis/constant-values.hpp"
#include "analysis/operand-class.hpp"
#include "analysis/vtable-info.hpp"
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

/// What a site of the declaration \p name reads, taken off \p info.
Constant *
field_of (StringRef name, const VTableInfo &info, Type *held)
{
	if (name == vtable_klass_name)
		return info.klass;

	if (name == vtable_type_name)
		return info.type;

	if (name == vtable_rank_name)
		return ConstantInt::get (held, info.rank);

	llvm_unreachable ("a vtable field with no value");
}

bool
fold_field (Function &f, StringRef name, FunctionAnalysisManager &fam)
{
	bool changed = false;
	const ConstantValues *values = nullptr;

	for (CallBase *site : builtin_sites (f, name)) {
		if (values == nullptr)
			values = &fam.getResult<MonoConstantValues> (f);

		const auto *vtable = dyn_cast_or_null<GlobalObject> (
			values->global (site->getArgOperand (0)));

		if (vtable == nullptr)
			continue;

		std::optional<VTableInfo> info = vtable_info (*vtable);

		if (!info)
			continue;

		site->replaceAllUsesWith (field_of (name, *info, site->getType ()));
		site->eraseFromParent ();
		changed = true;
	}

	return changed;
}

} // namespace

bool
fold_object_vtables (Function &f, FunctionAnalysisManager &fam)
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

	const ConstantValues *values = nullptr;

	for (LoadInst *read : reads) {
		if (values == nullptr)
			values = &fam.getResult<MonoConstantValues> (f);

		MonoClass *klass = exact_class (object_vtable_read (read), f, *values);

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
fold_vtable_fields (Function &f, FunctionAnalysisManager &fam)
{
	bool changed = fold_field (f, vtable_klass_name, fam);

	changed |= fold_field (f, vtable_type_name, fam);

	return fold_field (f, vtable_rank_name, fam) || changed;
}

} // namespace mono

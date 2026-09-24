/**
 * \file
 * \brief Lowering unresolved System.Enum builtins to their fallback calls.
 */

#include "enum.hpp"

#include "builtins.hpp"

#include <llvm/ADT/SmallVector.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/InstrTypes.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/ErrorHandling.h>

using namespace llvm;

namespace mono {
namespace {

const StringRef enum_builtin_names[] = {
	enum_hasflag_name,
	enum_hashcode_name,
	enum_compare_name,
	enum_equals_name,
	enum_elementtype_name,
	type_enum_underlying_name,
	type_enum_box_name,
	type_base_name,
	type_is_generic_var_name,
	type_attributes_name,
	type_enum_defined_name,
};

} // namespace

Function *
enum_builtin_decl (Module &m, StringRef name)
{
	LLVMContext &c = m.getContext ();
	Type *ptr = PointerType::get (c, 0);
	Type *i8 = Type::getInt8Ty (c);
	Type *i32 = Type::getInt32Ty (c);
	FunctionType *shape;

	if (name == enum_hasflag_name || name == enum_equals_name || name == type_enum_defined_name)
		shape = FunctionType::get (i8, { ptr, ptr, ptr }, false);
	else if (name == enum_compare_name)
		shape = FunctionType::get (i32, { ptr, ptr, ptr }, false);
	else if (name == enum_hashcode_name || name == type_attributes_name)
		shape = FunctionType::get (i32, { ptr, ptr }, false);
	else if (name == enum_elementtype_name || name == type_is_generic_var_name)
		shape = FunctionType::get (i8, { ptr, ptr }, false);
	else if (name == type_enum_underlying_name || name == type_base_name)
		shape = FunctionType::get (ptr, { ptr, ptr }, false);
	else if (name == type_enum_box_name)
		shape = FunctionType::get (ptr, { ptr, Type::getInt64Ty (c), ptr }, false);
	else
		llvm_unreachable ("not an enum builtin");

	return builtin_decl (m, name, shape);
}

bool
lower_enum_builtins (Module &m)
{
	bool changed = false;

	for (StringRef name : enum_builtin_names) {
		for (CallBase *site : builtin_sites (m, name)) {
			unsigned last = site->arg_size () - 1;
			auto *fallback = cast<Function> (site->getArgOperand (last));
			SmallVector<Value *, 2> args (site->args ().begin (),
			                              site->args ().begin () + last);
			CallBase *direct;

			if (auto *invoke = dyn_cast<InvokeInst> (site))
				direct = InvokeInst::Create (fallback, invoke->getNormalDest (),
				                             invoke->getUnwindDest (), args, "",
				                             site->getIterator ());
			else
				direct = CallInst::Create (fallback, args, "", site->getIterator ());

			direct->setDebugLoc (site->getDebugLoc ());
			site->replaceAllUsesWith (direct);
			site->eraseFromParent ();
			changed = true;
		}

		changed |= erase_builtin (m, name);
	}

	return changed;
}

} // namespace mono

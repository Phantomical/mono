/**
 * \file
 * \brief Writing back an Enum.HasFlag () site nothing settled.
 */

#include "enum-flag.hpp"

#include "builtins.hpp"

#include <llvm/ADT/SmallVector.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/InstrTypes.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>

using namespace llvm;

namespace mono {

Function *
enum_hasflag_decl (Module &m)
{
	LLVMContext &c = m.getContext ();
	Type *ptr = PointerType::get (c, 0);

	return builtin_decl (m, enum_hasflag_name,
	                     FunctionType::get (Type::getInt8Ty (c), { ptr, ptr, ptr }, false));
}

bool
lower_enum_has_flag (Module &m)
{
	SmallVector<CallBase *, 8> sites = builtin_sites (m, enum_hasflag_name);

	for (CallBase *site : sites) {
		auto *fallback = cast<Function> (site->getArgOperand (2));
		Value *args[] = { site->getArgOperand (0), site->getArgOperand (1) };
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
	}

	bool changed = !sites.empty ();

	return erase_builtin (m, enum_hasflag_name) || changed;
}

} // namespace mono

#include "builtins.hpp"

#include "array-address.hpp"
#include "array-shape.hpp"
#include "cast-func.hpp"
#include "devirtualize.hpp"
#include "fold-cast.hpp"
#include "fold-vtable.hpp"
#include "lower-builtins.hpp"
#include "vtable-func.hpp"

#include <llvm/ADT/Twine.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/InstrTypes.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/ErrorHandling.h>

using namespace llvm;

namespace mono {
namespace {

/// Times the folds take up a function's sites again.
///
/// One fold exposes another. A type test that settles a receiver's class
/// settles the dispatch below it. A dispatch folded to a direct call hands the
/// next round an operand it could not read. Each round walks the sites again,
/// which is what this bounds.
constexpr unsigned fold_rounds = 4;

SmallVector<CallBase *, 8>
sites_of (Function *decl, const Function *inside)
{
	SmallVector<CallBase *, 8> found;

	if (decl == nullptr)
		return found;

	for (User *user : decl->users ()) {
		auto *site = dyn_cast<CallBase> (user);

		if (site != nullptr && (inside == nullptr || site->getFunction () == inside))
			found.push_back (site);
	}

	return found;
}

} // namespace

Function *
builtin_decl (Module &m, StringRef name, FunctionType *shape)
{
	if (Function *existing = m.getFunction (name))
		return existing;

	return Function::Create (shape, GlobalValue::ExternalLinkage, name, m);
}

SmallVector<CallBase *, 8>
builtin_sites (Module &m, StringRef name)
{
	return sites_of (m.getFunction (name), nullptr);
}

SmallVector<CallBase *, 8>
builtin_sites (Function &f, StringRef name)
{
	return sites_of (f.getParent ()->getFunction (name), &f);
}

bool
erase_builtin (Module &m, StringRef name)
{
	Function *decl = m.getFunction (name);

	if (decl == nullptr)
		return false;

	if (!decl->use_empty ())
		report_fatal_error (Twine ("unlowered use of ") + name);

	decl->eraseFromParent ();
	return true;
}

PreservedAnalyses
MonoBuiltinConstProp::run (Function &f, FunctionAnalysisManager &)
{
	bool changed = false;

	for (unsigned round = 0; round < fold_rounds; ++round) {
		// Type tests first: folding one is what delivers the allocation a
		// chain's receiver comes from.
		bool again = fold_type_tests (f);

		again |= fold_object_vtables (f);
		again |= fold_vtable_fields (f);
		again |= fold_dispatch_sites (f);
		again |= fold_array_shapes (f);

		if (!again)
			break;

		changed = true;
	}

	return changed ? PreservedAnalyses::none () : PreservedAnalyses::all ();
}

PreservedAnalyses
MonoBuiltinLower::run (Module &m, ModuleAnalysisManager &)
{
	bool changed = false;

	switch (stage) {
	case LowerStage::pre_simplification:
		changed = lower_array_addresses (m);
		changed |= lower_runtime_builtins (m);
		break;

	case LowerStage::pre_profile:
		changed = lower_array_shapes (m);
		break;

	case LowerStage::post_inline:
		changed = lower_type_tests (m);
		changed |= lower_vtable_reads (m);
		break;
	}

	return changed ? PreservedAnalyses::none () : PreservedAnalyses::all ();
}

} // namespace mono

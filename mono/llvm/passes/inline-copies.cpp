#include "inline-copies.hpp"

#include "tier-counter.hpp"

#include <llvm/ADT/SmallVector.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Module.h>

#include <string>

using namespace llvm;

namespace mono {

void
mark_inline_copy (Function &copy, StringRef published_name)
{
	// The marks select a body for the counters that ask for the next tier.
	// Those belong to the method's own body, and this is a copy of it.
	copy.removeFnAttr (tier_counter_attribute);
	copy.removeFnAttr (tier_entry_weight_attribute);
	copy.removeFnAttr (tier_handle_attribute);

	// Local linkage is what lets an inliner delete the copy once it has folded
	// every call to it.
	copy.setLinkage (GlobalValue::InternalLinkage);
	copy.addFnAttr (inline_copy_attribute, published_name);
}

PreservedAnalyses
StripInlineCopiesPass::run (Module &m, ModuleAnalysisManager &mam)
{
	SmallVector<Function *, 8> copies;

	for (Function &fn : m)
		if (fn.hasFnAttribute (inline_copy_attribute))
			copies.push_back (&fn);

	if (copies.empty ())
		return PreservedAnalyses::all ();

	FunctionAnalysisManager &fam =
		mam.getResult<FunctionAnalysisManagerModuleProxy> (m).getManager ();

	for (Function *copy : copies) {
		std::string published =
			copy->getFnAttribute (inline_copy_attribute).getValueAsString ().str ();

		if (!copy->isDeclaration ())
			copy->deleteBody ();

		// getInlineCost () returns getAlways () for an alwaysinline
		// declaration, and the inliner then hard-fails on a body that is not
		// there.
		copy->removeFnAttr (Attribute::AlwaysInline);
		copy->removeFnAttr (inline_copy_attribute);

		GlobalValue *existing = m.getNamedValue (published);

		if (existing == copy)
			continue;

		/*
		 * The translator declares a method under a placeholder rather than
		 * under the published name. So a translation that called the same
		 * method after the copy was made got a declaration of its own. Two
		 * references to one method have to become one value, or setName ()
		 * uniques the copy into a symbol nothing defines.
		 */
		if (existing != nullptr) {
			copy->replaceAllUsesWith (existing);
			fam.clear (*copy, copy->getName ());
			copy->eraseFromParent ();
			continue;
		}

		copy->setName (published);
	}

	return PreservedAnalyses::none ();
}

Error
inline_copies_stripped (const Module &m)
{
	for (const Function &fn : m) {
		if (!fn.hasFnAttribute (inline_copy_attribute))
			continue;

		return createStringError (inconvertibleErrorCode (),
		                          "%s was materialized to be folded into its "
		                          "caller and was never taken back off",
		                          fn.getName ().str ().c_str ());
	}

	return Error::success ();
}

} // namespace mono

#include "profile-counters.hpp"
#include "tier-counter.hpp"

#include <llvm/IR/Function.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/IR/IntrinsicInst.h>
#include <llvm/IR/Module.h>
#include <llvm/ProfileData/InstrProf.h>

using namespace llvm;

namespace mono {
namespace {

/// The name the reader keys the function on.
///
/// The instrumentation put it in a global of its own, which is the copy the
/// reader would have got had this gone through a file.
std::string
recorded_name (const InstrProfCntrInstBase &inc, const Function &f)
{
	if (auto *var = dyn_cast<GlobalVariable> (inc.getNameValue ()))
		if (auto *text = dyn_cast_or_null<ConstantDataArray> (var->getInitializer ()))
			return text->getAsString ().str ();

	return getIRPGOFuncName (f);
}

/// Whether the lowering wrote \p global rather than the translator.
bool
is_profile_global (const GlobalVariable &global)
{
	// The version marker and the section anchors are "__llvm_prof..."; the
	// per-function arrays carry the prefixes below.
	StringRef name = global.getName ();

	return name.starts_with ("__llvm_prof")
	       || name.starts_with (getInstrProfCountersVarPrefix ())
	       || name.starts_with (getInstrProfDataVarPrefix ())
	       || name.starts_with (getInstrProfNameVarPrefix ())
	       || name.starts_with (getInstrProfBitmapVarPrefix ());
}

} // namespace

std::vector<ProfileSite> &
profile_sites ()
{
	static thread_local std::vector<ProfileSite> sites;

	return sites;
}

PreservedAnalyses
ProfileSelectPass::run (Module &m, ModuleAnalysisManager &)
{
	for (Function &f : m)
		if (!f.isDeclaration () && !f.hasFnAttribute (tier_counter_attribute))
			f.addFnAttr (Attribute::NoProfile);

	return PreservedAnalyses::all ();
}

PreservedAnalyses
ProfileGatherPass::run (Module &m, ModuleAnalysisManager &)
{
	std::vector<ProfileSite> &sites = profile_sites ();

	for (Function &f : m) {
		for (Instruction &i : instructions (f)) {
			auto *inc = dyn_cast<InstrProfCntrInstBase> (&i);

			if (inc == nullptr)
				continue;

			ProfileSite site;

			site.name = recorded_name (*inc, f);
			site.hash = inc->getHash ()->getZExtValue ();
			site.counters = inc->getNumCounters ()->getZExtValue ();

			sites.push_back (std::move (site));

			// Every increment in a function carries the same three values.
			break;
		}
	}

	return PreservedAnalyses::all ();
}

PreservedAnalyses
ProfileLocalizePass::run (Module &m, ModuleAnalysisManager &)
{
	for (GlobalVariable &global : m.globals ()) {
		// A declaration has to keep external linkage to stay valid IR. The only
		// one here is the profile runtime's hook, which nothing refers to
		// outside llvm.compiler.used, so no relocation ever asks for it.
		if (!is_profile_global (global) || global.isDeclaration ())
			continue;

		global.setComdat (nullptr);
		global.setLinkage (GlobalValue::PrivateLinkage);
	}

	return PreservedAnalyses::all ();
}

} // namespace mono

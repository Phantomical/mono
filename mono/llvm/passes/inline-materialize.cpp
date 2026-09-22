#include "inline-materialize.hpp"

#include "clause-marker.hpp"
#include "finally-marker.hpp"
#include "inline-copies.hpp"
#include "pipelines.hpp"
#include "tier-counter.hpp"

#include "../mono_lsda_format.hpp"

#include <llvm/Analysis/ProfileSummaryInfo.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/ValueHandle.h>
#include <llvm/Linker/Linker.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/Transforms/Utils/Cloning.h>

#include <memory>
#include <string>

using namespace llvm;

namespace mono {

ScratchAnalyses::ScratchAnalyses (PassBuilder &pb)
{
	pb.registerModuleAnalyses (mam);
	pb.registerCGSCCAnalyses (cgam);
	pb.registerFunctionAnalyses (fam);
	register_mono_analyses (fam);
	pb.registerLoopAnalyses (lam);
	pb.crossRegisterProxies (lam, fam, cgam, mam);
}

void
ScratchAnalyses::clear ()
{
	mam.clear ();
	cgam.clear ();
	fam.clear ();
	lam.clear ();
}

Function *
materialize_candidate (Module &m, Function &decl, InlineCandidates &candidates,
                       ModulePassManager &prepare, ScratchAnalyses &scratch, OneFileFS &profile_fs,
                       std::optional<SiteHeat> heat, const CallBase &call)
{
	std::string name = decl.getName ().str ();
	auto into = std::make_unique<Module> (name, m.getContext ());

	// The preparation and link must use the same target description.
	into->setDataLayout (m.getDataLayout ());
	into->setTargetTriple (m.getTargetTriple ());

	Function *made = candidates.materialize (decl, *into, heat, call);

	if (made == nullptr)
		return nullptr;

	// The engine may have materialized the body directly in the root module.
	if (made->getParent () == &m) {
		decl.replaceAllUsesWith (made);
		decl.eraseFromParent ();
		return made;
	}

	// Link the translated copy over the declaration, then restore its linkage.
	std::string copy = made->getName ().str ();

	made->setLinkage (GlobalValue::ExternalLinkage);

	// StripInlineCopiesPass removes this marker while preparing the copy.
	Attribute mark = made->getFnAttribute (inline_copy_attribute);

	made->removeFnAttr (inline_copy_attribute);

	OneFileFS::CurrentFileGuard counts = pushProfile (profile_fs, candidates.profile_for (decl));

	prepare.run (*into, scratch.mam);
	scratch.clear ();

	// Keep the root's summary flags; the candidate's branch weights are retained.
	if (Metadata *summary = m.getProfileSummary (/*IsCS=*/false))
		into->setProfileSummary (summary, ProfileSummary::PSK_Instr);

	// The link replaces the declaration, so look up the resulting body after it.
	if (Linker::linkModules (m, std::move (into)))
		return nullptr;

	Function *body = m.getFunction (copy);

	if (body == nullptr || body->isDeclaration ())
		return nullptr;

	body->setLinkage (GlobalValue::InternalLinkage);

	if (mark.isValid ())
		body->addFnAttr (mark);

	// Replace the declaration if the translator gave the copy a different name.
	Function *site = m.getFunction (name);

	if (site != nullptr && site != body) {
		site->replaceAllUsesWith (body);
		site->eraseFromParent ();
	}

	return body;
}

bool
has_own_clause (const Function &callee)
{
	for (const Instruction &i : instructions (callee))
		if (isa<LandingPadInst> (i) || finally_body_marker (i))
			return true;

	return false;
}

/* Return whether all landing-pad clauses use kinds supported by inline EH merging. */
bool
mergeable_clause_kinds_only (const Function &callee)
{
	for (const Instruction &i : instructions (callee)) {
		const auto *lpi = dyn_cast<LandingPadInst> (&i);

		if (lpi == nullptr)
			continue;

		for (unsigned c = 0; c < lpi->getNumClauses (); ++c) {
			// Filters are not supported by the inline EH merger.
			if (lpi->isFilter (c))
				return false;

			const auto *gv = dyn_cast<GlobalValue> (lpi->getClause (c));
			int clause_index, kind;

			if (!decode_clause_marker (gv, clause_index, kind))
				return false;

			switch ((std::uint32_t) kind) {
			case MONO_ECMA_CLAUSE_NONE:
			case MONO_ECMA_CLAUSE_FINALLY:
			case MONO_ECMA_CLAUSE_FAULT:
				break;
			default:
				return false;
			}
		}
	}

	return true;
}

namespace {

/// Metadata used to identify the callee's EH instructions in a trial clone.
constexpr StringRef clause_trial_tag = "mono.clause-trial";

} // namespace

/* Check whether the trial inline leaves EH instructions that need a clause. */
bool
clause_survives_inline (CallBase &call, Function &callee, FunctionPassManager &simplify,
                        FunctionAnalysisManager &fam,
                        function_ref<AssumptionCache &(Function &)> get_ac)
{
	MDNode *tag = MDNode::get (callee.getContext (), {});

	for (Instruction &i : instructions (callee))
		if (isa<LandingPadInst> (i) || finally_body_marker (i))
			i.setMetadata (clause_trial_tag, tag);

	ValueToValueMapTy vmap;
	Function *trial = CloneFunction (call.getFunction (), vmap);
	auto *site = cast<CallBase> (static_cast<Value *> (vmap[&call]));

	trial->removeFnAttr (tier_counter_attribute);

	InlineFunctionInfo ifi (get_ac);
	bool survives = true;

	if (InlineFunction (*site, ifi, /*MergeAttributes=*/true).isSuccess ()) {
		simplify.run (*trial, fam);

		survives = false;
		for (Instruction &i : instructions (*trial))
			if (i.getMetadata (clause_trial_tag) != nullptr) {
				survives = true;
				break;
			}
	}

	fam.clear (*trial, trial->getName ());
	trial->eraseFromParent ();

	for (Instruction &i : instructions (callee))
		i.setMetadata (clause_trial_tag, nullptr);

	return survives;
}

} // namespace mono

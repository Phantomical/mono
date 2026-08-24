#include "protected-null-checks.hpp"

#include <llvm/IR/Function.h>
#include <llvm/IR/Instructions.h>

using namespace llvm;

namespace mono {

namespace {

/// The mark method_to_llvm () puts on a function it gave EH clauses, and that
/// MonoEHGatherPass reads.
constexpr StringRef has_eh_clauses_attribute = "mono-has-eh-clauses";

} // namespace

PreservedAnalyses
ProtectedNullChecksPass::run (Function &f, FunctionAnalysisManager &)
{
	if (!f.hasFnAttribute (has_eh_clauses_attribute))
		return PreservedAnalyses::all ();

	for (BasicBlock &block : f) {
		auto *branch = dyn_cast<BranchInst> (block.getTerminator ());

		if (branch != nullptr)
			branch->setMetadata (make_implicit_metadata, nullptr);
	}

	// Metadata only. The CFG and every value in it are untouched, and the PGO
	// hash both tiers take is over the edges.
	return PreservedAnalyses::all ();
}

} // namespace mono

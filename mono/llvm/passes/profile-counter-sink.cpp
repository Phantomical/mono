#include "profile-counter-sink.hpp"

#include "profile-counters.hpp"

#include <utility>

#include <llvm/ADT/APInt.h>
#include <llvm/ADT/SmallVector.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DataLayout.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/LLVMContext.h>

using namespace llvm;

namespace mono {
namespace {

/**
 * The pointer \p branch tests against null, and the arm it takes when the
 * pointer is not null. Both null for any other branch.
 *
 * The arm-to-predicate mapping is the one ImplicitNullChecks uses: an equality
 * test takes the false arm, an inequality test the true one. So an arm this
 * names is the arm the fold looks in.
 */
std::pair<Value *, BasicBlock *>
not_null_arm (BranchInst &branch)
{
	auto *compare = dyn_cast<ICmpInst> (branch.getCondition ());

	if (compare == nullptr || !isa<ConstantPointerNull> (compare->getOperand (1)))
		return { nullptr, nullptr };

	if (compare->getPredicate () == ICmpInst::ICMP_EQ)
		return { compare->getOperand (0), branch.getSuccessor (1) };
	if (compare->getPredicate () == ICmpInst::ICMP_NE)
		return { compare->getOperand (0), branch.getSuccessor (0) };

	return { nullptr, nullptr };
}

/// Whether \p i reads or writes through \p pointer at a constant displacement,
/// which is the shape a check folds into. How far in the hardware still traps is
/// LLVM's question.
bool
dereferences (Instruction &i, Value *pointer, const DataLayout &layout)
{
	Value *address = nullptr;

	if (auto *load = dyn_cast<LoadInst> (&i))
		address = load->getPointerOperand ();
	else if (auto *store = dyn_cast<StoreInst> (&i))
		address = store->getPointerOperand ();
	else
		return false;

	APInt displacement (layout.getIndexTypeSizeInBits (address->getType ()), 0);

	return address->stripAndAccumulateConstantOffsets (layout, displacement,
	                                                   /*AllowNonInbounds=*/true)
	       == pointer;
}

} // namespace

PreservedAnalyses
ProfileCounterSinkPass::run (Function &f, FunctionAnalysisManager &)
{
	const DataLayout &layout = f.getDataLayout ();
	bool moved = false;

	for (BasicBlock &block : f) {
		auto *branch = dyn_cast<BranchInst> (block.getTerminator ());

		if (branch == nullptr
		    || branch->getMetadata (LLVMContext::MD_make_implicit) == nullptr)
			continue;

		auto [pointer, arm] = not_null_arm (*branch);

		if (pointer == nullptr)
			continue;

		SmallVector<AtomicRMWInst *, 2> counters;

		for (Instruction &i : *arm) {
			auto *update = dyn_cast<AtomicRMWInst> (&i);

			if (update != nullptr
			    && is_counter_address (update->getPointerOperand ())) {
				counters.push_back (update);
				continue;
			}

			if (dereferences (i, pointer, layout)) {
				Instruction *after = &i;

				for (AtomicRMWInst *counter : counters) {
					counter->moveAfter (after);
					after = counter;
				}

				moved = moved || !counters.empty ();
				break;
			}

			// Stop where the fold gives up. It refuses a call and an ordered
			// memory operation outright, and this stops at any side effect,
			// which is wider.
			if (i.mayHaveSideEffects ())
				break;
		}
	}

	if (!moved)
		return PreservedAnalyses::all ();

	// Only the order inside one block changed, so the CFG is as it was.
	PreservedAnalyses preserved;

	preserved.preserveSet<CFGAnalyses> ();
	return preserved;
}

} // namespace mono

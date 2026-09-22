#include "lmf-dedup.hpp"

#include <llvm/ADT/MapVector.h>
#include <llvm/ADT/STLExtras.h>
#include <llvm/ADT/SmallVector.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Dominators.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/IntrinsicInst.h>

using namespace llvm;

namespace mono {
namespace {

/// The slot and its capture/release stores.
struct Slot {
	AllocaInst *alloca = nullptr;
	SmallVector<StoreInst *, 2> captures;
	SmallVector<StoreInst *, 2> releases;
};

/// Find the alloca named by a capture or release pointer.
AllocaInst *
underlying_slot (Value *ptr)
{
	if (auto *alloca = dyn_cast<AllocaInst> (ptr))
		return alloca;
	if (auto *gep = dyn_cast<GetElementPtrInst> (ptr))
		return dyn_cast<AllocaInst> (gep->getPointerOperand ());
	return nullptr;
}

/// Whether the function contains an IL localloc.
bool
has_dynamic_alloca (Function &f)
{
	for (Instruction &i : instructions (f))
		if (auto *alloca = dyn_cast<AllocaInst> (&i))
			if (!isa<ConstantInt> (alloca->getArraySize ()))
				return true;
	return false;
}

/// Collect all marked LMF slots.
MapVector<AllocaInst *, Slot>
collect_slots (Function &f)
{
	MapVector<AllocaInst *, Slot> slots;

	for (Instruction &i : instructions (f)) {
		auto *store = dyn_cast<StoreInst> (&i);

		if (store == nullptr)
			continue;

		if (store->getMetadata ("mono.lmf.capture") != nullptr) {
			if (AllocaInst *slot = underlying_slot (store->getPointerOperand ())) {
				Slot &entry = slots[slot];

				entry.alloca = slot;
				entry.captures.push_back (store);
			}
			continue;
		}

		if (store->getMetadata ("mono.lmf.release") != nullptr) {
			auto *previous = dyn_cast<LoadInst> (store->getValueOperand ());

			if (previous == nullptr)
				continue;
			if (AllocaInst *slot = underlying_slot (previous->getPointerOperand ())) {
				Slot &entry = slots[slot];

				entry.alloca = slot;
				entry.releases.push_back (store);
			}
		}
	}

	return slots;
}

/// Remove a capture and its now-dead value chain.
void
erase_capture (StoreInst *store)
{
	Value *ptr = store->getPointerOperand ();
	Value *value = store->getValueOperand ();

	store->eraseFromParent ();

	if (auto *ptrtoint = dyn_cast<PtrToIntInst> (value)) {
		Value *stacksave = ptrtoint->getOperand (0);

		if (ptrtoint->use_empty ())
			ptrtoint->eraseFromParent ();
		if (auto *call = dyn_cast<Instruction> (stacksave))
			if (call->use_empty ())
				call->eraseFromParent ();
	} else if (auto *call = dyn_cast<Instruction> (value)) {
		if (call->use_empty ())
			call->eraseFromParent ();
	}

	if (auto *gep = dyn_cast<GetElementPtrInst> (ptr))
		if (gep->use_empty ())
			gep->eraseFromParent ();
}

/// Fold donor's captures into target's slot.
void
merge_slot (Slot &donor, Slot &target)
{
	SmallVector<Instruction *, 4> lifetime;

	for (User *user : donor.alloca->users ())
		if (auto *marker = dyn_cast<IntrinsicInst> (user))
			if (marker->getIntrinsicID () == Intrinsic::lifetime_start
			    || marker->getIntrinsicID () == Intrinsic::lifetime_end)
				lifetime.push_back (marker);
	for (Instruction *marker : lifetime)
		marker->eraseFromParent ();

	for (StoreInst *store : donor.captures)
		erase_capture (store);

	donor.alloca->replaceAllUsesWith (target.alloca);
	donor.alloca->eraseFromParent ();

	target.releases.append (donor.releases);
}

} // namespace

PreservedAnalyses
LmfDedupPass::run (Function &f, FunctionAnalysisManager &fam)
{
	MapVector<AllocaInst *, Slot> slots = collect_slots (f);

	if (slots.size () < 2 || has_dynamic_alloca (f))
		return PreservedAnalyses::all ();

	const DominatorTree &dt = fam.getResult<DominatorTreeAnalysis> (f);
	SmallVector<Slot *, 4> open;
	bool changed = false;

	for (auto &entry : slots) {
		Slot &site = entry.second;

		// Only merge the shape emitted by the LMF helpers.
		if (site.captures.size () != 2 || site.releases.empty ())
			continue;

		Slot *reuse = nullptr;

		for (Slot *candidate : open) {
			bool dominated = llvm::all_of (candidate->releases, [&] (StoreInst *release) {
				return dt.dominates (release, site.captures.front ());
			});

			if (dominated) {
				reuse = candidate;
				break;
			}
		}

		if (reuse != nullptr) {
			merge_slot (site, *reuse);
			changed = true;
		} else {
			open.push_back (&site);
		}
	}

	return changed ? PreservedAnalyses::none () : PreservedAnalyses::all ();
}

} // namespace mono

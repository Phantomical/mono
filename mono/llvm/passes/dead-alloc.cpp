/**
 * \file
 * \brief The walk that reads an allocation as dead, and the erase that takes it
 * away with everything under it.
 */

#include "dead-alloc.hpp"

#include "alloc-func.hpp"
#include "builtins.hpp"
#include "gc-barrier.hpp"

#include <llvm/ADT/STLExtras.h>
#include <llvm/ADT/SetVector.h>
#include <llvm/ADT/SmallPtrSet.h>
#include <llvm/ADT/SmallVector.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/InstrTypes.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/IntrinsicInst.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/ValueHandle.h>
#include <llvm/Transforms/Utils/Local.h>

using namespace llvm;

namespace mono {
namespace {

/// What one allocation reaches.
struct Cluster {
	/// The allocation, and each pointer a getelementptr took off it.
	SmallPtrSet<const Value *, 8> derived;

	/// Every instruction the erase takes away, the allocation apart.
	SmallSetVector<Instruction *, 16> users;
};

bool
is_lifetime_mark (const Instruction &in)
{
	const auto *call = dyn_cast<IntrinsicInst> (&in);

	if (call == nullptr)
		return false;

	return call->getIntrinsicID () == Intrinsic::lifetime_start
	       || call->getIntrinsicID () == Intrinsic::lifetime_end;
}

/**
 * Walks what \p alloc reaches and fills \p cluster in. Returns false where a
 * user can observe the object.
 *
 * Four users are accepted: a store through the object, a barrier for that
 * store, a getelementptr, and a lifetime mark. Everything else is a refusal. A
 * load reads the object back, and a call hands it outside the function.
 *
 * The object has to be the destination rather than the value. A store of the
 * object into another object writes through a pointer this walk never reaches.
 * The barrier beside it is then all the walk finds. Taking that barrier for one
 * of our own erases an object something else now holds. The walk reads the
 * users of each derived value, so a refusal at either operand catches the pair.
 */
bool
collect (CallInst &alloc, const Function *barrier, Cluster &cluster)
{
	SmallVector<Value *, 8> queue;

	cluster.derived.insert (&alloc);
	queue.push_back (&alloc);

	while (!queue.empty ()) {
		Value *value = queue.pop_back_val ();

		for (User *user : value->users ()) {
			auto *in = dyn_cast<Instruction> (user);

			// A constant expression naming the object outlives the erase.
			if (in == nullptr)
				return false;

			if (auto *gep = dyn_cast<GetElementPtrInst> (in)) {
				if (gep->getPointerOperand () != value)
					return false;

				if (cluster.derived.insert (gep).second) {
					cluster.users.insert (gep);
					queue.push_back (gep);
				}

				continue;
			}

			if (auto *store = dyn_cast<StoreInst> (in)) {
				// A volatile or an atomic store is an event of its own.
				if (!store->isSimple () || store->getValueOperand () == value)
					return false;

				cluster.users.insert (store);
				continue;
			}

			if (is_lifetime_mark (*in)) {
				cluster.users.insert (in);
				continue;
			}

			auto *call = dyn_cast<CallInst> (in);

			if (barrier == nullptr || call == nullptr)
				return false;
			if (call->getCalledFunction () != barrier)
				return false;
			if (call->getArgOperand (0) != value || call->getArgOperand (1) == value)
				return false;

			cluster.users.insert (call);
		}
	}

	return true;
}

/// Takes \p alloc and \p cluster away, and adds to \p orphans each value the
/// cluster stops reading.
void
erase (CallInst &alloc, const Cluster &cluster, SmallVectorImpl<WeakTrackingVH> &orphans)
{
	for (Instruction *in : cluster.users)
		for (Value *operand : in->operands ())
			if (auto *made = dyn_cast<Instruction> (operand))
				if (!cluster.derived.contains (made))
					orphans.push_back (made);

	for (Value *operand : alloc.operands ())
		if (auto *made = dyn_cast<Instruction> (operand))
			orphans.push_back (made);

	// Each getelementptr was found while the walk read the users of its own
	// pointer, so a value comes before what reads it. Reverse order therefore
	// erases each user while it has no uses left.
	for (Instruction *in : reverse (cluster.users))
		in->eraseFromParent ();

	alloc.eraseFromParent ();
}

} // namespace

bool
erase_dead_allocations (Function &f)
{
	const Function *barrier = f.getParent ()->getFunction (gc_barrier_name);
	bool changed = false;

	for (bool again = true; again;) {
		SmallVector<WeakTrackingVH, 8> orphans;

		again = false;

		// A cluster stops reading what it stored, so one object dies with
		// another. The scan repeats safely: an allocation is never a user of
		// another one, so no erase reaches a site the scan below still holds.
		for (StringRef name : { alloc_object_name, alloc_vector_name })
			for (CallBase *site : builtin_sites (f, name)) {
				// A site inside a clause is an invoke. Erasing one asks
				// for a repair of the pads its edges name, so it keeps
				// its object.
				auto *call = dyn_cast<CallInst> (site);
				Cluster cluster;

				if (call == nullptr || !collect (*call, barrier, cluster))
					continue;

				erase (*call, cluster, orphans);
				changed = again = true;
			}

		// Outside the scans above, which hold pointers this can erase.
		RecursivelyDeleteTriviallyDeadInstructionsPermissive (orphans);
	}

	return changed;
}

PreservedAnalyses
EraseDeadAllocationsPass::run (Function &f, FunctionAnalysisManager &)
{
	if (!erase_dead_allocations (f))
		return PreservedAnalyses::all ();

	PreservedAnalyses preserved;

	// The erase moves no branch and splits no block.
	preserved.preserveSet<CFGAnalyses> ();
	return preserved;
}

} // namespace mono

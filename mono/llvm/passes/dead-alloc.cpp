/**
 * \file
 * \brief The walk that reads an allocation as dead, and the erase that takes it
 * away with everything under it.
 */

#include "dead-alloc.hpp"

#include "alloc-func.hpp"
#include "builtins.hpp"
#include "escape.hpp"
#include "gc-barrier.hpp"

#include <llvm/ADT/DenseMap.h>
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

#include <utility>

using namespace llvm;

namespace mono {
namespace {

/// The allocation sites this run is still willing to take away.
using Candidates = SmallPtrSet<const CallInst *, 8>;

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

/// The declarations a barrier site names, or null where the module holds none.
struct Barriers {
	const Function *store = nullptr;
	const Function *range = nullptr;
	const Function *value_copy = nullptr;

	bool holds (const Function *called) const
	{
		return called != nullptr
		       && (called == store || called == range || called == value_copy);
	}
};

/**
 * Walks what \p alloc reaches and fills \p cluster in. Returns false where a
 * user reads the object, or where the erase cannot take that user away.
 *
 * It accepts a getelementptr and a lifetime mark. It accepts the writes this
 * backend makes into an object as well: a store with its barrier, a value copy,
 * and the memcpy or memmove such a copy becomes where the fold opens it.
 * Everything else is a refusal. A load reads the object back, and a call hands
 * it outside the function.
 *
 * The object has to be the destination rather than what is written, with one
 * exception. A write of the object into another object goes through a pointer
 * this walk never reaches, so the barrier beside it is all the walk finds.
 * Taking that barrier for one of our own erases an object something else now
 * holds. The exception is a destination \p candidates still holds: that object
 * dies in this same round, and its own cluster is what takes the write away.
 * The walk reads the users of each derived value, so a refusal at either operand
 * catches the pair.
 */
bool
collect (CallInst &alloc, const Barriers &barriers, const Candidates &candidates,
         Cluster &cluster)
{
	SmallVector<Value *, 8> queue;

	// Whether the object a write names as its destination dies in this round.
	auto destination_dies = [&] (Value *destination) {
		auto *holder = dyn_cast_or_null<CallInst> (allocation_behind (destination));

		return holder != nullptr && candidates.contains (holder);
	};

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
				if (!store->isSimple ())
					return false;

				if (store->getValueOperand () == value
				    && store->getPointerOperand () != value) {
					if (!destination_dies (store->getPointerOperand ()))
						return false;

					continue;
				}

				cluster.users.insert (store);
				continue;
			}

			if (auto *copy = dyn_cast<MemTransferInst> (in)) {
				// A volatile copy is an event of its own.
				if (copy->isVolatile ())
					return false;

				if (copy->getRawSource () == value && copy->getRawDest () != value) {
					if (!destination_dies (copy->getRawDest ()))
						return false;

					continue;
				}

				if (copy->getRawDest () != value)
					return false;

				cluster.users.insert (copy);
				continue;
			}

			if (is_lifetime_mark (*in)) {
				cluster.users.insert (in);
				continue;
			}

			auto *call = dyn_cast<CallInst> (in);

			if (call == nullptr)
				return false;
			if (!barriers.holds (call->getCalledFunction ()))
				return false;

			if (call->getArgOperand (1) == value && call->getArgOperand (0) != value) {
				if (!destination_dies (call->getArgOperand (0)))
					return false;

				continue;
			}

			if (call->getArgOperand (0) != value)
				return false;

			cluster.users.insert (call);
		}
	}

	return true;
}

/**
 * Whether \p alloc can be taken away, with \p cluster naming what goes with it.
 *
 * Two questions answer this, and they are asked apart. Can anything outside the
 * function reach the object? Does anything inside the function read it back?
 * The first is what `allocation_escapes ()` answers, and \p candidates is what
 * vouches for an object this one is stored into. The second is the walk above,
 * which also names every instruction the erase deletes.
 */
bool
is_dead (CallInst &alloc, const Barriers &barriers, const Candidates &candidates,
         Cluster &cluster)
{
	auto keeps_it_inside = [&] (CallBase &holder) {
		auto *call = dyn_cast<CallInst> (&holder);

		return call != nullptr && candidates.contains (call);
	};

	if (allocation_escapes (alloc, keeps_it_inside))
		return false;

	return collect (alloc, barriers, candidates, cluster);
}

/// Takes every candidate away with its cluster, and adds to \p orphans each
/// value the group stops reading.
void
erase (const SmallVectorImpl<CallInst *> &sites, const Candidates &candidates,
       DenseMap<CallInst *, Cluster> &clusters, SmallVectorImpl<WeakTrackingVH> &orphans)
{
	SmallSetVector<Instruction *, 32> doomed;

	for (CallInst *site : sites) {
		if (!candidates.contains (site))
			continue;

		for (Instruction *in : clusters[site].users)
			doomed.insert (in);

		doomed.insert (site);
	}

	for (Instruction *in : doomed)
		for (Value *operand : in->operands ())
			if (auto *made = dyn_cast<Instruction> (operand))
				if (!doomed.contains (made))
					orphans.push_back (made);

	/*
	 * Two passes over the group. A store of one dead object into another reads
	 * a value the group itself makes, so no single order erases each
	 * instruction while it has no uses left. With every operand dropped first,
	 * the order does not matter.
	 */
	for (Instruction *in : doomed)
		in->dropAllReferences ();

	for (Instruction *in : doomed)
		in->eraseFromParent ();
}

} // namespace

bool
erase_dead_allocations (Function &f)
{
	const Module &m = *f.getParent ();
	const Barriers barriers = { m.getFunction (gc_barrier_name),
		                    m.getFunction (gc_value_copy_name) };
	bool changed = false;

	for (bool again = true; again;) {
		SmallVector<WeakTrackingVH, 8> orphans;
		SmallVector<CallInst *, 8> sites;
		Candidates candidates;
		DenseMap<CallInst *, Cluster> clusters;

		again = false;

		for (StringRef name : { alloc_object_name, alloc_vector_name })
			for (CallBase *site : builtin_sites (f, name)) {
				// A site inside a clause is an invoke. Erasing one asks
				// for a repair of the pads its edges name, so it keeps
				// its object.
				if (auto *call = dyn_cast<CallInst> (site)) {
					sites.push_back (call);
					candidates.insert (call);
				}
			}

		/*
		 * Every site starts as a candidate and drops out at the first user
		 * that observes it. Two objects that hold each other are dead only as
		 * a pair, which one pass over the sites cannot decide: each arm reads
		 * the other's answer. Dropping one candidate can take another's reason
		 * away, so the loop repeats until a pass drops none.
		 */
		for (bool settling = true; settling;) {
			settling = false;
			clusters.clear ();

			for (CallInst *site : sites) {
				Cluster cluster;

				if (!candidates.contains (site))
					continue;

				if (is_dead (*site, barriers, candidates, cluster)) {
					clusters[site] = std::move (cluster);
					continue;
				}

				candidates.erase (site);
				settling = true;
			}
		}

		if (!candidates.empty ()) {
			erase (sites, candidates, clusters, orphans);
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

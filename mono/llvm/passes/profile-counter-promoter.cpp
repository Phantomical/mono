#include "profile-counter-promoter.hpp"

#include <llvm/ADT/DenseMap.h>
#include <llvm/ADT/MapVector.h>
#include <llvm/ADT/STLExtras.h>
#include <llvm/ADT/SmallPtrSet.h>
#include <llvm/ADT/SmallVector.h>
#include <llvm/Analysis/LoopInfo.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Dominators.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/IntrinsicInst.h>
#include <llvm/IR/Intrinsics.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/Transforms/Utils/PromoteMemToReg.h>

#include <algorithm>
#include <cstdint>

using namespace llvm;

namespace mono {
namespace {

// LLVM declares options of its own for most of these, and reads them from
// inside its lowering pass. The names here therefore carry a prefix. Both
// sets are registered in one process, and a second registration under a
// name the first took is an error at startup.

cl::opt<bool> promotion_enabled ("mono-counter-promotion", cl::init (PromotionPolicy ().enabled),
                                 cl::Hidden,
                                 cl::desc ("Hoist profile counter updates out of loops"));

cl::opt<unsigned> max_per_loop ("mono-max-counter-promotions-per-loop",
                                cl::init (PromotionPolicy ().max_per_loop), cl::Hidden,
                                cl::desc ("Counters one loop may promote"));

// LLVM stops at three. A `for` with three early returns already has four
// exiting blocks, so at that setting most hand-written loops keep their
// per-turn writes. What eight admits is a write-back on each further exit,
// and one exit is taken per pass through the loop whatever the count. So
// the code at the exits is what holds eight down, not the time spent in
// them.
cl::opt<unsigned> max_exiting ("mono-speculative-counter-promotion-max-exiting",
                               cl::init (PromotionPolicy ().max_exiting), cl::Hidden,
                               cl::desc ("Exiting blocks past which a loop is left alone"));

// LLVM refuses such a loop, so that a profile read in the middle of a long
// one does not under-report it. Almost every loop a C# method ends with
// can be left through a return, so the refusal costs most of what
// promotion is worth here. A read that comes early loses only the turns
// the threads now in the loop took. Entry count is what takes a body to
// tier 2.
cl::opt<bool> skip_ret_exit_block ("mono-skip-ret-exit-block",
                                   cl::init (PromotionPolicy ().skip_ret_exit_block), cl::Hidden,
                                   cl::desc ("Leave a loop alone when a return can leave it"));

cl::opt<bool> speculative_promotion_to_loop (
	"mono-speculative-counter-promotion-to-loop",
	cl::init (PromotionPolicy ().speculative_promotion_to_loop), cl::Hidden,
	cl::desc ("Allow a speculative promotion to write back into a block that "
              "is itself inside a loop"));

cl::opt<bool>
	iterative_promotion ("mono-iterative-counter-promotion",
                         cl::init (PromotionPolicy ().iterative), cl::Hidden,
                         cl::desc ("Promote a write-back again out of the loop it landed in"));

using Group = SmallVector<InstrProfIncrementInst *, 4>;

/// The counters one loop increments, keyed by counter index.
///
/// A MapVector, so the write-backs come out in the order the increments were
/// found rather than in the order a DenseMap buckets the indices.
using Groups = MapVector<uint64_t, Group>;

using Pending = DenseMap<Loop *, Groups>;

bool
is_promotion_possible (const Loop &loop, ArrayRef<BasicBlock *> exits)
{
	// A catchswitch is an EH pad, and both the first non-phi and the last
	// instruction of its block. So getFirstInsertionPt () steps past it onto
	// end (), and a write-back has nowhere to go.
	if (any_of (exits, [] (const BasicBlock *exit) {
			return isa<CatchSwitchInst> (exit->getTerminator ());
		}))
		return false;

	// Without dedicated exits, a block the loop leaves for is reached from
	// outside it as well. A write-back there would count turns that never
	// happened. Without a preheader there is nowhere to start the count that
	// runs once per entry to the loop.
	return loop.hasDedicatedExits () && loop.getLoopPreheader () != nullptr;
}

unsigned max_promotions_in (const PromotionPolicy &policy, Loop &loop, LoopInfo &li,
                            const Pending &pending);

unsigned
promotion_budget (const PromotionPolicy &policy, Loop &loop, LoopInfo &li, const Pending &pending,
                  ArrayRef<BasicBlock *> exits)
{
	SmallVector<BasicBlock *, 8> exiting;

	loop.getExitingBlocks (exiting);

	// One exiting block is not speculation: the loop is left through it
	// whatever path it took, so the write-back runs once per entry.
	if (exiting.size () == 1)
		return policy.max_per_loop;

	if (exiting.size () > policy.max_exiting)
		return 0;

	if (policy.speculative_promotion_to_loop)
		return policy.max_per_loop;

	// Each write-back this loop leaves in an enclosing loop is a candidate
	// that loop then has to hold a register for. Take what it has already
	// promised away from what this loop can spend. That keeps a nest from
	// committing more registers than any one of its loops would.
	unsigned budget = policy.max_per_loop;

	for (BasicBlock *exit : exits) {
		Loop *target = li.getLoopFor (exit);

		if (target == nullptr)
			continue;

		unsigned there = max_promotions_in (policy, *target, li, pending);
		unsigned claimed = pending.lookup (target).size ();

		budget = std::min (budget, std::max (there, claimed) - claimed);
	}

	return budget;
}

unsigned
max_promotions_in (const PromotionPolicy &policy, Loop &loop, LoopInfo &li, const Pending &pending)
{
	SmallVector<BasicBlock *, 8> exits;

	loop.getExitBlocks (exits);

	if (!is_promotion_possible (loop, exits))
		return 0;

	return promotion_budget (policy, loop, li, pending, exits);
}

class CounterPromoter {
public:
	CounterPromoter (const PromotionPolicy &policy, Loop &loop, LoopInfo &li, Pending &pending,
	                 SmallVectorImpl<AllocaInst *> &accumulators)
		: policy_ (policy), loop_ (loop), li_ (li), pending_ (pending), accumulators_ (accumulators)
	{
		SmallVector<BasicBlock *, 8> exits;
		SmallPtrSet<BasicBlock *, 8> seen;

		loop_.getExitBlocks (exits);

		if (!is_promotion_possible (loop_, exits))
			return;

		// A loop reaches one exit block over several edges, and the block
		// wants one write-back however many of them there are.
		for (BasicBlock *exit : exits)
			if (seen.insert (exit).second) {
				exits_.push_back (exit);
				insert_points_.push_back (&*exit->getFirstInsertionPt ());
			}
	}

	bool run ();

private:
	void promote (uint64_t index, const Group &sites);

	const PromotionPolicy &policy_;
	Loop &loop_;
	LoopInfo &li_;
	Pending &pending_;
	SmallVectorImpl<AllocaInst *> &accumulators_;
	SmallVector<BasicBlock *, 8> exits_;
	SmallVector<Instruction *, 8> insert_points_;
};

bool
CounterPromoter::run ()
{
	// Empty when the loop has no exit, and when the constructor refused it. A
	// count taken out of a loop with no exit is lost rather than deferred.
	if (exits_.empty ())
		return false;

	if (policy_.skip_ret_exit_block)
		for (BasicBlock *exit : exits_)
			if (isa<ReturnInst> (exit->getTerminator ()))
				return false;

	unsigned budget = promotion_budget (policy_, loop_, li_, pending_, exits_);

	if (budget == 0)
		return false;

	// Move the groups out of the map first. Promoting writes back into the
	// entry for whichever loop an exit lands in, and growing the map moves
	// what a reference into it names.
	Groups groups = std::move (pending_[&loop_]);

	pending_.erase (&loop_);

	unsigned promoted = 0;

	for (const auto &group : groups) {
		if (group.second.empty ())
			continue;

		promote (group.first, group.second);

		if (++promoted >= budget)
			break;
	}

	return promoted != 0;
}

void
CounterPromoter::promote (uint64_t index, const Group &sites)
{
	InstrProfIncrementInst *first = sites.front ();
	Function &f = *first->getFunction ();
	Type *i64 = Type::getInt64Ty (f.getContext ());

	// Read off before the sites are erased. Every increment of one counter
	// carries the same four: the profile name and hash for the function, the
	// counter count, and the slot.
	Value *name = first->getArgOperand (0);
	Value *hash = first->getArgOperand (1);
	Value *counters = first->getArgOperand (2);
	Value *slot = first->getArgOperand (3);

	// The accumulator is a stack slot, which PromoteMemToReg turns into phis
	// once every loop is done. LLVM's own instrumentation lowering builds
	// the same chain with an SSAUpdater instead, which needs every value
	// available up front. A stack slot needs no such bookkeeping: each
	// store is the last value plus a step, the way a memory location
	// already works.
	BasicBlock &entry = f.getEntryBlock ();
	IRBuilder<> at_entry (&entry, entry.getFirstInsertionPt ());
	AllocaInst *accumulator = at_entry.CreateAlloca (i64, nullptr, "pgocount.promoted");

	accumulators_.push_back (accumulator);

	IRBuilder<> at_preheader (loop_.getLoopPreheader ()->getTerminator ());

	at_preheader.CreateStore (ConstantInt::get (i64, 0), accumulator);

	for (InstrProfIncrementInst *site : sites) {
		IRBuilder<> at_site (site);
		Value *step = site->getStep ();
		Value *sum = at_site.CreateAdd (at_site.CreateLoad (i64, accumulator), step);

		at_site.CreateStore (sum, accumulator);
		site->eraseFromParent ();
	}

	for (unsigned i = 0; i < exits_.size (); ++i) {
		IRBuilder<> at_exit (insert_points_[i]);
		Value *live = at_exit.CreateLoad (i64, accumulator);
		auto *back = cast<InstrProfIncrementInst> (at_exit.CreateIntrinsic (
			Intrinsic::instrprof_increment_step, {name, hash, counters, slot, live}));

		if (!policy_.iterative)
			continue;

		if (Loop *target = li_.getLoopFor (exits_[i]))
			pending_[target][index].push_back (back);
	}
}

bool
promote_in (Function &f, const PromotionPolicy &policy)
{
	DominatorTree dt (f);
	LoopInfo li (dt);
	Pending pending;

	for (Instruction &i : instructions (f))
		if (auto *increment = dyn_cast<InstrProfIncrementInst> (&i))
			if (Loop *loop = li.getLoopFor (increment->getParent ()))
				pending[loop][increment->getIndex ()->getZExtValue ()].push_back (increment);

	if (pending.empty ())
		return false;

	SmallVector<Loop *, 8> loops = li.getLoopsInPreorder ();
	SmallVector<AllocaInst *, 8> accumulators;
	bool changed = false;

	// Innermost first, so the write-backs a loop leaves behind are candidates
	// for the loop around it and a nest hoists all the way out.
	for (Loop *loop : reverse (loops))
		changed |= CounterPromoter (policy, *loop, li, pending, accumulators).run ();

	// The CFG did not move, so the tree the loops were found over still
	// describes the function.
	if (!accumulators.empty ())
		PromoteMemToReg (accumulators, dt);

	return changed;
}

} // namespace

PromotionPolicy
PromotionPolicy::from_command_line ()
{
	PromotionPolicy policy;

	// Qualified where an option shares a name with the field it sets.
	policy.enabled = promotion_enabled;
	policy.max_per_loop = mono::max_per_loop;
	policy.max_exiting = mono::max_exiting;
	policy.skip_ret_exit_block = mono::skip_ret_exit_block;
	policy.speculative_promotion_to_loop = mono::speculative_promotion_to_loop;
	policy.iterative = iterative_promotion;

	return policy;
}

ProfileCounterPromoterPass::ProfileCounterPromoterPass ()
	: ProfileCounterPromoterPass (PromotionPolicy::from_command_line ())
{
}

ProfileCounterPromoterPass::ProfileCounterPromoterPass (PromotionPolicy policy) : policy_ (policy)
{
}

PreservedAnalyses
ProfileCounterPromoterPass::run (Module &m, ModuleAnalysisManager &)
{
	if (!policy_.enabled)
		return PreservedAnalyses::all ();

	bool changed = false;

	for (Function &f : m)
		if (!f.isDeclaration ())
			changed |= promote_in (f, policy_);

	return changed ? PreservedAnalyses::none () : PreservedAnalyses::all ();
}

} // namespace mono

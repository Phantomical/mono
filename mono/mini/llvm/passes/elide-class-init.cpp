/**
 * \file
 * elide-class-init.cpp - the class-init barrier redundancy analysis.
 *
 * See elide-class-init.hpp for what "redundant" means here and why the two
 * facts it looks for are sound.
 */

#include <mono/mini/mini.h>
#include <mono/metadata/abi-details.h>
#include <mono/metadata/object-internals.h>

/* Has to come after the mono headers - mono-tls.h puts PIC back in scope. */
#ifdef PIC
#undef PIC
#endif

#include "elide-class-init.hpp"

#include <llvm/ADT/APInt.h>
#include <llvm/ADT/DenseMap.h>
#include <llvm/ADT/DepthFirstIterator.h>
#include <llvm/ADT/PostOrderIterator.h>
#include <llvm/ADT/STLExtras.h>
#include <llvm/ADT/SmallPtrSet.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/CFG.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/IR/InstrTypes.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/IntrinsicInst.h>
#include <llvm/IR/Metadata.h>
#include <llvm/IR/Module.h>

#ifdef ENABLE_LLVM

using namespace llvm;

namespace {

/* The two halves of a barrier, as the translator tags them. */
const char kInitTag[] = "mono.class-init";
const char kCheckTag[] = "mono.class-init-check";

/*
 * The address a constant pointer expression names. The translator writes the
 * vtable as a literal, so both barrier shapes bottom out in an inttoptr of an
 * integer: the prologue guard folds the field offset into it, the front-end's
 * in-body barrier leaves a getelementptr on top.
 */
bool
constant_address (const Value *v, const DataLayout &dl, uint64_t *addr)
{
	APInt offset (dl.getIndexTypeSizeInBits (v->getType ()), 0);
	const Value *base = v->stripAndAccumulateConstantOffsets (dl, offset, true);

	auto *expr = dyn_cast<ConstantExpr> (base);
	if (!expr || expr->getOpcode () != Instruction::IntToPtr)
		return false;

	auto *literal = dyn_cast<ConstantInt> (expr->getOperand (0));
	if (!literal)
		return false;

	*addr = literal->getZExtValue () + offset.getSExtValue ();
	return true;
}

/* The vtable a tagged trigger call initializes. */
bool
trigger_vtable (const CallBase *call, const DataLayout &dl, uint64_t *vtable)
{
	if (call->arg_size () != 1)
		return false;

	const Value *arg = call->getArgOperand (0);
	if (auto *literal = dyn_cast<ConstantInt> (arg)) {
		*vtable = literal->getZExtValue ();
		return true;
	}

	/* Belt and braces: the icall's argument is an intptr today, not a pointer. */
	return arg->getType ()->isPointerTy () && constant_address (arg, dl, vtable);
}

struct DecodedCheck {
	uint64_t vtable;
	/* The successor taken when the class turned out to be initialized already. */
	unsigned inited_succ;
};

/*
 * Read a tagged check branch back into the class it tests and the edge along
 * which that class is known initialized.
 *
 * Both of those come from the comparison rather than from the tag or from the
 * successor order, because neither survives the optimizer intact: SimplifyCFG
 * inverts a branch by negating the predicate and swapping the successors, and
 * carries the metadata across unchanged. The comparison is the only part that
 * has to keep meaning what it says.
 */
bool
decode_check (const BranchInst *branch, const DataLayout &dl, DecodedCheck *out)
{
	if (!branch->isConditional ())
		return false;

	/* The translator wraps the compare in llvm.expect to weight the branch. */
	const Value *cond = branch->getCondition ();
	while (auto *expect = dyn_cast<IntrinsicInst> (cond)) {
		if (expect->getIntrinsicID () != Intrinsic::expect)
			break;
		cond = expect->getArgOperand (0);
	}

	auto *cmp = dyn_cast<ICmpInst> (cond);
	if (!cmp || !cmp->isEquality ())
		return false;

	auto is_zero = [] (const Value *v) {
		auto *literal = dyn_cast<ConstantInt> (v);
		return literal && literal->isZero ();
	};

	const Value *tested = nullptr;
	if (is_zero (cmp->getOperand (1)))
		tested = cmp->getOperand (0);
	else if (is_zero (cmp->getOperand (0)))
		tested = cmp->getOperand (1);
	else
		return false;

	/* The byte is widened before the compare until InstCombine narrows it back. */
	while (auto *cast = dyn_cast<CastInst> (tested)) {
		if (!isa<ZExtInst> (cast) && !isa<SExtInst> (cast) && !isa<TruncInst> (cast))
			break;
		tested = cast->getOperand (0);
	}

	auto *load = dyn_cast<LoadInst> (tested);
	if (!load || !load->getType ()->isIntegerTy (8))
		return false;

	uint64_t addr;
	if (!constant_address (load->getPointerOperand (), dl, &addr))
		return false;

	const uint64_t field = MONO_STRUCT_OFFSET (MonoVTable, initialized);
	if (addr < field)
		return false;

	out->vtable = addr - field;
	/* `!= 0` is true when initialized, `== 0` is true when it still owes a cctor. */
	out->inited_succ = cmp->getPredicate () == ICmpInst::ICMP_NE ? 0 : 1;
	return true;
}

/* Everything tagged in one function, decoded once and shared by all the classes. */
struct Barriers {
	DenseMap<const CallBase *, uint64_t> triggers;
	DenseMap<const BranchInst *, DecodedCheck> checks;
	/* The distinct vtables, in first-seen order, so results are deterministic. */
	SmallVector<uint64_t, 2> classes;
};

Barriers
collect_barriers (Function &f)
{
	const DataLayout &dl = f.getParent ()->getDataLayout ();
	Barriers out;

	for (Instruction &ins : instructions (f)) {
		uint64_t vtable;

		if (auto *call = dyn_cast<CallBase> (&ins)) {
			if (!call->getMetadata (kInitTag) || !trigger_vtable (call, dl, &vtable))
				continue;
			out.triggers [call] = vtable;
		} else if (auto *branch = dyn_cast<BranchInst> (&ins)) {
			DecodedCheck check;

			if (!branch->getMetadata (kCheckTag) || !decode_check (branch, dl, &check))
				continue;
			out.checks [branch] = check;
			vtable = check.vtable;
		} else {
			continue;
		}

		if (!is_contained (out.classes, vtable))
			out.classes.push_back (vtable);
	}

	return out;
}

/* What a block contributes to the fact for one class. */
struct BlockFacts {
	/* A trigger among the block's non-terminator instructions: by the time
	 * control leaves the block it has returned, so every outgoing edge has it. */
	bool body_trigger = false;
	/* A trigger that *is* the terminator, i.e. an invoke inside a try region.
	 * Only its normal edge carries the fact - down the unwind edge the cctor
	 * threw and the class is emphatically not initialized. */
	const InvokeInst *invoke_trigger = nullptr;
	/* A check, and the edge it leaves by when the byte was already set. */
	const BasicBlock *inited_edge = nullptr;
};

/*
 * Mark every trigger call for VTABLE that is already covered where it stands.
 *
 * A forward must-analysis: the fact holds on entry to a block only if it held
 * along every incoming edge, which is what makes a completed barrier count -
 * one arm checked, the other called, and the join has it either way. Interior
 * blocks start optimistic and are refined downward to the fixpoint, so a fact
 * generated inside a loop still covers a later trigger in the same loop.
 * Nothing ever clears the fact - a class does not become uninitialized again -
 * so there is no kill set to carry.
 */
void
cover_class (Function &f, uint64_t vtable, const Barriers &barriers,
             const SmallPtrSetImpl<BasicBlock *> &live,
             SmallPtrSetImpl<const CallBase *> &covered)
{
	auto trigger_for = [&] (const Instruction &ins) {
		const auto *call = dyn_cast<CallBase> (&ins);
		if (!call)
			return false;
		auto it = barriers.triggers.find (call);
		return it != barriers.triggers.end () && it->second == vtable;
	};

	DenseMap<const BasicBlock *, BlockFacts> facts;
	DenseMap<const BasicBlock *, bool> known_in;
	BasicBlock *entry = &f.getEntryBlock ();

	for (BasicBlock *bb : live) {
		BlockFacts bf;

		for (Instruction &ins : *bb) {
			if (!trigger_for (ins))
				continue;
			if (auto *invoke = dyn_cast<InvokeInst> (&ins))
				bf.invoke_trigger = invoke;
			else
				bf.body_trigger = true;
		}

		if (auto *branch = dyn_cast<BranchInst> (bb->getTerminator ())) {
			auto it = barriers.checks.find (branch);
			if (it != barriers.checks.end () && it->second.vtable == vtable)
				bf.inited_edge = branch->getSuccessor (it->second.inited_succ);
		}

		facts [bb] = bf;
		known_in [bb] = bb != entry;
	}

	auto edge_known = [&] (const BasicBlock *from, const BasicBlock *to) {
		const BlockFacts &bf = facts [from];

		if (bf.body_trigger)
			return true;
		if (bf.invoke_trigger && to == bf.invoke_trigger->getNormalDest ())
			return true;
		if (bf.inited_edge == to)
			return true;
		return known_in [from];
	};

	/* Reverse post-order so most blocks settle in the first sweep. */
	ReversePostOrderTraversal<Function *> rpo (&f);
	bool changed = true;
	while (changed) {
		changed = false;

		for (BasicBlock *bb : rpo) {
			if (bb == entry || !live.count (bb))
				continue;

			bool in = true;
			for (BasicBlock *pred : predecessors (bb)) {
				/* A dead predecessor is not a path anyone takes. */
				if (!live.count (pred))
					continue;
				if (!edge_known (pred, bb)) {
					in = false;
					break;
				}
			}

			if (in != known_in [bb]) {
				known_in [bb] = in;
				changed = true;
			}
		}
	}

	for (BasicBlock *bb : live) {
		bool known = known_in [bb];

		for (Instruction &ins : *bb) {
			bool is_trigger = trigger_for (ins);

			if (is_trigger && known)
				covered.insert (cast<CallBase> (&ins));
			/* A trigger covers what follows it, never itself. */
			known = known || is_trigger;
		}
	}
}

} // namespace

SmallVector<mono::RedundantClassInit, 4>
mono::find_redundant_class_inits (Function &f)
{
	Barriers barriers = collect_barriers (f);
	if (barriers.triggers.empty ())
		return {};

	SmallPtrSet<BasicBlock *, 16> live;
	for (BasicBlock *bb : depth_first (&f.getEntryBlock ()))
		live.insert (bb);

	SmallPtrSet<const CallBase *, 4> covered;
	for (uint64_t vtable : barriers.classes)
		cover_class (f, vtable, barriers, live, covered);

	SmallVector<RedundantClassInit, 4> redundant;
	for (Instruction &ins : instructions (f)) {
		auto *call = dyn_cast<CallBase> (&ins);

		if (call && covered.count (call))
			redundant.push_back ({call, barriers.triggers.lookup (call)});
	}

	return redundant;
}

bool
mono::ClassInitElisionAnalysis::Result::is_redundant (const CallBase *call) const
{
	for (const RedundantClassInit &entry : redundant_) {
		if (entry.call == call)
			return true;
	}
	return false;
}

AnalysisKey mono::ClassInitElisionAnalysis::Key;

mono::ClassInitElisionAnalysis::Result
mono::ClassInitElisionAnalysis::run (Function &f, FunctionAnalysisManager &)
{
	return Result (find_redundant_class_inits (f));
}

#endif /* ENABLE_LLVM */

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
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Dominators.h>
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

} // namespace

SmallVector<mono::RedundantClassInit, 4>
mono::find_redundant_class_inits (Function &f, const DominatorTree &dt)
{
	const DataLayout &dl = f.getParent ()->getDataLayout ();

	SmallVector<std::pair<CallBase *, uint64_t>, 4> calls;
	SmallVector<std::pair<BranchInst *, DecodedCheck>, 4> checks;

	for (Instruction &ins : instructions (f)) {
		if (auto *call = dyn_cast<CallBase> (&ins)) {
			uint64_t vtable;

			if (call->getMetadata (kInitTag) && trigger_vtable (call, dl, &vtable))
				calls.emplace_back (call, vtable);
		} else if (auto *branch = dyn_cast<BranchInst> (&ins)) {
			DecodedCheck check;

			if (branch->getMetadata (kCheckTag) && decode_check (branch, dl, &check))
				checks.emplace_back (branch, check);
		}
	}

	SmallVector<RedundantClassInit, 4> redundant;

	for (auto &[call, vtable] : calls) {
		/*
		 * A dominating trigger call for the same class. Two calls can never
		 * dominate each other, so nothing here can pair a call with itself
		 * transitively - and a dominating call that is itself redundant is
		 * harmless: whatever dominates it dominates this call too, and shows up
		 * in this call's own search.
		 */
		bool found = false;

		for (auto &[other, other_vtable] : calls) {
			if (other == call || other_vtable != vtable)
				continue;
			if (dt.dominates (static_cast<const Instruction *> (other), call)) {
				redundant.push_back ({call, other, ClassInitFactKind::PriorCall, vtable});
				found = true;
				break;
			}
		}
		if (found)
			continue;

		/* Or a check for the same class whose "already initialized" edge dominates us. */
		for (auto &[branch, check] : checks) {
			if (check.vtable != vtable)
				continue;

			BasicBlockEdge edge (branch->getParent (), branch->getSuccessor (check.inited_succ));
			if (dt.dominates (edge, call->getParent ())) {
				redundant.push_back ({call, branch, ClassInitFactKind::InitializedEdge, vtable});
				break;
			}
		}
	}

	return redundant;
}

const mono::RedundantClassInit *
mono::ClassInitElisionAnalysis::Result::lookup (const CallBase *call) const
{
	for (const RedundantClassInit &entry : redundant_) {
		if (entry.call == call)
			return &entry;
	}
	return nullptr;
}

bool
mono::ClassInitElisionAnalysis::Result::invalidate (Function &f, const PreservedAnalyses &pa,
                                                    FunctionAnalysisManager::Invalidator &inv)
{
	auto checker = pa.getChecker<ClassInitElisionAnalysis> ();

	/* Every answer here is a dominance question, so a stale tree is a stale result. */
	return !(checker.preserved () || checker.preservedSet<AllAnalysesOn<Function>> ())
	       || inv.invalidate<DominatorTreeAnalysis> (f, pa);
}

AnalysisKey mono::ClassInitElisionAnalysis::Key;

mono::ClassInitElisionAnalysis::Result
mono::ClassInitElisionAnalysis::run (Function &f, FunctionAnalysisManager &fam)
{
	return Result (find_redundant_class_inits (f, fam.getResult<DominatorTreeAnalysis> (f)));
}

#endif /* ENABLE_LLVM */

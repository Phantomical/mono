#include "hoist-guard-vtable.hpp"

#include "vtable-func.hpp"

#include "runtime/options.hpp"

#include <llvm/ADT/DenseMap.h>
#include <llvm/ADT/SmallVector.h>
#include <llvm/Analysis/LoopInfo.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/MDBuilder.h>
#include <llvm/Transforms/Utils/BasicBlockUtils.h>

using namespace llvm;

namespace mono {

namespace {

/// Whether \p user compares \p read with a non-null constant.
bool
compares_to_a_vtable (const User *user, const Value *read)
{
	const auto *compare = dyn_cast<ICmpInst> (user);

	if (compare == nullptr || !compare->isEquality ())
		return false;

	const Value *other = compare->getOperand (compare->getOperand (0) == read ? 1 : 0);

	return isa<Constant> (other) && !isa<ConstantPointerNull> (other);
}

/// The outermost loop around \p read that \p object does not change across.
Loop *
outermost_invariant (LoopInfo &loops, const LoadInst *read, Value *object)
{
	Loop *outer = nullptr;

	for (Loop *at = loops.getLoopFor (read->getParent ());
	     at != nullptr && at->isLoopInvariant (object); at = at->getParentLoop ())
		outer = at;

	return outer;
}

/* A stale proxy vtable still misses comparisons with class vtables. */
PHINode *
read_in_preheader (BasicBlock *head, Value *object, const LoadInst &like, LoopInfo &loops)
{
	IRBuilder<> b (head->getTerminator ());
	Value *present = b.CreateIsNotNull (object);
	Instruction *then = SplitBlockAndInsertIfThen (
		present, head->getTerminator (), false,
		MDBuilder (object->getContext ()).createLikelyBranchWeights (), nullptr, &loops);
	BasicBlock *loaded = then->getParent ();
	BasicBlock *tail = loaded->getSingleSuccessor ();

	b.SetInsertPoint (then);

	LoadInst *early = b.CreateAlignedLoad (like.getType (), object, like.getAlign (), "vtable");

	early->setMetadata (LLVMContext::MD_tbaa, like.getMetadata (LLVMContext::MD_tbaa));

	PHINode *vtable = PHINode::Create (like.getType (), 2, "guard_vtable", tail->begin ());

	vtable->addIncoming (early, loaded);
	vtable->addIncoming (ConstantPointerNull::get (cast<PointerType> (like.getType ())), head);
	return vtable;
}

} // namespace

PreservedAnalyses
HoistGuardVtablePass::run (Function &f, FunctionAnalysisManager &fam)
{
	if (!hoist_guard_vtable ())
		return PreservedAnalyses::all ();

	LoopInfo &loops = fam.getResult<LoopAnalysis> (f);

	if (loops.empty ())
		return PreservedAnalyses::all ();

	SmallVector<LoadInst *, 8> reads;

	for (Instruction &i : instructions (f))
		if (auto *load = dyn_cast<LoadInst> (&i))
			if (loops.getLoopFor (load->getParent ()) != nullptr)
				reads.push_back (load);

	// Reuse one preheader read for all guards on the same receiver and loop.
	DenseMap<std::pair<Value *, Loop *>, PHINode *> hoisted;

	for (LoadInst *read : reads) {
		Value *object = object_vtable_read (read);

		if (object == nullptr)
			continue;

		Loop *loop = outermost_invariant (loops, read, object);

		if (loop == nullptr || loop->getLoopPreheader () == nullptr)
			continue;

		SmallVector<Use *, 4> compares;

		for (Use &use : read->uses ())
			if (compares_to_a_vtable (use.getUser (), read)
			    && loop->contains (cast<Instruction> (use.getUser ())))
				compares.push_back (&use);

		if (compares.empty ())
			continue;

		PHINode *&vtable = hoisted[{ object, loop }];

		if (vtable == nullptr)
			vtable = read_in_preheader (loop->getLoopPreheader (), object, *read, loops);

		for (Use *use : compares)
			use->set (vtable);
	}

	return hoisted.empty () ? PreservedAnalyses::all () : PreservedAnalyses::none ();
}

} // namespace mono

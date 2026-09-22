#include "class-init-guard.hpp"
#include "class-init-elision.hpp"

#include "mono/llvm/analysis/strip-casts.hpp"

#include "mono/metadata/abi-details.h"
#include "mono/metadata/object-internals.h"

#include <llvm/ADT/SmallVector.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/InstrTypes.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>

using namespace llvm;

namespace mono {
namespace {

/// Return whether the class still needs its constructor run.
/// The vtable may be an integer when it comes from a shared body's rgctx fetch.
Value *
needs_class_init (IRBuilder<> &b, Value *vtable)
{
	if (!vtable->getType ()->isPointerTy ())
		vtable = b.CreateIntToPtr (vtable, b.getPtrTy ());

	Value *flag = b.CreateAlignedLoad (
		b.getInt8Ty (),
		b.CreateGEP (b.getInt8Ty (), vtable,
	                    b.getInt32 (MONO_STRUCT_OFFSET (MonoVTable, initialized))),
		Align (1), "cctor_finished");

	return b.CreateICmpEQ (flag, b.getInt8 (0), "needs_class_init");
}

/// Copy phi inputs from one predecessor to another.
void
share_phis_with (BasicBlock *block, BasicBlock *from, BasicBlock *to)
{
	for (PHINode &phi : block->phis ()) {
		int at = phi.getBasicBlockIndex (from);

		if (at >= 0)
			phi.addIncoming (phi.getIncomingValue (at), to);
	}
}

/// Execute the call only when the vtable still needs initialization.
void
guard_call (CallBase &call, Value *vtable)
{
	LLVMContext &c = call.getContext ();
	Function *f = call.getFunction ();
	BasicBlock *head = call.getParent ();
	auto *unwinds = dyn_cast<InvokeInst> (&call);

	BasicBlock *tail = unwinds != nullptr
	                           ? unwinds->getNormalDest ()
	                           : head->splitBasicBlock (call.getIterator (), "class_init_done");
	BasicBlock *pad = unwinds != nullptr ? unwinds->getUnwindDest () : nullptr;
	BasicBlock *slow = BasicBlock::Create (c, "class_init_run", f, tail);

	// The original block remains the skip predecessor; add the new slow path to
	// successor phi nodes.
	share_phis_with (tail, head, slow);
	if (pad != nullptr)
		pad->replacePhiUsesWith (head, slow);

	call.removeFromParent ();
	call.insertInto (slow, slow->end ());

	if (unwinds == nullptr)
		IRBuilder<> (slow).CreateBr (tail);

	if (Instruction *stale = head->getTerminatorOrNull ())
		stale->eraseFromParent ();

	IRBuilder<> guard (head);

	guard.SetCurrentDebugLocation (call.getDebugLoc ());
	guard.CreateCondBr (needs_class_init (guard, vtable), slow, tail);
}

} // namespace

PreservedAnalyses
ClassInitGuardPass::run (Function &f, FunctionAnalysisManager &)
{
	SmallVector<std::pair<CallBase *, Value *>, 8> sites;

	for (Function &decl : f.getParent ()->functions ()) {
		if (!decl.isDeclaration () || !decl.hasFnAttribute (class_init_attribute))
			continue;

		for (User *user : decl.users ()) {
			auto *call = dyn_cast<CallBase> (user);

			if (call == nullptr || call->getFunction () != &f || call->getCalledFunction () != &decl)
				continue;
			if (call->arg_size () < 1 || !call->use_empty ())
				continue;

			// Strip conversions introduced by earlier late-stage passes.
			auto *vtable = const_cast<Value *> (strip_casts (call->getArgOperand (0)));
			if (!vtable->getType ()->isIntOrPtrTy ())
				continue;

			sites.push_back ({ call, vtable });
		}
	}

	if (sites.empty ())
		return PreservedAnalyses::all ();

	for (auto &[call, vtable] : sites)
		guard_call (*call, vtable);

	return PreservedAnalyses::none ();
}

} // namespace mono

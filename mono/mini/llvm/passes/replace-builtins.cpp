/**
 * \file
 * \brief Implementation of the corlib builtin replacements. See
 * replace-builtins.hpp for what they are and what they rely on.
 */

#include <config.h>

#include <mono/mini/mini.h>

#ifdef PIC
#undef PIC
#endif

#include "inliner-support.hpp"
#include "replace-builtins.hpp"

#include <llvm/ADT/SmallVector.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/IR/InstrTypes.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/PassManager.h>
#include <llvm/Passes/PassBuilder.h>

#ifdef ENABLE_LLVM

using namespace llvm;

namespace {

struct Candidate {
	CallBase *call;
	/* Whether the length parameter is a signed type, and so needs clamping. */
	bool signed_length;
};

/*
 * Whether METHOD is System.Buffer:Memcpy (byte*, byte*, len). Matched on the
 * method rather than on the mangled callee symbol, which is lossy.
 */
bool
match_buffer_memcpy (MonoMethod *method, bool *signed_length)
{
	MonoClass *klass = method->klass;

	if (m_class_get_image (klass) != mono_defaults.corlib)
		return false;
	if (strcmp (m_class_get_name_space (klass), "System") != 0 ||
	    strcmp (m_class_get_name (klass), "Buffer") != 0)
		return false;
	if (strcmp (method->name, "Memcpy") != 0)
		return false;

	/*
	 * The shape decides, the same one intrinsics.c matches for the sibling
	 * Memmove it turns into OP_MEMMOVE: static, void, (byte*, byte*, integer).
	 */
	MonoMethodSignature *sig = mono_method_signature_internal (method);
	if (!sig || sig->hasthis || sig->param_count != 3)
		return false;
	if (sig->ret->type != MONO_TYPE_VOID)
		return false;
	if (sig->params [0]->type != MONO_TYPE_PTR || sig->params [1]->type != MONO_TYPE_PTR)
		return false;

	switch (sig->params [2]->type) {
	case MONO_TYPE_I:
	case MONO_TYPE_I4:
	case MONO_TYPE_I8:
		*signed_length = true;
		return true;
	case MONO_TYPE_U:
	case MONO_TYPE_U4:
	case MONO_TYPE_U8:
		*signed_length = false;
		return true;
	default:
		return false;
	}
}

/*
 * The byte count to hand the intrinsic, widened to i64. A signed length is
 * clamped at zero on the way: negative copies nothing today, and sign-extending
 * one would ask for a copy of nearly the whole address space.
 */
Value *
copy_length (IRBuilder<> &builder, Value *len, bool is_signed)
{
	Type *i64 = builder.getInt64Ty ();
	Value *wide = builder.CreateIntCast (len, i64, is_signed);

	if (!is_signed)
		return wide;

	Constant *zero = ConstantInt::get (i64, 0);
	return builder.CreateSelect (builder.CreateICmpSGT (wide, zero), wide, zero);
}

} // namespace

PreservedAnalyses
mono::ReplaceMonoBuiltins::run (Function &f, FunctionAnalysisManager &)
{
	SmallVector<Candidate, 4> found;

	for (Instruction &ins : instructions (f)) {
		auto *call = dyn_cast<CallBase> (&ins);
		if (!call || call->arg_size () != 3)
			continue;

		/* Null for an indirect or virtual call, neither of which names a method. */
		Function *callee = call->getCalledFunction ();
		if (!callee)
			continue;

		MonoMethod *method = mono::managed_method_from_symbol (callee->getName ().str ().c_str ());
		if (!method)
			continue;

		Candidate candidate = { call, false };
		if (!match_buffer_memcpy (method, &candidate.signed_length))
			continue;

		/*
		 * The signature said (ptr, ptr, integer); if the call site disagrees it
		 * is not the call we think it is, and the intrinsic builder would assert
		 * on it.
		 */
		if (!call->getArgOperand (0)->getType ()->isPointerTy () ||
		    !call->getArgOperand (1)->getType ()->isPointerTy () ||
		    !call->getArgOperand (2)->getType ()->isIntegerTy ())
			continue;

		found.push_back (candidate);
	}

	if (found.empty ())
		return PreservedAnalyses::all ();

	bool cfg_changed = false;

	for (const Candidate &candidate : found) {
		CallBase *call = candidate.call;
		IRBuilder<> builder (call);

		Value *dst = call->getArgOperand (0);
		Value *src = call->getArgOperand (1);
		Value *bytes = copy_length (builder, call->getArgOperand (2), candidate.signed_length);

		/* Nothing is known about how either end is aligned. */
		const MaybeAlign align (1);
		CallInst *lowered = builder.CreateMemCpy (dst, align, src, align, bytes);
		lowered->setDebugLoc (call->getDebugLoc ());

		if (auto *invoke = dyn_cast<InvokeInst> (call)) {
			/*
			 * A call in a try region arrives as an invoke, and an invoke is a
			 * terminator: the normal edge becomes the only one, and the handler
			 * loses a predecessor along with the call that could have thrown
			 * into it. The intrinsic cannot throw, so there is nothing left to
			 * reach the handler from here.
			 */
			BasicBlock *from = invoke->getParent ();

			BranchInst::Create (invoke->getNormalDest (), invoke);
			invoke->getUnwindDest ()->removePredecessor (from);
			cfg_changed = true;
		}

		call->eraseFromParent ();
	}

	PreservedAnalyses pa;
	if (!cfg_changed)
		pa.preserveSet<CFGAnalyses> ();
	return pa;
}

void
mono::ReplaceMonoBuiltins::register_pass (PassBuilder &pb)
{
	/*
	 * Once up front, for a call the translator emitted into the root directly.
	 */
	pb.registerPipelineStartEPCallback (
		[] (ModulePassManager &mpm, OptimizationLevel) {
			mpm.addPass (createModuleToFunctionPassAdaptor (ReplaceMonoBuiltins ()));
		});

	/*
	 * And in the peephole slot, which the function simplification pipeline
	 * reaches several times per round. That is where the interesting call sites
	 * show up: Memcpy is rarely called by the root itself, it arrives inside a
	 * body the tier-1 inliner just folded in, and by then the length is often a
	 * constant. Running here also puts the intrinsic in front of the SROA and
	 * InstCombine behind it, which are what turn it into loads and stores and
	 * retire the alloca it was copying into.
	 *
	 * Runs that match nothing walk the function and preserve everything, so the
	 * repetition is cheap, and lowering twice is not possible - the call leaves
	 * with the rewrite.
	 */
	pb.registerPeepholeEPCallback (
		[] (FunctionPassManager &fpm, OptimizationLevel) {
			fpm.addPass (ReplaceMonoBuiltins ());
		});
}

#endif /* ENABLE_LLVM */

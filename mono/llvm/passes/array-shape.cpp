/**
 * \file
 * \brief Lowering `mono.array.shape.*` calls into header reads.
 *
 * The reads restate what ves_icall_System_Array_GetLength () and
 * ves_icall_System_Array_GetLowerBound () answer for dimension zero. A szarray
 * carries no bounds vector: its length is MonoArray.max_length and its lower
 * bound is zero. An array with a shape carries one MonoArrayBounds for each
 * dimension, so the lowering branches on that pointer and reads the first one.
 *
 * The layout comes from mono's own headers. What arrives on the declaration is
 * what no header states: which accessor the site is and the method it falls
 * back to. The exception a null array raises rides on the site itself, as the
 * type token behind the dimension.
 */

#include "array-shape.hpp"

#include "array-address.hpp"

#include "mono/metadata/abi-details.h"
#include "mono/metadata/class-internals.h"
#include "mono/metadata/object-internals.h"

#include <llvm/ADT/SmallVector.h>
#include <llvm/ADT/StringRef.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/InstrTypes.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/MDBuilder.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/ErrorHandling.h>

using namespace llvm;

namespace mono {
namespace {

/// Reads one field of the array header.
///
/// Both bounds fields are scalar typedefs, so the size alone is the layout.
Value *
load_field (IRBuilder<> &b, Value *base, int32_t offset, unsigned bytes)
{
	Value *slot = b.CreateInBoundsGEP (b.getInt8Ty (), base, b.getInt64 (offset));
	LoadInst *load = b.CreateAlignedLoad (b.getIntNTy (bytes * 8), slot, Align (bytes));

	mark_array_header_load (load);
	return load;
}

/// Reads the pointer to the array's bounds vector.
Value *
load_bounds (IRBuilder<> &b, Value *array)
{
	LoadInst *load = b.CreateAlignedLoad (
		PointerType::get (b.getContext (), 0),
		b.CreateInBoundsGEP (b.getInt8Ty (), array,
	                             b.getInt64 (MONO_STRUCT_OFFSET (MonoArray, bounds))),
		Align (sizeof (void *)));

	mark_array_header_load (load);
	return load;
}

/// Weights a branch's throw edge as unlikely, matching what the translator
/// puts on its own guards.
void
mark_unlikely (BranchInst *branch)
{
	MDBuilder md (branch->getContext ());

	branch->setMetadata (LLVMContext::MD_prof, md.createBranchWeights (1, 1000));
}

/// Replaces the site with the header reads that answer for dimension zero.
void
lower_call (CallBase *site, bool lower_bound)
{
	Function *fn = site->getFunction ();
	LLVMContext &ctx = fn->getContext ();
	Module &m = *fn->getParent ();
	Value *array = site->getArgOperand (0);
	Type *i32 = Type::getInt32Ty (ctx);

	/*
	 * Carve the containing block: everything from the call on becomes the
	 * continuation, and the reads grow as blocks in between.
	 */
	BasicBlock *head = site->getParent ();
	BasicBlock *cont = head->splitBasicBlock (site, "array_shape_done");
	head->getTerminator ()->eraseFromParent ();

	BasicBlock *raise = BasicBlock::Create (ctx, "array_shape_throw", fn);
	BasicBlock *live = BasicBlock::Create (ctx, "array_shape_live", fn, cont);
	BasicBlock *flat = BasicBlock::Create (ctx, "array_shape_szarray", fn, cont);
	BasicBlock *shaped = BasicBlock::Create (ctx, "array_shape_bounds", fn, cont);
	IRBuilder<> b (head);
	BranchInst *guard = b.CreateCondBr (b.CreateIsNull (array), raise, live);

	mark_unlikely (guard);

	/*
	 * The tag ImplicitNullChecks looks for, on the shape emit_null_check ()
	 * describes: the not-taken arm dereferences the pointer that was tested,
	 * near enough to its head for the hardware to trap on it.
	 */
	guard->setMetadata (LLVMContext::MD_make_implicit, MDNode::get (ctx, {}));

	b.SetInsertPoint (live);

	Value *bounds = load_bounds (b, array);

	b.CreateCondBr (b.CreateIsNull (bounds), flat, shaped);

	IRBuilder<> fb (flat);
	Value *without_bounds =
		lower_bound ? static_cast<Value *> (fb.getInt32 (0))
		            : fb.CreateZExtOrTrunc (
				      load_field (fb, array,
	                                          MONO_STRUCT_OFFSET (MonoArray, max_length),
	                                          sizeof (mono_array_size_t)),
				      i32);

	fb.CreateBr (cont);

	IRBuilder<> sb (shaped);
	Value *held = load_field (
		sb, bounds,
		lower_bound ? MONO_STRUCT_OFFSET (MonoArrayBounds, lower_bound)
		            : MONO_STRUCT_OFFSET (MonoArrayBounds, length),
		lower_bound ? sizeof (mono_array_lower_bound_t) : sizeof (mono_array_size_t));
	// A lower bound is signed and a length is not.
	Value *first = lower_bound ? sb.CreateSExtOrTrunc (held, i32)
	                           : sb.CreateZExtOrTrunc (held, i32);

	sb.CreateBr (cont);

	IRBuilder<> cb (cont, cont->begin ());
	PHINode *result = cb.CreatePHI (i32, 2, "dim_result");

	result->addIncoming (without_bounds, flat);
	result->addIncoming (first, shaped);

	FunctionCallee thrower = m.getOrInsertFunction (
		"mono_llvm_throw_corlib_exception", Type::getVoidTy (ctx), i32);
	if (auto *decl = dyn_cast<Function> (thrower.getCallee ())) {
		decl->setDoesNotReturn ();
		decl->addFnAttr (Attribute::Cold);
	}

	/*
	 * The throw unwinds. An invoke site hands its landing pad on to it, so the
	 * exception stays catchable exactly where the accessor call was.
	 */
	IRBuilder<> rb (raise);
	// Behind the dimension, which is where the translator puts it.
	Value *token = site->getArgOperand (2);

	site->replaceAllUsesWith (result);
	if (auto *invoke = dyn_cast<InvokeInst> (site)) {
		BasicBlock *pad = invoke->getUnwindDest ();
		BasicBlock *unreached = BasicBlock::Create (ctx, "array_shape_unreach", fn);

		rb.CreateInvoke (thrower, unreached, pad, { token });
		IRBuilder<> (unreached).CreateUnreachable ();
		for (PHINode &phi : pad->phis ())
			phi.replaceIncomingBlockWith (cont, raise);

		BasicBlock *normal = invoke->getNormalDest ();

		invoke->eraseFromParent ();
		IRBuilder<> (cont).CreateBr (normal);
	} else {
		rb.CreateCall (thrower, { token });
		rb.CreateUnreachable ();
		site->eraseFromParent ();
	}
}

/// Puts the site back on the accessor, which is where a dimension this pass
/// cannot read has to be answered.
void
restore_call (CallBase *site, Function *target)
{
	IRBuilder<> b (site);
	// Without the exception token, which the accessor raises for itself.
	SmallVector<Value *, 2> args (site->arg_begin (), site->arg_end () - 1);
	CallBase *lowered;

	if (auto *invoke = dyn_cast<InvokeInst> (site)) {
		lowered = b.CreateInvoke (target, invoke->getNormalDest (),
		                          invoke->getUnwindDest (), args);
	} else {
		CallInst *call = b.CreateCall (target, args);

		/* A managed frame is observable. emit_protected_call says why. */
		call->setTailCallKind (CallInst::TCK_NoTail);
		lowered = call;
	}

	lowered->setCallingConv (target->getCallingConv ());
	site->replaceAllUsesWith (lowered);
	site->eraseFromParent ();
}

} // namespace

PreservedAnalyses
ArrayShapePass::run (Module &m, ModuleAnalysisManager &)
{
	SmallVector<Function *, 4> decls;

	for (Function &f : m)
		if (f.isDeclaration () && f.getName ().starts_with (array_shape_prefix))
			decls.push_back (&f);

	if (decls.empty ())
		return PreservedAnalyses::all ();

	for (Function *decl : decls) {
		StringRef kind = decl->getFnAttribute (array_shape_attribute).getValueAsString ();
		StringRef name =
			decl->getFnAttribute (array_shape_target_attribute).getValueAsString ();
		Function *target = m.getFunction (name);

		if (kind != array_shape_length && kind != array_shape_lower_bound)
			report_fatal_error (Twine ("unknown ") + array_shape_attribute + " kind '"
			                    + kind + "' on " + decl->getName ());

		if (target == nullptr)
			report_fatal_error (Twine ("array shape ") + decl->getName ()
			                    + " falls back to " + name
			                    + ", which is not declared");

		if (decl->arg_size () != target->arg_size () + 1)
			report_fatal_error (Twine ("array shape ") + decl->getName ()
			                    + " does not take the arguments of " + name
			                    + " and an exception token");

		SmallVector<CallBase *, 8> sites;

		for (User *user : decl->users ())
			if (auto *site = dyn_cast<CallBase> (user))
				sites.push_back (site);

		for (CallBase *site : sites) {
			auto *dimension = dyn_cast<ConstantInt> (site->getArgOperand (1));

			if (dimension != nullptr && dimension->isZero ())
				lower_call (site, kind == array_shape_lower_bound);
			else if (finalize)
				restore_call (site, target);
		}

		if (!finalize)
			continue;

		/* Anything left is a use no lowering understands, so fail loudly. */
		if (!decl->use_empty ())
			report_fatal_error (Twine ("unlowered use of ") + decl->getName ());
		decl->eraseFromParent ();
	}

	return PreservedAnalyses::none ();
}

} // namespace mono

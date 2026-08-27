/**
 * \file
 * \brief Writing an allocation back as the call its collector serves.
 */

#include "alloc-func.hpp"

#include "builtins.hpp"

#include "mono/metadata/abi-details.h"

#include <llvm/ADT/SmallVector.h>
#include <llvm/IR/Attributes.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/InstrTypes.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/ModRef.h>

using namespace llvm;

namespace mono {
namespace {

StringRef
name_of (AllocShape shape, bool erasable)
{
	if (shape == AllocShape::object)
		return erasable ? alloc_object_name : alloc_object_kept_name;

	return erasable ? alloc_vector_name : alloc_vector_kept_name;
}

/// \p args, reshaped to what \p callee declares, and cut to the parameters it
/// has.
///
/// A managed allocator takes its second operand as a native integer and the
/// icall takes an int32. The second operand reaches either width.
SmallVector<Value *, 2>
adapt_to_callee (IRBuilder<> &b, Function *callee, ArrayRef<Value *> args)
{
	FunctionType *type = callee->getFunctionType ();
	SmallVector<Value *, 2> adapted;

	for (unsigned i = 0; i < args.size () && i < type->getNumParams (); ++i) {
		Type *want = type->getParamType (i);
		Value *have = args[i];

		if (have->getType () == want)
			adapted.push_back (have);
		else if (want->isPointerTy ())
			adapted.push_back (b.CreateIntToPtr (have, want));
		else if (have->getType ()->isPointerTy ())
			adapted.push_back (b.CreatePtrToInt (have, want));
		else
			adapted.push_back (b.CreateSExtOrTrunc (have, want));
	}

	return adapted;
}

/**
 * Rewrites one site into the call its allocator operand names.
 *
 * Where a clause protects the site, the site is the block's terminator. The new
 * call keeps its two successors and stays in the same block, so the pads and
 * the phis of both need no repair.
 */
void
lower (CallBase *site)
{
	auto *allocator = cast<Function> (site->getArgOperand (2)->stripPointerCasts ());
	IRBuilder<> b (site);

	b.SetCurrentDebugLocation (site->getDebugLoc ());

	SmallVector<Value *, 2> args =
		adapt_to_callee (b, allocator, { site->getArgOperand (0), site->getArgOperand (1) });
	CallBase *call;

	if (auto *invoke = dyn_cast<InvokeInst> (site)) {
		call = b.CreateInvoke (allocator, invoke->getNormalDest (),
		                       invoke->getUnwindDest (), args);
	} else {
		CallInst *plain = b.CreateCall (allocator, args);

		// A managed frame is observable, so a call in tail position stays a
		// call. emit_protected_call () marks the sites it writes the same way.
		plain->setTailCallKind (CallInst::TCK_NoTail);
		call = plain;
	}

	// The class of a fresh object sits on the site as metadata. What a length
	// says about the extent sits on the return attributes.
	call->copyMetadata (*site);
	call->setAttributes (call->getAttributes ().addRetAttributes (
		site->getContext (),
		AttrBuilder (site->getContext (), site->getAttributes ().getRetAttrs ())));

	site->replaceAllUsesWith (call);
	site->eraseFromParent ();
}

} // namespace

Function *
alloc_func_decl (Module &m, AllocShape shape, bool erasable)
{
	LLVMContext &c = m.getContext ();
	Type *ptr = PointerType::get (c, 0);
	Type *word = Type::getIntNTy (c, TARGET_SIZEOF_VOID_P * 8);
	Function *decl = builtin_decl (m, name_of (shape, erasable),
	                               FunctionType::get (ptr, { ptr, word, ptr }, false));

	decl->addRetAttr (Attribute::NoAlias);

	/*
	 * The allocator reads the vtable it is handed and writes the collector's
	 * own state, and a caller can name neither. A call with no memory effects
	 * clobbers every location instead. That is what keeps a store into a field
	 * from reaching a load below an allocation.
	 *
	 * `inaccessiblemem: readwrite` is the ceiling, because the nursery bump
	 * pointer keeps its new value for the next call. Under `memory(none)` the
	 * call is a function of its arguments alone, and `CallValue::canHandle ()`
	 * (`EarlyCSE.cpp`) merges two allocations of one class into one object.
	 *
	 * SGen copies a nursery survivor and rewrites the reference fields that
	 * name it, which is a write to memory a caller can see. No load a pass
	 * moves over the call reads one. `mini_gc_init ()` (`mono/mini/mini-gc.c`)
	 * sets no `thread_mark_func`, so `sgen-mono.c` scans the frame and the
	 * saved registers conservatively and pins whatever they name. A pinned
	 * object does not move. A precise mark function, or a major collector that
	 * compacts, makes this attribute wrong with no build error and no failing
	 * test.
	 */
	decl->setMemoryEffects (MemoryEffects::argMemOnly (ModRefInfo::Ref)
	                        | MemoryEffects::inaccessibleMemOnly (ModRefInfo::ModRef));

	if (erasable)
		decl->addFnAttr (Attribute::getWithAllocKind (c, AllocFnKind::Alloc));

	return decl;
}

bool
lower_allocations (Module &m)
{
	bool changed = false;

	for (AllocShape shape : { AllocShape::object, AllocShape::vector })
		for (bool erasable : { true, false }) {
			StringRef name = name_of (shape, erasable);

			for (CallBase *site : builtin_sites (m, name))
				lower (site);

			changed |= erase_builtin (m, name);
		}

	return changed;
}

} // namespace mono

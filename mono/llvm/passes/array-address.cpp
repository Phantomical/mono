/**
 * \file
 * \brief Lowering `mono.array.address.*` calls into element address arithmetic.
 *
 * The bounded case restates the runtime's ElementAddr wrapper
 * (emit_array_address_ilgen): walk the dimensions accumulating a linear index,
 * checking each partial index unsigned-below its dimension's length, then
 * scale by the element size into the array's vector. A one-dimensional
 * zero-based array has no bounds vector at all, so that case compares the
 * index against max_length directly. That is the same check element_address ()
 * inlines for ldelem and stelem. A failed check throws the corlib exception
 * whose type token the site carries behind its indices.
 *
 * The layout comes from mono's own headers. What arrives on the declaration is
 * what no header states: the rank, the element size, and whether the array
 * carries a bounds vector.
 */

#include "array-address.hpp"

#include "builtins.hpp"

#include "mono/metadata/abi-details.h"
#include "mono/metadata/class-internals.h"
#include "mono/metadata/object-internals.h"

#include <llvm/ADT/SmallVector.h>
#include <llvm/ADT/StringExtras.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Constants.h>
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

struct AddressSpec {
	uint64_t rank;
	uint64_t elem_size;
	bool bounded;
};

AddressSpec
parse_spec (const Function &decl)
{
	AddressSpec spec {};
	StringRef text =
		decl.getFnAttribute (array_address_attribute).getValueAsString ();

	while (!text.empty ()) {
		auto [pair, rest] = text.split (',');
		auto [key, value] = pair.split ('=');
		uint64_t number = 0;

		if (value.getAsInteger (10, number))
			report_fatal_error (Twine ("malformed ") + array_address_attribute
			                    + " attribute on " + decl.getName ());

		if (key == "rank")
			spec.rank = number;
		else if (key == "size")
			spec.elem_size = number;
		else if (key == "bounded")
			spec.bounded = number != 0;
		else
			report_fatal_error (Twine ("unknown key in ") + array_address_attribute
			                    + " attribute on " + decl.getName ());
		text = rest;
	}

	return spec;
}

/// Reads one field of the array header.
///
/// Every field it is asked for is a scalar typedef, so the size alone is the
/// layout.
Value *
load_field (IRBuilder<> &b, Value *base, uint64_t offset, unsigned bytes)
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

/// Takes a one-dimensional array's lower bound off index and returns the result.
///
/// This branches, so it leaves the builder in a new block rather than the one it
/// was given. The blocks it makes are inserted in front of continuation, which is
/// where the rest of the lowering goes.
///
/// The runtime allocates a one-dimensional array whose lower bound is zero with
/// no bounds vector at all, so an absent vector means a lower bound of zero.
Value *
subtract_lower_bound (IRBuilder<> &b, Value *array, Value *index,
                      BasicBlock *continuation)
{
	LLVMContext &ctx = b.getContext ();
	Function *fn = b.GetInsertBlock ()->getParent ();
	Type *i32 = b.getInt32Ty ();
	Value *bounds = load_bounds (b, array);
	BasicBlock *from = b.GetInsertBlock ();
	BasicBlock *have = BasicBlock::Create (ctx, "array_addr_lb", fn, continuation);
	BasicBlock *merge = BasicBlock::Create (ctx, "array_addr_idx", fn, continuation);

	b.CreateCondBr (b.CreateIsNull (bounds), merge, have);

	IRBuilder<> hb (have);
	Value *lower = hb.CreateZExtOrTrunc (
		load_field (hb, bounds, MONO_STRUCT_OFFSET (MonoArrayBounds, lower_bound),
	                    sizeof (mono_array_lower_bound_t)),
		i32);

	hb.CreateBr (merge);
	b.SetInsertPoint (merge);

	PHINode *bound = b.CreatePHI (i32, 2);

	bound->addIncoming (b.getInt32 (0), from);
	bound->addIncoming (lower, have);
	return b.CreateSub (index, bound);
}

void
lower_call (CallBase *site, const AddressSpec &spec)
{
	Function *fn = site->getFunction ();
	LLVMContext &ctx = fn->getContext ();
	Module &m = *fn->getParent ();
	Value *array = site->getArgOperand (0);

	/*
	 * Carve the containing block: everything from the call on becomes the
	 * continuation, and the checks grow as a chain of blocks in between.
	 */
	BasicBlock *head = site->getParent ();
	BasicBlock *cont = head->splitBasicBlock (site, "array_addr_done");
	head->getTerminator ()->eraseFromParent ();

	BasicBlock *raise = BasicBlock::Create (ctx, "array_addr_throw", fn);
	IRBuilder<> b (head);
	Type *i32 = b.getInt32Ty ();

	auto check = [&] (Value *bad) {
		BasicBlock *ok = BasicBlock::Create (ctx, "array_addr_ok", fn, cont);

		mark_unlikely (b.CreateCondBr (bad, raise, ok));
		b.SetInsertPoint (ok);
	};

	Value *linear;
	if (!spec.bounded || spec.rank == 1) {
		linear = b.CreateZExtOrTrunc (site->getArgOperand (1), i32);

		if (spec.bounded)
			linear = subtract_lower_bound (b, array, linear, cont);

		Value *length = load_field (b, array,
		                            MONO_STRUCT_OFFSET (MonoArray, max_length),
		                            sizeof (mono_array_size_t));

		check (b.CreateICmpUGE (b.CreateZExt (linear, b.getInt64Ty ()),
		                        b.CreateZExtOrTrunc (length, b.getInt64Ty ())));
	} else {
		Value *bounds = load_bounds (b, array);

		linear = nullptr;
		for (uint64_t dim = 0; dim < spec.rank; ++dim) {
			uint64_t at = dim * sizeof (MonoArrayBounds);
			Value *lower = b.CreateZExtOrTrunc (
				load_field (b, bounds,
			                    at + MONO_STRUCT_OFFSET (MonoArrayBounds,
			                                             lower_bound),
			                    sizeof (mono_array_lower_bound_t)),
				i32);
			Value *length = b.CreateZExtOrTrunc (
				load_field (b, bounds,
			                    at + MONO_STRUCT_OFFSET (MonoArrayBounds, length),
			                    sizeof (mono_array_size_t)),
				i32);
			Value *relative =
				b.CreateSub (site->getArgOperand (1 + (unsigned) dim), lower);

			check (b.CreateICmpUGE (relative, length));
			linear = linear == nullptr
			                 ? relative
			                 : b.CreateAdd (b.CreateMul (linear, length), relative);
		}
	}

	Value *vector = b.CreateInBoundsGEP (
		b.getInt8Ty (), array, b.getInt64 (MONO_STRUCT_OFFSET (MonoArray, vector)));
	Value *address = b.CreateInBoundsGEP (
		b.getInt8Ty (), vector,
		b.CreateMul (b.CreateZExt (linear, b.getInt64Ty ()),
	                     b.getInt64 (spec.elem_size)));
	b.CreateBr (cont);

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
	// Behind the indices, which is where the translator puts it.
	Value *token = site->getArgOperand (1 + (unsigned) spec.rank);

	site->replaceAllUsesWith (address);
	if (auto *invoke = dyn_cast<InvokeInst> (site)) {
		BasicBlock *pad = invoke->getUnwindDest ();
		BasicBlock *unreached = BasicBlock::Create (ctx, "array_addr_unreach", fn);

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

} // namespace

bool
lower_array_addresses (Module &m)
{
	SmallVector<Function *, 4> decls;

	for (Function &f : m)
		if (f.isDeclaration () && f.getName ().starts_with (array_address_prefix))
			decls.push_back (&f);

	if (decls.empty ())
		return false;

	for (Function *decl : decls) {
		AddressSpec spec = parse_spec (*decl);

		for (CallBase *site : builtin_sites (m, decl->getName ()))
			lower_call (site, spec);

		/* Anything left is a use no lowering understands, so fail loudly. */
		if (!decl->use_empty ())
			report_fatal_error (Twine ("unlowered use of ") + decl->getName ());
		decl->eraseFromParent ();
	}

	return true;
}

} // namespace mono

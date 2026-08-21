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
 * whose type token the declaration carries.
 *
 * Every number the arithmetic needs - field offsets and widths, the element
 * size, the exception token - arrives in the declaration's attribute, written
 * by the translator, which is what keeps mono's headers out of this file.
 */

#include "array-address.hpp"

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

/// What one declaration's attribute spells out.
struct AddressSpec {
	uint64_t rank;
	uint64_t elem_size;
	bool bounded;
	uint64_t token;
	uint64_t bounds_offset;
	uint64_t max_length_offset;
	uint64_t max_length_bytes;
	uint64_t vector_offset;
	uint64_t bounds_stride;
	uint64_t length_offset;
	uint64_t length_bytes;
	uint64_t lower_bound_offset;
	uint64_t lower_bound_bytes;
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
		else if (key == "token")
			spec.token = number;
		else if (key == "bounds")
			spec.bounds_offset = number;
		else if (key == "maxlen")
			spec.max_length_offset = number;
		else if (key == "maxlen_bytes")
			spec.max_length_bytes = number;
		else if (key == "vector")
			spec.vector_offset = number;
		else if (key == "stride")
			spec.bounds_stride = number;
		else if (key == "blen")
			spec.length_offset = number;
		else if (key == "blen_bytes")
			spec.length_bytes = number;
		else if (key == "blb")
			spec.lower_bound_offset = number;
		else if (key == "blb_bytes")
			spec.lower_bound_bytes = number;
		else
			report_fatal_error (Twine ("unknown key in ") + array_address_attribute
			                    + " attribute on " + decl.getName ());
		text = rest;
	}

	return spec;
}

Value *
load_field (IRBuilder<> &b, Value *base, uint64_t offset, uint64_t bytes)
{
	Value *slot = b.CreateInBoundsGEP (b.getInt8Ty (), base, b.getInt64 (offset));

	return b.CreateAlignedLoad (b.getIntNTy ((unsigned) bytes * 8), slot,
	                            Align (bytes));
}

/// Weights a branch's throw edge as unlikely, matching what the translator
/// puts on its own guards.
void
mark_unlikely (BranchInst *branch)
{
	MDBuilder md (branch->getContext ());

	branch->setMetadata (LLVMContext::MD_prof, md.createBranchWeights (1, 1000));
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

		if (spec.bounded) {
			/*
			 * The runtime allocates a one-dimensional array whose lower
			 * bound is zero without a bounds vector at all. Absent bounds
			 * mean a lower bound of zero.
			 */
			Value *bounds = b.CreateAlignedLoad (
				PointerType::get (ctx, 0),
				b.CreateInBoundsGEP (b.getInt8Ty (), array,
			                             b.getInt64 (spec.bounds_offset)),
				Align (sizeof (void *)));
			BasicBlock *from = b.GetInsertBlock ();
			BasicBlock *have =
				BasicBlock::Create (ctx, "array_addr_lb", fn, cont);
			BasicBlock *merge =
				BasicBlock::Create (ctx, "array_addr_idx", fn, cont);

			b.CreateCondBr (b.CreateIsNull (bounds), merge, have);

			IRBuilder<> hb (have);
			Value *lower = hb.CreateZExtOrTrunc (
				load_field (hb, bounds, spec.lower_bound_offset,
			                    spec.lower_bound_bytes),
				i32);

			hb.CreateBr (merge);
			b.SetInsertPoint (merge);

			PHINode *bound = b.CreatePHI (i32, 2);

			bound->addIncoming (b.getInt32 (0), from);
			bound->addIncoming (lower, have);
			linear = b.CreateSub (linear, bound);
		}

		Value *length =
			load_field (b, array, spec.max_length_offset, spec.max_length_bytes);

		check (b.CreateICmpUGE (b.CreateZExt (linear, b.getInt64Ty ()),
		                        b.CreateZExtOrTrunc (length, b.getInt64Ty ())));
	} else {
		Value *bounds = b.CreateAlignedLoad (
			PointerType::get (ctx, 0),
			b.CreateInBoundsGEP (b.getInt8Ty (), array,
		                             b.getInt64 (spec.bounds_offset)),
			Align (sizeof (void *)));

		linear = nullptr;
		for (uint64_t dim = 0; dim < spec.rank; ++dim) {
			uint64_t at = dim * spec.bounds_stride;
			Value *lower = b.CreateZExtOrTrunc (
				load_field (b, bounds, at + spec.lower_bound_offset,
			                    spec.lower_bound_bytes),
				i32);
			Value *length = b.CreateZExtOrTrunc (
				load_field (b, bounds, at + spec.length_offset,
			                    spec.length_bytes),
				i32);
			Value *relative =
				b.CreateSub (site->getArgOperand (1 + (unsigned) dim), lower);

			check (b.CreateICmpUGE (relative, length));
			linear = linear == nullptr
			                 ? relative
			                 : b.CreateAdd (b.CreateMul (linear, length), relative);
		}
	}

	Value *vector = b.CreateInBoundsGEP (b.getInt8Ty (), array,
	                                     b.getInt64 (spec.vector_offset));
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
	Value *token = rb.getInt32 ((uint32_t) spec.token);

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

PreservedAnalyses
ArrayAddressPass::run (Module &m, ModuleAnalysisManager &)
{
	SmallVector<Function *, 4> decls;

	for (Function &f : m)
		if (f.isDeclaration () && f.getName ().starts_with (array_address_prefix))
			decls.push_back (&f);

	if (decls.empty ())
		return PreservedAnalyses::all ();

	for (Function *decl : decls) {
		AddressSpec spec = parse_spec (*decl);
		SmallVector<CallBase *, 8> sites;

		for (User *user : decl->users ())
			if (auto *site = dyn_cast<CallBase> (user))
				sites.push_back (site);

		for (CallBase *site : sites)
			lower_call (site, spec);

		/* Anything left is a use no lowering understands, so fail loudly. */
		if (!decl->use_empty ())
			report_fatal_error (Twine ("unlowered use of ") + decl->getName ());
		decl->eraseFromParent ();
	}

	return PreservedAnalyses::none ();
}

} // namespace mono

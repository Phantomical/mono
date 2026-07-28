/**
 * \file
 * \brief Implementation of the write-barrier lowering. See wbarrier.hpp for
 * what the transform is and why each of its memory orderings is what it is.
 */

#include <config.h>

#include <mono/mini/mini.h>
#include <mono/metadata/gc-internals.h>

#ifdef PIC
#undef PIC
#endif

#include "wbarrier.hpp"

#include <llvm/ADT/SmallVector.h>
#include <llvm/IR/Attributes.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DataLayout.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/InlineAsm.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/IR/InstrTypes.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/MDBuilder.h>
#include <llvm/IR/Module.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/Support/MathExtras.h>
#include <llvm/Transforms/Utils/BasicBlockUtils.h>

#ifdef ENABLE_LLVM

using namespace llvm;

namespace {

/*
 * How much the mark is biased against running. Any page busy enough to be worth
 * a barrier gets dirtied once and then stays dirty until the next collection, so
 * the taken arm is genuinely rare - rare enough to be worth pushing out of line
 * so the fast path stays straight.
 */
constexpr uint32_t kMarkWeight = 1;
constexpr uint32_t kSkipWeight = 1023;

/*
 * The address the barrier was asked to mark. The wrapper's parameter is a native
 * int rather than a pointer (see mono_gc_get_write_barrier ()), but that is a
 * detail of how the signature was built, so accept either.
 */
Value *
mark_address (IRBuilder<> &b, CallInst *call, IntegerType *word_ty)
{
	Value *arg = call->getArgOperand (0);

	if (arg->getType ()->isPointerTy ())
		return b.CreatePtrToInt (arg, word_ty, "wb.addr");

	return b.CreateZExtOrTrunc (arg, word_ty, "wb.addr");
}

/*
 * A locked instruction is a full fence on x86 no matter what address it
 * touches (confirmed with a store-buffering litmus test - see wbarrier.hpp),
 * so it does not have to be the card word itself. Locking a thread-private
 * scratch byte that nothing ever reads back gets the same cross-thread
 * ordering as `mfence` without ever pulling the card word's cache line into
 * this core in Exclusive state, so a marker or another mutator reading that
 * word concurrently never contends with the fence.
 *
 * This has to be raw inline asm rather than an IR-level atomicrmw on the
 * scratch alloca: nothing ever loads the scratch byte back, so an atomicrmw
 * there is exactly the dead store DSE is designed to remove once it can see
 * the slot never escapes. `sideeffect` inline asm is opaque to every IR-level
 * optimization pass, the same way `fence` already was.
 */
void
emit_lock_fence (IRBuilder<> &b, Value *scratch)
{
	LLVMContext &ctx = b.getContext ();
	Type *scratch_ty = Type::getInt8Ty (ctx);
	FunctionType *asm_ty = FunctionType::get (Type::getVoidTy (ctx), {scratch->getType ()}, false);
	InlineAsm *lock_or = InlineAsm::get (asm_ty, "lock orb $$0, $0", "*m,~{memory}",
	                                    /* hasSideEffects */ true);

	CallInst *call = b.CreateCall (asm_ty, lock_or, {scratch});
	/* Opaque pointers carry no pointee type of their own, but an indirect
	 * ("*m") asm operand needs one to know how many bytes it is locking. */
	call->addParamAttr (0, Attribute::get (ctx, Attribute::ElementType, scratch_ty));
}

/* Replace one tagged call with the inline card mark. */
void
lower_barrier (CallInst *call, const mono::CardBitmap &bitmap, Value *fence_scratch)
{
	LLVMContext &ctx = call->getContext ();
	const DataLayout &dl = call->getModule ()->getDataLayout ();

	/*
	 * The collector reads this table a machine word at a time, so we have to set
	 * the bit the same way it counts them. boehm-gc.c asserts the two widths
	 * agree.
	 */
	IntegerType *word_ty = dl.getIntPtrType (ctx);
	const unsigned word_bits = word_ty->getBitWidth ();
	const Align word_align (word_bits / 8);

	IRBuilder<> b (call);

	Value *addr = mark_address (b, call, word_ty);

	/* index = (addr >> shift) & index_mask */
	Value *page = b.CreateLShr (addr, bitmap.shift, "wb.page");
	Value *index = b.CreateAnd (page, bitmap.index_mask, "wb.index");

	/* Split that into the word holding the bit, and the bit within the word. */
	Value *word_index = b.CreateLShr (index, Log2_32 (word_bits), "wb.word");
	Value *bit_index = b.CreateAnd (index, word_bits - 1, "wb.bit");
	Value *mask = b.CreateShl (ConstantInt::get (word_ty, 1), bit_index, "wb.mask");

	Value *table = b.CreateIntToPtr (ConstantInt::get (word_ty, bitmap.table),
	                                 PointerType::get (ctx, 0), "wb.table");
	Value *word_ptr = b.CreateGEP (word_ty, table, word_index, "wb.wordptr");

	/*
	 * x86 permits StoreLoad reordering - the guarded store can retire after
	 * this word's load - and that is exactly the reordering a concurrent
	 * incremental mark has to not see: it can observe the card as unmarked
	 * and the store as not-yet-visible in the same instant, decide the page
	 * is clean, and finish marking before the store ever appears. Ruling that
	 * out needs a real fence, seen by every thread, not just a compiler
	 * barrier against this thread's own reordering. See emit_lock_fence ().
	 */
	emit_lock_fence (b, fence_scratch);

	/* Monotonic: aliasing means two mutator threads can be RMW-ing and reading
	 * this same word concurrently, not just interleaved with a stopped-world
	 * GC, so the load needs a coherent order with those RMWs. See wbarrier.hpp. */
	LoadInst *cur = b.CreateLoad (word_ty, word_ptr, "wb.cur");
	cur->setAtomic (AtomicOrdering::Monotonic);
	cur->setAlignment (word_align);

	Value *unmarked = b.CreateICmpEQ (b.CreateAnd (cur, mask, "wb.hit"),
	                                  ConstantInt::get (word_ty, 0), "wb.unmarked");

	MDBuilder mdb (ctx);
	Instruction *mark = SplitBlockAndInsertIfThen (
		unmarked, call, /* Unreachable */ false,
		mdb.createBranchWeights (kMarkWeight, kSkipWeight));

	b.SetInsertPoint (mark);
	b.CreateAtomicRMW (AtomicRMWInst::Or, word_ptr, mask, word_align,
	                   AtomicOrdering::Release);

	call->eraseFromParent ();
}

} // namespace

std::optional<mono::CardBitmap>
mono::card_bitmap ()
{
	int shift = 0;
	gsize index_mask = 0;

	gpointer table = mono_gc_get_card_bitmap (&shift, &index_mask);
	if (!table)
		return std::nullopt;

	return CardBitmap { (uint64_t) (gsize) table, (unsigned) shift, (uint64_t) index_mask };
}

PreservedAnalyses
mono::WriteBarrierLoweringPass::run (Function &f, FunctionAnalysisManager &)
{
	SmallVector<CallInst *, 8> barriers;

	for (Instruction &ins : instructions (f)) {
		/*
		 * CallInst, not CallBase: lowering ends by deleting the barrier, and
		 * deleting an invoke would take its block's terminator with it. A
		 * barrier reached by invoke - a protected region, if the translator
		 * ever emits one there - is left as the call it already was, which is
		 * correct, just not inlined.
		 */
		auto *call = dyn_cast<CallInst> (&ins);
		if (call && call->getMetadata ("mono.wbarrier") && call->arg_size () >= 1)
			barriers.push_back (call);
	}

	if (barriers.empty ())
		return PreservedAnalyses::all ();

	/*
	 * One scratch byte, shared by every barrier this function lowers, for
	 * emit_lock_fence () to lock against. Built in the entry block so the
	 * backend treats it as a single static stack slot rather than a dynamic
	 * allocation re-run every time a barrier reached in a loop executes.
	 */
	BasicBlock &entry = f.getEntryBlock ();
	IRBuilder<> entry_b (&entry, entry.begin ());
	Value *fence_scratch = entry_b.CreateAlloca (Type::getInt8Ty (f.getContext ()),
	                                             nullptr, "wb.fence_scratch");

	/*
	 * Collected up front because lowering splits blocks out from under the
	 * iteration. The calls themselves survive their neighbours being split -
	 * they only move to the continuation block - so the list stays valid.
	 */
	for (CallInst *call : barriers)
		lower_barrier (call, bitmap_, fence_scratch);

	return PreservedAnalyses::none ();
}

void
mono::WriteBarrierLoweringPass::register_pass (PassBuilder &pb)
{
	std::optional<CardBitmap> bitmap = card_bitmap ();
	if (!bitmap)
		return;

	/*
	 * The peephole slot, which the function simplification pipeline reaches
	 * several times per round. Two things want it there rather than in one of
	 * the late slots. It is early, and until the barrier is lowered it is an
	 * opaque call that clobbers all memory, which costs every load and store
	 * near a reference store. And it runs again on each round's freshly
	 * materialized bodies, which is where the tier-1 inliner's barriers arrive.
	 *
	 * Runs that find no tag walk the function and preserve everything, so the
	 * repetition is cheap and lowering twice is not possible - the tag leaves
	 * with the call.
	 */
	pb.registerPeepholeEPCallback (
		[bitmap] (FunctionPassManager &fpm, OptimizationLevel) {
			fpm.addPass (WriteBarrierLoweringPass (*bitmap));
		});
}

#endif /* ENABLE_LLVM */

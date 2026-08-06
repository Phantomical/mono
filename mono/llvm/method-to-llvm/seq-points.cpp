/**
 * \file
 * \brief Soft-debugger sequence points.
 *
 * A sequence point is where the soft debugger is allowed to stop, and the way
 * it stops is that the code asks, at every one of them, whether anybody wants
 * it to: one word says whether single stepping is on, another whether this body
 * has a breakpoint in it, and either one being set calls the matching sdb
 * trampoline. The trampoline builds a MonoContext out of the frame it was
 * called from, hands it to the debugger and restores it on the way back, so
 * everything the debugger does with the stopped thread it does from there.
 *
 * mini gets the same effect by patching the instruction stream, which needs an
 * encoding the code generator promises; nothing promises that here, so the
 * breakpoint arm is a store into a word the code loads instead
 * (MonoLLVMBreakpointSwitch).
 *
 * The construct is
 *
 *     %ss = load volatile ptr, @single_step_tramp
 *     %bp = load volatile ptr, @this_body's_switch
 *     br (%ss | %bp) != 0, label %trap, label %cont
 *   trap:
 *     nop                          ; marked, and what the runtime records
 *     call %ss_or_nop ()
 *     call %bp_or_nop ()
 *     br label %cont
 *
 * Both calls sit in the one block, ahead of which is the marked nop, because
 * what maps a trampoline's return address back to a sequence point is
 * mono_find_prev_seq_point_for_native_offset (): the nearest recorded offset at
 * or before it. Two calls in two blocks could be laid out either side of
 * another sequence point's marker and resolve to it instead. Calling through a
 * do-nothing function is what lets the arm that is not set stay in the block.
 */

#include "method-to-llvm.hpp"
#include "seq-point-marker.hpp"

#include "mini-runtime.h"

#include "mono/metadata/domain-internals.h"

#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/InlineAsm.h>
#include <llvm/IR/Instructions.h>

namespace mono {

/*
 * Called where neither the debugger nor the stepper wants this sequence point,
 * which is what keeps both arms of the construct inside one basic block.
 */
extern "C" void
mono_llvm_seq_point_nop (void)
{
}

/// Emit the check that lets the soft debugger stop at ENCODED_IL, which is an
/// IL offset in the encoding seq-point-marker.hpp describes.
void
MethodLLVMEmitter::emit_seq_point (MonoIrBuilder &builder, uint32_t encoded_il)
{
	/*
	 * A filter body is a helper the runtime calls over the parent frame during
	 * the search pass; a breakpoint in one would stop the thread in the middle
	 * of dispatching an exception, which is not a place the debugger's frame
	 * model knows about.
	 */
	if (!mini_get_debug_options ()->gen_sdb_seq_points || filter_mode)
		return;
	if (il_scope == nullptr)
		return;

	if (bp_switch == nullptr)
		bp_switch = (MonoLLVMBreakpointSwitch *) mono_domain_alloc0 (
			cfg->domain, sizeof (MonoLLVMBreakpointSwitch));

	llvm::Type *ptr = llvm::PointerType::get (context (), 0);
	llvm::Type *i64 = builder.getInt64Ty ();
	llvm::FunctionType *hook =
		llvm::FunctionType::get (llvm::Type::getVoidTy (context ()), false);

	llvm::Constant *ss_slot = address_symbol (
		"mono_sdb_single_step_tramp", mono_arch_get_single_step_tramp_addr ());
	llvm::Constant *bp_slot = address_symbol (
		identity_symbol ("mono_sdb_breakpoint_switch", bp_switch), bp_switch);
	llvm::Constant *nop = address_symbol (
		"mono_llvm_seq_point_nop", (void *) mono_llvm_seq_point_nop);

	/*
	 * Both words are written by other threads - the stepper's from the debugger
	 * thread, the switch from whichever thread set the breakpoint - so neither
	 * load may be hoisted out of a loop or folded with the one at the previous
	 * sequence point.
	 */
	llvm::Value *ss = builder.CreateLoad (ptr, ss_slot, true, "sp.ss");
	llvm::Value *bp = builder.CreateLoad (ptr, bp_slot, true, "sp.bp");
	llvm::Value *armed = builder.CreateICmpNE (
		builder.CreateOr (builder.CreatePtrToInt (ss, i64),
	                          builder.CreatePtrToInt (bp, i64)),
		llvm::ConstantInt::get (i64, 0), "sp.armed");

	llvm::BasicBlock *trap =
		llvm::BasicBlock::Create (context (), "sp.trap", function);
	llvm::BasicBlock *cont =
		llvm::BasicBlock::Create (context (), "sp.cont", function);

	/*
	 * The two targets are chosen before the branch so that nothing but the
	 * marked nop and the calls themselves is left to emit inside the block.
	 */
	llvm::Value *ss_target =
		builder.CreateSelect (builder.CreateIsNotNull (ss), ss, nop);
	llvm::Value *bp_target =
		builder.CreateSelect (builder.CreateIsNotNull (bp), bp, nop);

	builder.CreateCondBr (armed, trap, cont);
	builder.SetInsertPoint (trap);

	/*
	 * The nop's address is what the runtime records for this sequence point.
	 * It is here so that both return addresses are after it and nothing else
	 * with an address of its own is between: an asm block cannot be reordered
	 * across a call, which is all the ordering this needs.
	 */
	uint32_t restore = (uint32_t) offset;

	set_il_location (builder, SEQ_POINT_MARKER_BASE + encoded_il);
	builder.CreateCall (llvm::InlineAsm::get (hook, "nop", "", true));
	set_il_location (builder, restore);

	llvm::CallInst *ss_call = builder.CreateCall (hook, ss_target);
	llvm::CallInst *bp_call = builder.CreateCall (hook, bp_target);

	/*
	 * A trampoline reads the frame it was called from and the debugger walks
	 * out of it, so neither call may become a jump.
	 */
	ss_call->setTailCallKind (llvm::CallInst::TCK_NoTail);
	bp_call->setTailCallKind (llvm::CallInst::TCK_NoTail);

	builder.CreateBr (cont);
	builder.SetInsertPoint (cont);
}

} // namespace mono

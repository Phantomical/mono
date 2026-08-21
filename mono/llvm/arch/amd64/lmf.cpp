/**
 * \file
 * \brief The LMF: the shape of a managed-to-native transition on this machine.
 */

/*
 * Before anything else, so that MonoError is the internal struct the rest of
 * the runtime passes around rather than the opaque public one.
 */
#include "runtime-error.hpp"

#include "arch/arch.hpp"

#include "mini.h"
#include "mini-runtime.h"

#include "mono/utils/mono-tls-inline.h"

// This breaks some LLVM headers
#undef PIC

#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/InlineAsm.h>
#include <llvm/IR/Intrinsics.h>

#include <cstring>
#include <optional>

namespace mono::arch {

/*
 * What lazy_frame_enter () reserves stack for. A plain LMF - not one of the
 * MonoLMFExt kinds - because an ordinary managed-to-native transition is what it
 * is: rsp and rbp below are the caller's, and its ip is read back off its own
 * stack from there.
 *
 * Crossing one of these zeroes the rest of the callee-saved registers, which a
 * managed-to-native wrapper makes up for by restoring them from its own frame
 * and there is no wrapper here. That costs only an exception raised inside the
 * compiler, and an abort does not come out that way: the resolver throws it
 * from the caller's frame, having already unlinked this.
 */
struct TransitionFrame {
	MonoLMF lmf;
	MonoLMF **addr;
};

static_assert (sizeof (TransitionFrame) <= managed_frame_size,
               "a caller does not reserve enough for a transition frame");

/*
 * What interp_frame_enter () reserves stack for. The zeroing that TransitionFrame
 * lives with is not affordable here: an interpreted method can throw anything, so
 * an exception crossing this frame is ordinary rather than exceptional, and there
 * is no wrapper above it to put the registers back. A MonoLMFTramp instead, whose
 * context the unwinder copies out verbatim.
 */
struct InterpTransitionFrame {
	MonoLMFTramp lmf;
	MonoContext ctx;
	MonoLMF **addr;
};

static_assert (sizeof (InterpTransitionFrame) <= interp_frame_size,
               "a caller does not reserve enough for an interpreter frame");

namespace {

/// Link a frame onto the chain, or record that there was no chain to link onto.
void
link_frame (TransitionFrame *frame, uint64_t caller_fp, uint64_t caller_sp)
{
	/*
	 * No LMF chain means a thread that has not run managed code, which reaches
	 * a stub only by the runtime calling one directly. There is no managed
	 * caller for a walk to find, and nothing to deliver an abort to either.
	 */
	frame->addr = mono_tls_get_lmf_addr ();

	if (!frame->addr)
		return;

	frame->lmf.rbp = caller_fp;
	frame->lmf.rsp = caller_sp;
	frame->lmf.previous_lmf = *frame->addr;
	*frame->addr = &frame->lmf;
}

/// Whether the frame was linked, having taken it back off the chain if it was.
bool
unlink_frame (TransitionFrame *frame)
{
	if (!frame->addr)
		return false;

	*frame->addr = (MonoLMF *) (((gsize) frame->lmf.previous_lmf) & ~7);
	return true;
}

} // namespace

void
lazy_frame_enter (void *frame, uint64_t caller_fp, uint64_t caller_sp)
{
	link_frame (static_cast<TransitionFrame *> (frame), caller_fp, caller_sp);
}

void *
lazy_frame_leave (void *frame)
{
	if (!unlink_frame (static_cast<TransitionFrame *> (frame)))
		return nullptr;

	/*
	 * An abort aimed at a thread inside the compiler is left as a flag rather
	 * than delivered by hijack, because the thread is not running managed code
	 * at the point it was suspended. This is where it gets picked up - the
	 * forced variant, since the method being compiled can have a protected
	 * wrapper for a caller. The ordinary checkpoint declines while one is there.
	 */
	return mono_thread_force_interruption_checkpoint_noraise ();
}

void
interp_frame_enter (void *frame, const InterpArgContext *args)
{
	InterpTransitionFrame *entry = static_cast<InterpTransitionFrame *> (frame);
	uint64_t caller_sp = (uint64_t) args->stack;

	entry->addr = mono_tls_get_lmf_addr ();

	if (!entry->addr)
		return;

	/*
	 * The caller as it stood at the call: its return address sits directly
	 * below the arguments it pushed, and the callee-saved registers are the
	 * ones the thunk spilled, untouched since.
	 */
	memset (&entry->ctx, 0, sizeof (entry->ctx));
	entry->ctx.gregs[AMD64_RIP] = *(uint64_t *) (caller_sp - sizeof (uint64_t));
	entry->ctx.gregs[AMD64_RSP] = caller_sp;
	entry->ctx.gregs[AMD64_RBP] = args->caller_fp;
	entry->ctx.gregs[AMD64_RBX] = args->saved[0];
	entry->ctx.gregs[AMD64_R12] = args->saved[1];
	entry->ctx.gregs[AMD64_R13] = args->saved[2];
	entry->ctx.gregs[AMD64_R14] = args->saved[3];
	entry->ctx.gregs[AMD64_R15] = args->saved[4];

	entry->lmf.ctx = &entry->ctx;
	entry->lmf.lmf_addr = entry->addr;
	entry->lmf.lmf.rsp = caller_sp;
	/* Bit 2 is what tells the unwinder to read the context rather than rbp. */
	entry->lmf.lmf.previous_lmf = (gpointer) ((gsize) *entry->addr | 4);
	*entry->addr = &entry->lmf.lmf;
}

void
interp_frame_leave (void *frame)
{
	/*
	 * No checkpoint on the way out, unlike the lazy entry: the interpreter
	 * polls for interruption itself while it runs the method, so a thread
	 * getting this far has already been given every chance to take one.
	 */
	InterpTransitionFrame *entry = static_cast<InterpTransitionFrame *> (frame);

	if (entry->addr)
		*entry->addr = (MonoLMF *) (((gsize) entry->lmf.lmf.previous_lmf) & ~7);
}

void **
rethrow_trampoline_slot ()
{
	return (void **) mono_get_rethrow_preserve_exception_addr ();
}

void
emit_callee_saved_clobber (llvm::IRBuilderBase &b)
{
	b.CreateCall (llvm::InlineAsm::get (
		llvm::FunctionType::get (b.getVoidTy (), false), "",
		"~{rbx},~{r12},~{r13},~{r14},~{r15}", /*hasSideEffects=*/true));
}

/*
 * The %fs-relative displacement mono_tls_lmf_addr sits at, or nothing when this
 * build cannot name one.
 *
 * mono_tls_offsets holds what the linker resolved for the thread-local, which is
 * only a displacement from the thread pointer under the initial-exec and
 * local-exec models. Under the dynamic ones the block moves per thread and the
 * number means nothing. Rather than reason about which model a given build got,
 * check the answer: walk the displacement from this thread's own thread pointer
 * and see whether it arrives at the variable.
 */
static std::optional<int32_t>
lmf_address_tls_displacement ()
{
#ifdef MONO_KEYWORD_THREAD
	gint32 offset = mono_tls_offsets[TLS_KEY_LMF_ADDR];
	uint8_t *thread_pointer;

	asm ("movq %%fs:0, %0" : "=r" (thread_pointer));

	if (thread_pointer + offset != (uint8_t *) &mono_tls_lmf_addr)
		return std::nullopt;
	return offset;
#else
	return std::nullopt;
#endif
}

/*
 * Address space 257 is what LLVM calls %fs-relative on x86-64, so the load below
 * is one `mov %fs:disp, reg` - no call, and so nowhere for a thread to be caught
 * with neither a jit-info record nor an LMF to walk from. That window is the
 * whole point: a wrapper's prologue reaches this before it has linked anything
 * onto the chain, and an async stack walk that starts inside it sees no managed
 * frame at all.
 */
llvm::Value *
emit_lmf_address (llvm::IRBuilderBase &b)
{
	std::optional<int32_t> displacement = lmf_address_tls_displacement ();

	if (!displacement)
		return nullptr;

	llvm::Value *slot = b.CreateIntToPtr (
		b.getInt64 ((uint64_t) (int64_t) *displacement),
		llvm::PointerType::get (b.getContext (), 257));

	return b.CreateAlignedLoad (llvm::PointerType::get (b.getContext (), 0),
	                            slot, llvm::Align (TARGET_SIZEOF_VOID_P),
	                            "lmf_addr");
}

/*
 * rsp has to be the frame's settled value - the one live at the transition
 * call - which stacksave reads after the prologue has reserved everything,
 * since codegen never moves rsp again inside a frame without dynamic allocas.
 * frameaddress pins rbp the same way.
 */
void
emit_lmf_capture_registers (llvm::IRBuilderBase &b, llvm::Value *slot)
{
	llvm::Type *ptr = llvm::PointerType::get (b.getContext (), 0);
	llvm::Type *i8 = b.getInt8Ty ();
	llvm::Align align (TARGET_SIZEOF_VOID_P);

	b.CreateAlignedStore (
		b.CreatePtrToInt (b.CreateIntrinsic (llvm::Intrinsic::frameaddress,
	                                             { ptr }, { b.getInt32 (0) }),
	                          b.getInt64Ty ()),
		b.CreateConstInBoundsGEP1_32 (i8, slot,
	                                      MONO_STRUCT_OFFSET (MonoLMF, rbp)),
		align);
	b.CreateAlignedStore (
		b.CreatePtrToInt (b.CreateStackSave (), b.getInt64Ty ()),
		b.CreateConstInBoundsGEP1_32 (i8, slot,
	                                      MONO_STRUCT_OFFSET (MonoLMF, rsp)),
		align);
}

} // namespace mono::arch

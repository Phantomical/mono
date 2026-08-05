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

namespace mono::arch {

/*
 * What the lazy-entry resolver reserves stack for. A plain LMF - not one of the
 * MonoLMFExt kinds - because an ordinary managed-to-native transition is what
 * this is: rsp and rbp below are the caller's, and its ip is read back off its
 * own stack from there.
 *
 * Crossing one of these zeroes the rest of the callee-saved registers, which a
 * managed-to-native wrapper makes up for by restoring them from its own frame
 * and there is no wrapper here. That costs only an exception raised inside the
 * compiler, and an abort does not come out that way: the resolver throws it
 * from the caller's frame, having already unlinked this.
 */
struct LazyFrame {
	MonoLMF lmf;
	MonoLMF **addr;
};

static_assert (sizeof (LazyFrame) <= lazy_frame_size,
               "the resolver does not reserve enough for a lazy-entry frame");

void
lazy_frame_enter (void *frame, uint64_t caller_fp, uint64_t caller_sp)
{
	LazyFrame *lazy = static_cast<LazyFrame *> (frame);

	/*
	 * No LMF chain to link onto means a thread that has not run managed code,
	 * which reaches a stub only by the runtime calling one directly. There is
	 * no managed caller for the walk to find, and nothing to deliver an abort
	 * to either.
	 */
	lazy->addr = mono_tls_get_lmf_addr ();

	if (!lazy->addr)
		return;

	lazy->lmf.rbp = caller_fp;
	lazy->lmf.rsp = caller_sp;
	lazy->lmf.previous_lmf = *lazy->addr;
	*lazy->addr = &lazy->lmf;
}

void *
lazy_frame_leave (void *frame)
{
	LazyFrame *lazy = static_cast<LazyFrame *> (frame);

	if (!lazy->addr)
		return nullptr;

	*lazy->addr = (MonoLMF *) (((gsize) lazy->lmf.previous_lmf) & ~7);

	/*
	 * An abort aimed at a thread inside the compiler is left as a flag rather
	 * than delivered by hijack, because the thread is not running managed code
	 * at the point it was suspended. This is where it gets picked up - the
	 * forced variant, since the method being compiled may well have been
	 * called from a protected wrapper.
	 */
	return mono_thread_force_interruption_checkpoint_noraise ();
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

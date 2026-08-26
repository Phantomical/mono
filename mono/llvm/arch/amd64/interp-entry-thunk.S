/*
 * Enters the interpreter for a method that has no compiled body.
 *
 * A caller makes this call the same way it calls a compiled body: this
 * backend's own convention, with the arguments wherever LLVM put them. %r11
 * carries the MonoDomainMethod * the thunk stands for, loaded by the method's
 * own stub. %r10 is left as the caller set it, because a dispatched call site
 * puts the key naming the method it asked for there.
 *
 * This thunk does not know the prototype, so it spills rather than reads. The
 * helper takes the arguments out of the context once it has looked the method
 * up.
 *
 * The %rbp frame is for the stack walks that cross this: an exception leaving
 * the interpreted method, a thread suspended inside it. interp-entry.cpp
 * registers the unwind program that describes it.
 */

#include "interp-entry-offsets.h"

	.intel_syntax noprefix
	.text
	.globl	mono_llvm_interp_entry_thunk
	.hidden	mono_llvm_interp_entry_thunk
	.type	mono_llvm_interp_entry_thunk, @function
	.p2align 4
mono_llvm_interp_entry_thunk:
	.cfi_startproc
	push	rbp
	.cfi_def_cfa_offset 16
	.cfi_offset rbp, -16
mono_llvm_interp_entry_thunk_pushed:
	mov	rbp, rsp
	.cfi_def_cfa_register rbp
mono_llvm_interp_entry_thunk_framed:
	/*
	 * A multiple of 16, so the call below is made on an aligned stack. Entry
	 * is 8 mod 16, and the push above takes it to 0.
	 */
	sub	rsp, MONO_INTERP_CTX_SIZE

	mov	[rsp + MONO_INTERP_CTX_GREGS + 0x00], rdi
	mov	[rsp + MONO_INTERP_CTX_GREGS + 0x08], rsi
	mov	[rsp + MONO_INTERP_CTX_GREGS + 0x10], rdx
	mov	[rsp + MONO_INTERP_CTX_GREGS + 0x18], rcx
	mov	[rsp + MONO_INTERP_CTX_GREGS + 0x20], r8
	mov	[rsp + MONO_INTERP_CTX_GREGS + 0x28], r9

	/* Whole registers: a Vector128 argument rides all sixteen bytes of one. */
	movaps	[rsp + MONO_INTERP_CTX_FREGS + 0x00], xmm0
	movaps	[rsp + MONO_INTERP_CTX_FREGS + 0x10], xmm1
	movaps	[rsp + MONO_INTERP_CTX_FREGS + 0x20], xmm2
	movaps	[rsp + MONO_INTERP_CTX_FREGS + 0x30], xmm3
	movaps	[rsp + MONO_INTERP_CTX_FREGS + 0x40], xmm4
	movaps	[rsp + MONO_INTERP_CTX_FREGS + 0x50], xmm5
	movaps	[rsp + MONO_INTERP_CTX_FREGS + 0x60], xmm6
	movaps	[rsp + MONO_INTERP_CTX_FREGS + 0x70], xmm7

	/*
	 * %rbp+16 is the caller's %rsp from before its call, which is both where
	 * the stack arguments start and what the LMF stands on. (%rbp) is the
	 * caller's frame pointer, untouched since the call.
	 */
	lea	rax, [rbp + 16]
	mov	[rsp + MONO_INTERP_CTX_STACK], rax
	mov	rax, [rbp]
	mov	[rsp + MONO_INTERP_CTX_CALLER_FP], rax

	/*
	 * Still the caller's, since nothing above has written them. An exception
	 * leaving the interpreted method unwinds straight past this frame into the
	 * caller, so this is the only surviving copy of them. interp_frame_enter ()
	 * reads them to build the context the LMF carries.
	 */
	mov	[rsp + MONO_INTERP_CTX_SAVED + 0x00], rbx
	mov	[rsp + MONO_INTERP_CTX_SAVED + 0x08], r12
	mov	[rsp + MONO_INTERP_CTX_SAVED + 0x10], r13
	mov	[rsp + MONO_INTERP_CTX_SAVED + 0x18], r14
	mov	[rsp + MONO_INTERP_CTX_SAVED + 0x20], r15

	mov	rdi, r11
	mov	rsi, rsp
	call	mono_llvm_interp_entry_from_context

	/* Every register a return of this convention can use, and no more. */
	mov	rax, [rsp + MONO_INTERP_CTX_RET_GREGS + 0x00]
	mov	rdx, [rsp + MONO_INTERP_CTX_RET_GREGS + 0x08]
	mov	rcx, [rsp + MONO_INTERP_CTX_RET_GREGS + 0x10]
	movaps	xmm0, [rsp + MONO_INTERP_CTX_RET_FREGS + 0x00]
	movaps	xmm1, [rsp + MONO_INTERP_CTX_RET_FREGS + 0x10]
	movaps	xmm2, [rsp + MONO_INTERP_CTX_RET_FREGS + 0x20]
	movaps	xmm3, [rsp + MONO_INTERP_CTX_RET_FREGS + 0x30]

	mov	rsp, rbp
	pop	rbp
	.cfi_def_cfa rsp, 8
mono_llvm_interp_entry_thunk_popped:
	ret
	.cfi_endproc
	.size	mono_llvm_interp_entry_thunk, .-mono_llvm_interp_entry_thunk
mono_llvm_interp_entry_thunk_end:

	/*
	 * mono reads its own unwind programs rather than the .cfi_* above.
	 * interp-entry.cpp builds one from these labels: where the frame goes up,
	 * where it is settled, and where it has come back down again.
	 */
	.globl	mono_llvm_interp_entry_thunk_pushed
	.globl	mono_llvm_interp_entry_thunk_framed
	.globl	mono_llvm_interp_entry_thunk_popped
	.globl	mono_llvm_interp_entry_thunk_end
	.hidden	mono_llvm_interp_entry_thunk_pushed
	.hidden	mono_llvm_interp_entry_thunk_framed
	.hidden	mono_llvm_interp_entry_thunk_popped
	.hidden	mono_llvm_interp_entry_thunk_end

	.section .note.GNU-stack,"",@progbits

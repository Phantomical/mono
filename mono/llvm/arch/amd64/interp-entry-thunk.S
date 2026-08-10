/*
 * Enters the interpreter for a method that has no compiled body.
 *
 * A call arrives here exactly as it would have arrived at the method - the
 * backend's own convention, arguments wherever LLVM put them - with %r10
 * carrying the MonoMethod *. That is the register LLVM pins a `nest` parameter
 * to, and the same channel every dispatched call already uses to say which
 * method was asked for.
 *
 * Nothing here knows the prototype, so it spills rather than reads, and the
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
	 * A multiple of 16, so the call below is made on an aligned stack: entry
	 * is 8 mod 16 and the push above takes it to 0.
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
	 * the stack arguments start and what the LMF stands on; (%rbp) is the
	 * caller's frame pointer, untouched since the call.
	 */
	lea	rax, [rbp + 16]
	mov	[rsp + MONO_INTERP_CTX_STACK], rax
	mov	rax, [rbp]
	mov	[rsp + MONO_INTERP_CTX_CALLER_FP], rax

	mov	rdi, r10
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
	 * mono reads its own unwind programs rather than the .cfi_* above, and
	 * interp-entry.cpp builds one from these: where the frame goes up, where it
	 * is settled, and where it has come back down again.
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

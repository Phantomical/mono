/*
 * Makes a call whose prototype is known only at run time.
 *
 * The interpreter reaches a compiled body through this. dyn-call.cpp has
 * already put each argument in the DynCallFrame slot the convention gives it.
 * This thunk moves those slots into the registers and onto the stack, makes
 * the call, and puts the return value back. %rdi carries the frame and %rsi
 * the address to call.
 *
 * The frame's stack area is an image of the callee's incoming stack arguments.
 * So the copy is a straight run of words rather than a walk of the signature.
 *
 * An exception that leaves the callee unwinds past this frame without reading
 * it. do_jit_call () pushes an LMF before the call, and a walk that finds no
 * jit info for an address goes to that LMF. The .cfi_* below are for gdb and
 * perf, which have no such side channel.
 */

#include "dyn-call-offsets.h"

	.intel_syntax noprefix
	.text
	.globl	mono_llvm_dyn_call_thunk
	.hidden	mono_llvm_dyn_call_thunk
	.type	mono_llvm_dyn_call_thunk, @function
	.p2align 4
mono_llvm_dyn_call_thunk:
	.cfi_startproc
	push	rbp
	.cfi_def_cfa_offset 16
	.cfi_offset rbp, -16
	mov	rbp, rsp
	.cfi_def_cfa_register rbp
	push	rbx
	.cfi_offset rbx, -24

	/* %rbx and %r11 hold what the register loads below would otherwise
	 * overwrite. Only %rbx has to survive the call. */
	mov	rbx, rdi
	mov	r11, rsi

	/*
	 * The ABI wants %rsp a multiple of 16 at the call. Aligning here rather
	 * than counting the pushes lets the stack area be any number of words.
	 * %rbp keeps the way back.
	 */
	and	rsp, -16

	mov	rax, [rbx + MONO_DYN_CALL_NSTACK]
	test	rax, rax
	jz	2f

	/* Round the word count up to a pair, so the reservation keeps the
	 * alignment above. */
	lea	rcx, [rax + 1]
	and	rcx, -2
	shl	rcx, 3
	sub	rsp, rcx

	xor	rcx, rcx
1:
	mov	rdx, [rbx + MONO_DYN_CALL_STACK + rcx * 8]
	mov	[rsp + rcx * 8], rdx
	inc	rcx
	cmp	rcx, rax
	jb	1b
2:

	/*
	 * %al is the count of vector registers a variadic callee would read.
	 * No managed call is variadic, so a callee ignores it, and a C one that
	 * is reached this way needs it right.
	 */
	cmp	qword ptr [rbx + MONO_DYN_CALL_HAS_FP], 0
	je	3f

	movsd	xmm0, qword ptr [rbx + MONO_DYN_CALL_FREGS + 0x00]
	movsd	xmm1, qword ptr [rbx + MONO_DYN_CALL_FREGS + 0x08]
	movsd	xmm2, qword ptr [rbx + MONO_DYN_CALL_FREGS + 0x10]
	movsd	xmm3, qword ptr [rbx + MONO_DYN_CALL_FREGS + 0x18]
	movsd	xmm4, qword ptr [rbx + MONO_DYN_CALL_FREGS + 0x20]
	movsd	xmm5, qword ptr [rbx + MONO_DYN_CALL_FREGS + 0x28]
	movsd	xmm6, qword ptr [rbx + MONO_DYN_CALL_FREGS + 0x30]
	movsd	xmm7, qword ptr [rbx + MONO_DYN_CALL_FREGS + 0x38]
	mov	eax, 8
	jmp	4f
3:
	xor	eax, eax
4:

	/* Last, because the moves above need registers of their own. */
	mov	rdi, [rbx + MONO_DYN_CALL_GREGS + 0x00]
	mov	rsi, [rbx + MONO_DYN_CALL_GREGS + 0x08]
	mov	rdx, [rbx + MONO_DYN_CALL_GREGS + 0x10]
	mov	rcx, [rbx + MONO_DYN_CALL_GREGS + 0x18]
	mov	r8,  [rbx + MONO_DYN_CALL_GREGS + 0x20]
	mov	r9,  [rbx + MONO_DYN_CALL_GREGS + 0x28]

	call	r11

	/* A scalar return uses one of these two and dyn-call.cpp reads the one
	 * the signature names. */
	mov	[rbx + MONO_DYN_CALL_RET_GREG], rax
	movsd	qword ptr [rbx + MONO_DYN_CALL_RET_FREG], xmm0

	lea	rsp, [rbp - 8]
	pop	rbx
	.cfi_restore rbx
	pop	rbp
	.cfi_def_cfa rsp, 8
	ret
	.cfi_endproc
	.size	mono_llvm_dyn_call_thunk, .-mono_llvm_dyn_call_thunk

	.section .note.GNU-stack,"",@progbits

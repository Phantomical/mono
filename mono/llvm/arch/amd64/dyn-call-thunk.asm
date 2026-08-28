; Makes a call whose prototype is known only at run time.
;
; The Microsoft x64 spelling of dyn-call-thunk.S.  The interpreter reaches a
; compiled body through this.  dyn-call.cpp has already put each argument in
; the DynCallFrame slot the convention gives it.  This thunk moves those slots
; into the registers and onto the stack, makes the call, and puts the return
; value back.  rcx carries the frame and rdx the address to call.
;
; The frame's stack area is an image of the callee's incoming stack arguments,
; the 32 bytes of shadow space in front of them included.  So the copy is a
; straight run of words rather than a walk of the signature.
;
; An exception that leaves the callee unwinds past this frame without reading
; it.  do_jit_call () pushes an LMF before the call, and a walk that finds no
; jit info for an address goes to that LMF.

include dyn-call-offsets.inc

.code

PUBLIC mono_llvm_dyn_call_thunk

ALIGN 16
mono_llvm_dyn_call_thunk PROC
	push	rbp
	mov	rbp, rsp
	push	rbx

	; rbx and r10 hold what the register loads below would otherwise
	; overwrite.  Only rbx has to survive the call, and r10 is free here
	; because a dyn call never carries the key that register otherwise holds.
	mov	rbx, rcx
	mov	r10, rdx

	; The ABI wants rsp a multiple of 16 at the call.  Aligning here rather
	; than counting the pushes lets the stack area be any number of words.
	; rbp keeps the way back.
	and	rsp, -16

	mov	rax, [rbx + MONO_DYN_CALL_NSTACK]

	; Round the word count up to a pair, so the reservation keeps the
	; alignment above.  A call with no stack arguments still reserves the
	; four shadow words its callee is owed, which is what nstack counts from.
	lea	rcx, [rax + 1]
	and	rcx, -2
	shl	rcx, 3
	sub	rsp, rcx

	test	rax, rax
	jz	copied

	xor	rcx, rcx
copy_word:
	mov	rdx, [rbx + MONO_DYN_CALL_STACK + rcx * 8]
	mov	[rsp + rcx * 8], rdx
	inc	rcx
	cmp	rcx, rax
	jb	copy_word
copied:

	cmp	qword ptr [rbx + MONO_DYN_CALL_HAS_FP], 0
	je	no_fp

	; Whole registers: a Vector128 argument rides all sixteen bytes of one.
	movups	xmm0, xmmword ptr [rbx + MONO_DYN_CALL_FREGS + 00h]
	movups	xmm1, xmmword ptr [rbx + MONO_DYN_CALL_FREGS + 10h]
	movups	xmm2, xmmword ptr [rbx + MONO_DYN_CALL_FREGS + 20h]
	movups	xmm3, xmmword ptr [rbx + MONO_DYN_CALL_FREGS + 30h]
no_fp:

	; Last, because the moves above need registers of their own.
	mov	rcx, [rbx + MONO_DYN_CALL_GREGS + 00h]
	mov	rdx, [rbx + MONO_DYN_CALL_GREGS + 08h]
	mov	r8,  [rbx + MONO_DYN_CALL_GREGS + 10h]
	mov	r9,  [rbx + MONO_DYN_CALL_GREGS + 18h]

	call	r10

	; Every register a return of this convention can use, and no more.  Which
	; of these dyn-call.cpp reads back is the plan's, not this thunk's.
	mov	[rbx + MONO_DYN_CALL_RET_GREGS + 00h], rax
	mov	[rbx + MONO_DYN_CALL_RET_GREGS + 08h], rdx
	mov	[rbx + MONO_DYN_CALL_RET_GREGS + 10h], rcx
	movups	xmmword ptr [rbx + MONO_DYN_CALL_RET_FREGS + 00h], xmm0
	movups	xmmword ptr [rbx + MONO_DYN_CALL_RET_FREGS + 10h], xmm1
	movups	xmmword ptr [rbx + MONO_DYN_CALL_RET_FREGS + 20h], xmm2
	movups	xmmword ptr [rbx + MONO_DYN_CALL_RET_FREGS + 30h], xmm3

	lea	rsp, [rbp - 8]
	pop	rbx
	pop	rbp
	ret
mono_llvm_dyn_call_thunk ENDP

end

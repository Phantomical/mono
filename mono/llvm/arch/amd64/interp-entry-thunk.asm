; Enters the interpreter for a method that has no compiled body.
;
; The Microsoft x64 spelling of interp-entry-thunk.S.  A caller makes this call
; the same way it calls a compiled body: this backend's own convention, with
; the arguments wherever LLVM put them.  r11 carries the MonoDomainMethod * the
; thunk stands for, loaded by the method's own stub.  r10 is left as the caller
; set it, because a dispatched call site puts the key naming the method it
; asked for there.
;
; This thunk does not know the prototype, so it spills rather than reads.  The
; helper takes the arguments out of the context once it has looked the method
; up.
;
; The rbp frame is for the stack walks that cross this: an exception leaving
; the interpreted method, a thread suspended inside it.  interp-entry.cpp
; registers the unwind program that describes it.

include interp-entry-offsets.inc

; The 32 bytes below the context are the shadow space the helper's own callees
; are owed.  Reserving them here rather than around the call keeps rsp settled
; for the whole body.
CTX EQU 20h

.code

PUBLIC mono_llvm_interp_entry_thunk
PUBLIC mono_llvm_interp_entry_thunk_pushed
PUBLIC mono_llvm_interp_entry_thunk_framed
PUBLIC mono_llvm_interp_entry_thunk_popped
PUBLIC mono_llvm_interp_entry_thunk_end

EXTERN mono_llvm_interp_entry_from_context:PROC

ALIGN 16
mono_llvm_interp_entry_thunk PROC
	push	rbp
mono_llvm_interp_entry_thunk_pushed LABEL BYTE
	mov	rbp, rsp
mono_llvm_interp_entry_thunk_framed LABEL BYTE
	; A multiple of 16, so the call below is made on an aligned stack. Entry
	; is 8 mod 16, and the push above takes it to 0.
	sub	rsp, CTX + MONO_INTERP_CTX_SIZE

	mov	[rsp + CTX + MONO_INTERP_CTX_GREGS + 00h], rcx
	mov	[rsp + CTX + MONO_INTERP_CTX_GREGS + 08h], rdx
	mov	[rsp + CTX + MONO_INTERP_CTX_GREGS + 10h], r8
	mov	[rsp + CTX + MONO_INTERP_CTX_GREGS + 18h], r9

	; Whole registers: a Vector128 argument rides all sixteen bytes of one.
	movaps	[rsp + CTX + MONO_INTERP_CTX_FREGS + 00h], xmm0
	movaps	[rsp + CTX + MONO_INTERP_CTX_FREGS + 10h], xmm1
	movaps	[rsp + CTX + MONO_INTERP_CTX_FREGS + 20h], xmm2
	movaps	[rsp + CTX + MONO_INTERP_CTX_FREGS + 30h], xmm3

	; rbp+16 is the caller's rsp from before its call, which is both where
	; the stack arguments start -- the first 32 bytes of them being the
	; shadow space -- and what the LMF stands on.  [rbp] is the caller's
	; frame pointer, untouched since the call.
	lea	rax, [rbp + 16]
	mov	[rsp + CTX + MONO_INTERP_CTX_STACK], rax
	mov	rax, [rbp]
	mov	[rsp + CTX + MONO_INTERP_CTX_CALLER_FP], rax

	; Still the caller's, since nothing above has written them.  An exception
	; leaving the interpreted method unwinds straight past this frame into
	; the caller, so this is the only surviving copy of them.
	; interp_frame_enter () reads them to build the context the LMF carries.
	mov	[rsp + CTX + MONO_INTERP_CTX_SAVED + 00h], rbx
	mov	[rsp + CTX + MONO_INTERP_CTX_SAVED + 08h], rdi
	mov	[rsp + CTX + MONO_INTERP_CTX_SAVED + 10h], rsi
	mov	[rsp + CTX + MONO_INTERP_CTX_SAVED + 18h], r12
	mov	[rsp + CTX + MONO_INTERP_CTX_SAVED + 20h], r13
	mov	[rsp + CTX + MONO_INTERP_CTX_SAVED + 28h], r14
	mov	[rsp + CTX + MONO_INTERP_CTX_SAVED + 30h], r15

	mov	rcx, r11
	lea	rdx, [rsp + CTX]
	call	mono_llvm_interp_entry_from_context

	; Every register a return of this convention can use, and no more.
	mov	rax, [rsp + CTX + MONO_INTERP_CTX_RET_GREGS + 00h]
	mov	rdx, [rsp + CTX + MONO_INTERP_CTX_RET_GREGS + 08h]
	mov	rcx, [rsp + CTX + MONO_INTERP_CTX_RET_GREGS + 10h]
	movaps	xmm0, [rsp + CTX + MONO_INTERP_CTX_RET_FREGS + 00h]
	movaps	xmm1, [rsp + CTX + MONO_INTERP_CTX_RET_FREGS + 10h]
	movaps	xmm2, [rsp + CTX + MONO_INTERP_CTX_RET_FREGS + 20h]
	movaps	xmm3, [rsp + CTX + MONO_INTERP_CTX_RET_FREGS + 30h]

	mov	rsp, rbp
	pop	rbp
mono_llvm_interp_entry_thunk_popped LABEL BYTE
	ret
mono_llvm_interp_entry_thunk ENDP

mono_llvm_interp_entry_thunk_end LABEL BYTE

end

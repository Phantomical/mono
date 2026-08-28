/**
 * \file
 * \brief The InterpArgContext layout, in a form the assembler can read.
 *
 * The thunk and the struct in amd64.hpp are two spellings of the same thing.
 * The offsets are written down once here, and the C++ side static_asserts its
 * own against them.
 *
 * The two hosts pass a call differently, so each gets a layout of its own: the
 * System V convention has six integer and eight SSE argument registers, and
 * the Microsoft one has four of each, sharing one sequence of argument slots
 * between the two files.
 *
 * A GAS source reads this file through the C preprocessor.  MASM has no such
 * step, so the build turns this into a .inc of EQUs and the .asm reads that --
 * see MonoMasmOffsets.cmake.
 */

#ifndef MONO_LLVM_ARCH_AMD64_INTERP_ENTRY_OFFSETS_H
#define MONO_LLVM_ARCH_AMD64_INTERP_ENTRY_OFFSETS_H

#ifdef HOST_WIN32

#define MONO_INTERP_CTX_GREGS     0x00  /* 4 x 8:  rcx rdx r8 r9              */
#define MONO_INTERP_CTX_FREGS     0x20  /* 4 x 16: xmm0 - xmm3                */
#define MONO_INTERP_CTX_RET_GREGS 0x60  /* 3 x 8:  rax rdx rcx                */
#define MONO_INTERP_CTX_RET_FREGS 0x80  /* 4 x 16: xmm0 - xmm3                */
#define MONO_INTERP_CTX_STACK     0xc0
#define MONO_INTERP_CTX_CALLER_FP 0xc8
#define MONO_INTERP_CTX_SAVED     0xd0  /* 7 x 8:  rbx rdi rsi r12 r13 r14 r15 */
#define MONO_INTERP_CTX_SIZE      0x110

#else

#define MONO_INTERP_CTX_GREGS     0x00  /* 6 x 8:  rdi rsi rdx rcx r8 r9      */
#define MONO_INTERP_CTX_FREGS     0x30  /* 8 x 16: xmm0 - xmm7                */
#define MONO_INTERP_CTX_RET_GREGS 0xb0  /* 3 x 8:  rax rdx rcx                */
#define MONO_INTERP_CTX_RET_FREGS 0xd0  /* 4 x 16: xmm0 - xmm3                */
#define MONO_INTERP_CTX_STACK     0x110
#define MONO_INTERP_CTX_CALLER_FP 0x118
#define MONO_INTERP_CTX_SAVED     0x120 /* 5 x 8:  rbx r12 r13 r14 r15        */
#define MONO_INTERP_CTX_SIZE      0x150

#endif

#endif /* MONO_LLVM_ARCH_AMD64_INTERP_ENTRY_OFFSETS_H */

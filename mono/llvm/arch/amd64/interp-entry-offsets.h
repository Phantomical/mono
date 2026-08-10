/**
 * \file
 * \brief The InterpArgContext layout, in a form the assembler can read.
 *
 * interp-entry-thunk.S and the struct in amd64.hpp are two spellings of the
 * same thing, so the offsets are written down once here and the C++ side
 * static_asserts its own against them.
 */

#ifndef MONO_LLVM_ARCH_AMD64_INTERP_ENTRY_OFFSETS_H
#define MONO_LLVM_ARCH_AMD64_INTERP_ENTRY_OFFSETS_H

#define MONO_INTERP_CTX_GREGS     0x00  /* 6 x 8:  rdi rsi rdx rcx r8 r9 */
#define MONO_INTERP_CTX_FREGS     0x30  /* 8 x 16: xmm0 - xmm7           */
#define MONO_INTERP_CTX_RET_GREGS 0xb0  /* 3 x 8:  rax rdx rcx           */
#define MONO_INTERP_CTX_RET_FREGS 0xd0  /* 4 x 16: xmm0 - xmm3           */
#define MONO_INTERP_CTX_STACK     0x110
#define MONO_INTERP_CTX_CALLER_FP 0x118
#define MONO_INTERP_CTX_SAVED     0x120  /* 5 x 8:  rbx r12 r13 r14 r15    */
#define MONO_INTERP_CTX_SIZE      0x150

#endif /* MONO_LLVM_ARCH_AMD64_INTERP_ENTRY_OFFSETS_H */

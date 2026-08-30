/**
 * \file
 * \brief The DynCallFrame layout, in a form the assembler can read.
 *
 * dyn-call-thunk.S and the struct in amd64.hpp are two spellings of the same
 * thing. The offsets are written down once here, and the C++ side
 * static_asserts its own against them.
 */

#ifndef MONO_LLVM_ARCH_AMD64_DYN_CALL_OFFSETS_H
#define MONO_LLVM_ARCH_AMD64_DYN_CALL_OFFSETS_H

#define MONO_DYN_CALL_GREGS     0x00 /* 6 x 8:  rdi rsi rdx rcx r8 r9 */
#define MONO_DYN_CALL_FREGS     0x30 /* 8 x 16: xmm0 - xmm7           */
#define MONO_DYN_CALL_HAS_FP    0xb0
#define MONO_DYN_CALL_NSTACK    0xb8
#define MONO_DYN_CALL_RET_GREGS 0xc0 /* 3 x 8:  rax rdx rcx           */
#define MONO_DYN_CALL_RET_FREGS 0xe0 /* 4 x 16: xmm0 - xmm3           */
#define MONO_DYN_CALL_STACK     0x120
#define MONO_DYN_CALL_SIZE      0x120

#endif /* MONO_LLVM_ARCH_AMD64_DYN_CALL_OFFSETS_H */

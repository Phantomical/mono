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

#define MONO_DYN_CALL_GREGS    0x00 /* 6 x 8: rdi rsi rdx rcx r8 r9 */
#define MONO_DYN_CALL_FREGS    0x30 /* 8 x 8: xmm0 - xmm7           */
#define MONO_DYN_CALL_HAS_FP   0x70
#define MONO_DYN_CALL_NSTACK   0x78
#define MONO_DYN_CALL_RET_GREG 0x80
#define MONO_DYN_CALL_RET_FREG 0x88
#define MONO_DYN_CALL_STACK    0x90
#define MONO_DYN_CALL_SIZE     0x90

#endif /* MONO_LLVM_ARCH_AMD64_DYN_CALL_OFFSETS_H */

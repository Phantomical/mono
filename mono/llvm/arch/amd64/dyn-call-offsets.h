/**
 * \file
 * \brief The DynCallFrame layout, in a form the assembler can read.
 *
 * The thunk and the struct in amd64.hpp are two spellings of the same thing.
 * The offsets are written down once here, and the C++ side static_asserts its
 * own against them.
 *
 * Each host gets a layout of its own, for the reason interp-entry-offsets.h
 * gives, and that file also says how MASM comes to read this one.
 */

#ifndef MONO_LLVM_ARCH_AMD64_DYN_CALL_OFFSETS_H
#define MONO_LLVM_ARCH_AMD64_DYN_CALL_OFFSETS_H

#ifdef HOST_WIN32

#define MONO_DYN_CALL_GREGS     0x00 /* 4 x 8:  rcx rdx r8 r9 */
#define MONO_DYN_CALL_FREGS     0x20 /* 4 x 16: xmm0 - xmm3   */
#define MONO_DYN_CALL_HAS_FP    0x60
#define MONO_DYN_CALL_NSTACK    0x68
#define MONO_DYN_CALL_RET_GREGS 0x70 /* 3 x 8:  rax rdx rcx   */
#define MONO_DYN_CALL_RET_FREGS 0x90 /* 4 x 16: xmm0 - xmm3   */
#define MONO_DYN_CALL_STACK     0xd0
#define MONO_DYN_CALL_SIZE      0xd0

#else

#define MONO_DYN_CALL_GREGS     0x00 /* 6 x 8:  rdi rsi rdx rcx r8 r9 */
#define MONO_DYN_CALL_FREGS     0x30 /* 8 x 16: xmm0 - xmm7           */
#define MONO_DYN_CALL_HAS_FP    0xb0
#define MONO_DYN_CALL_NSTACK    0xb8
#define MONO_DYN_CALL_RET_GREGS 0xc0 /* 3 x 8:  rax rdx rcx           */
#define MONO_DYN_CALL_RET_FREGS 0xe0 /* 4 x 16: xmm0 - xmm3           */
#define MONO_DYN_CALL_STACK     0x120
#define MONO_DYN_CALL_SIZE      0x120

#endif

#endif /* MONO_LLVM_ARCH_AMD64_DYN_CALL_OFFSETS_H */

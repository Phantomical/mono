/**
 * \file
 * \brief amd64 SysV: the types and constants the arch seam is spelled in.
 *
 * Pulled in by arch/arch.hpp, which declares the functions these go with and
 * is what the rest of the backend includes.
 */

#ifndef MONO_LLVM_ARCH_AMD64_AMD64_HPP
#define MONO_LLVM_ARCH_AMD64_AMD64_HPP

#include <llvm/ExecutionEngine/Orc/OrcABISupport.h>
#include <llvm/TargetParser/Triple.h>

#include <cstdint>

namespace mono::arch {

/// The target the backend generates code for.
constexpr llvm::Triple::ArchType target_arch = llvm::Triple::x86_64;

/*
 * A stub is the 6 bytes of `jmpq *slot(%rip)` padded with int3 out to 16.
 *
 * Stock JITLink stubs are those 6 bytes at alignment 1, so they pack tightly
 * and a detour would run off the end of one and into its neighbour. The widest
 * patch Harmony and MonoMod write is 14 bytes - `jmp *0(%rip)` plus the 8-byte
 * destination behind it - so a stub has to own at least that many for a detour
 * to be containable. 16 is that, rounded up to the alignment, and the two
 * bytes left over trap anything that jumps into the tail.
 */
constexpr uint64_t stub_block_size = 16;
constexpr uint64_t stub_alignment = 16;

/*
 * A call-counting thunk is 32 bytes of head - the counter and the two addresses
 * its jumps read - followed by 48 bytes of code. Both jumps are indirect
 * through the head so that neither depends on how far away its destination
 * landed, which is what lets the block be carved anywhere in the slabs.
 */
constexpr uint64_t counter_thunk_size = 80;
constexpr uint64_t counter_thunk_alignment = 16;

/// How much of a thunk's block is code, which is all of it after the head.
constexpr uint64_t counter_thunk_code_size = 48;

/*
 * Stack the lazy-entry resolver reserves for its frame. lmf.cpp casts it to
 * its own struct and static_asserts it fits; 32 keeps the frame that follows
 * 16-aligned.
 */
constexpr unsigned lazy_frame_size = 32;

/// ORC's re-entry ABI, resolving through a mono lazy-entry frame.
struct LazyEntryABI : public llvm::orc::OrcX86_64_SysV {
	static constexpr unsigned ResolverCodeSize = 0xc2;

	static void writeResolverCode (char *resolver_mem,
	                               llvm::orc::ExecutorAddr resolver_addr,
	                               llvm::orc::ExecutorAddr reentry_fn,
	                               llvm::orc::ExecutorAddr reentry_ctx);
};

} // namespace mono::arch

#endif

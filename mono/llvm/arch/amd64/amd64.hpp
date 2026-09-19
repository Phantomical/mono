/**
 * \file
 * \brief amd64: the types and constants the arch seam is spelled in.
 *
 * Pulled in by arch/arch.hpp, which declares the functions these go with and
 * is what the rest of the backend includes.
 *
 * The architecture is one and the conventions are two.  Where a type here
 * describes registers a call arrived in or is made through, it has an arm per
 * convention: System V hands out six integer and eight SSE argument registers
 * from two independent files, and the Microsoft one hands out four argument
 * slots, each of which is an integer register or an SSE register depending on
 * what the argument is.  Everything else on this page is shared.
 */

#ifndef MONO_LLVM_ARCH_AMD64_AMD64_HPP
#define MONO_LLVM_ARCH_AMD64_AMD64_HPP

// Every arm below turns on HOST_WIN32, and so do the two layout headers. A
// translation unit that reaches this page before config.h otherwise gets the
// System V arm of each while its neighbours get the Microsoft one.
#include "config.h"

#include "sidetables.hpp"

#include <llvm/ExecutionEngine/Orc/OrcABISupport.h>
#include <llvm/TargetParser/Triple.h>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace mono::arch {

constexpr llvm::Triple::ArchType target_arch = llvm::Triple::x86_64;

/*
 * What a DWARF CIE for this target says: which column holds the return address,
 * and the factors a CFI program's operands are scaled by.
 *
 * The alignment factors are what make the common operands fit in one LEB byte.
 * Every instruction boundary is a legal code offset here, and a saved register
 * always lands on an 8-byte boundary below the CFA.
 */
constexpr unsigned dwarf_return_address_reg = 16; /* RIP */
constexpr unsigned dwarf_stack_pointer_reg = 7;   /* RSP */
constexpr unsigned dwarf_frame_pointer_reg = 6;   /* RBP */
constexpr int dwarf_code_alignment_factor = 1;
constexpr int dwarf_data_alignment_factor = -8;

/*
 * Stack to reserve for the LMF standing for a managed-to-native transition -
 * what lazy_frame_enter () links. lmf.cpp casts it to its own struct and
 * static_asserts it fits; 32 keeps the frame that follows 16-aligned.
 */
constexpr unsigned managed_frame_size = 32;

/// How many registers of each file a return value's leaves can be spread over.
///
/// The two SSE counts index one register file and run out at different points,
/// so one running count serves both. RetCC_X86_64_C gives f32 and f64 only XMM0
/// and XMM1, and the four-register rule beside it in RetCC_X86Common is for
/// vectors.
///
/// A scalar leaf past the second is not demoted to memory. It comes back on
/// the x87 stack, which hidden-return.hpp's plan does not reach either.
constexpr unsigned ret_gregs = 3, ret_scalar_fregs = 2, ret_vector_fregs = 4;

/// How much code a context stub takes, and what it wants to be aligned to.
///
/// It is a `movabs` into the key register and a `jmp rel32`: 10 bytes and 5.
constexpr size_t context_stub_size = 15;
constexpr size_t context_stub_align = 16;

/// What the re-entry resolver spilled, which is the whole state of the call it
/// interrupted. The resolver hands a pointer to this to its callback.
///
/// A managed `call` pushed the return address, the stub jumped to a trampoline,
/// and the trampoline called the resolver. So the four words above the
/// registers name that call: which trampoline it came through, and where in the
/// caller it came from.
struct LazyEntryFrame {
	void *r15;
	void *r14;
	void *r13;
	void *r12;
	/// What a method's thunk writes to say which method the call asked for.
	void *r11;
	/// MONO_ARCH_IMT_REG and MONO_ARCH_RGCTX_REG together: the key an IMT
	/// thunk or a generic-virtual trampoline was entered with, or the context
	/// a shared body reads.
	void *r10;
	void *r9;
	void *r8;
	void *rdi;
	void *rsi;
	void *rdx;
	void *rcx;
	void *rbx;
	void *rax;

	/// The caller's frame pointer, untouched since the call.
	void *caller_fp;
	/// Where the trampoline's own call returns to.
	void *trampoline_ret;
	/// Where the managed call came from.
	void *caller_ip;
	/// The caller's stack pointer.
	void *caller_sp;
};

/// The trampoline the call in \p frame arrived through.
///
/// ORC's trampoline is one call instruction, so its return address is six
/// bytes past where it starts. That address is what LazyCallbacks keys on.
inline void *
trampoline_of (const LazyEntryFrame *frame)
{
	return (char *) frame->trampoline_ret - 6;
}

/// ORC's re-entry ABI, resolving through a mono lazy-entry frame.
///
/// Only the trampoline and stub writers come from the base, and those are the
/// same either way; the resolver below is written here for both conventions.
/// The base still names which one, so that the two never disagree silently.
#ifdef HOST_WIN32
struct LazyEntryABI : public llvm::orc::OrcX86_64_Win32 {
	static constexpr unsigned ResolverCodeSize = 0xc2;
#else
struct LazyEntryABI : public llvm::orc::OrcX86_64_SysV {
	static constexpr unsigned ResolverCodeSize = 0xbe;
#endif

	static void writeResolverCode (char *resolver_mem,
	                               llvm::orc::ExecutorAddr resolver_addr,
	                               llvm::orc::ExecutorAddr reentry_fn,
	                               llvm::orc::ExecutorAddr reentry_ctx);

	/// Writes the resolver's instructions into the first ResolverCodeSize bytes
	/// of \p resolver_mem and does nothing else. writeResolverCode () publishes
	/// the block it wrote.
	static void write_resolver_body (char *resolver_mem,
	                                 llvm::orc::ExecutorAddr reentry_fn,
	                                 llvm::orc::ExecutorAddr reentry_ctx);
};

/// Re-entry ABI for methods that pass Vector256<T> values in YMM registers.
/// FXSAVE/FXRSTOR preserve only the low 128 bits, so this resolver also saves
/// and restores the full YMM register file with VMOVUPS.
#ifdef HOST_WIN32
struct LazyEntryAvxABI : public llvm::orc::OrcX86_64_Win32 {
	static constexpr unsigned ResolverCodeSize = 0x1dd;
#else
struct LazyEntryAvxABI : public llvm::orc::OrcX86_64_SysV {
	static constexpr unsigned ResolverCodeSize = 0x1d9;
#endif

	static void writeResolverCode (char *resolver_mem,
	                               llvm::orc::ExecutorAddr resolver_addr,
	                               llvm::orc::ExecutorAddr reentry_fn,
	                               llvm::orc::ExecutorAddr reentry_ctx);

	static void write_resolver_body (char *resolver_mem,
	                                 llvm::orc::ExecutorAddr reentry_fn,
	                                 llvm::orc::ExecutorAddr reentry_ctx);
};

/// Builds the frame LazyEntryABI's re-entry resolver runs on, as a CFI
/// program.
///
/// The frame it declares is the managed caller's rather than the resolver's
/// own. A walk that arrives during a compile therefore goes on to the code
/// that made the call. The rules are tied to the instruction offsets in the
/// resolver, so the two only stay true together.
std::vector<UnwindRecord> lazy_resolver_frame ();

/// Builds the CFI program for the AVX re-entry resolver.
std::vector<UnwindRecord> lazy_resolver_frame_avx ();

} // namespace mono::arch

#endif

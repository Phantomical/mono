/**
 * \file
 * \brief amd64 SysV: the types and constants the arch seam is spelled in.
 *
 * Pulled in by arch/arch.hpp, which declares the functions these go with and
 * is what the rest of the backend includes.
 */

#ifndef MONO_LLVM_ARCH_AMD64_AMD64_HPP
#define MONO_LLVM_ARCH_AMD64_AMD64_HPP

#include "arch/amd64/interp-entry-offsets.h"
#include "sidetables.hpp"

#include <llvm/ExecutionEngine/Orc/OrcABISupport.h>
#include <llvm/TargetParser/Triple.h>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace mono::arch {

/// The target the backend generates code for.
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
 * The prologue in front of a stub that a call off a value type's vtable arrives
 * at. It runs into the stub behind it and needs no jump of its own, so
 * `addq $imm8, %rdi` is the whole of it.
 */
constexpr uint64_t unbox_prologue_size = 4;

/*
 * Stack to reserve for the LMF standing for a managed-to-native transition -
 * what lazy_frame_enter () links. lmf.cpp casts it to its own struct and
 * static_asserts it fits; 32 keeps the frame that follows 16-aligned.
 */
constexpr unsigned managed_frame_size = 32;

/*
 * Stack to reserve for the LMF an interpreter entry links, which is larger
 * because that one carries a whole MonoContext. lmf.cpp static_asserts the fit.
 */
constexpr unsigned interp_frame_size = 512;

/* -- Entering the interpreter --------------------------------------------- */

/// The registers a call arrived in, as interp-entry-thunk.S spilled them.
struct InterpArgContext {
	uint64_t gregs[6];                     ///< rdi rsi rdx rcx r8 r9
	alignas (16) uint8_t fregs[8][16];     ///< xmm0 - xmm7
	uint64_t ret_gregs[3];                 ///< rax rdx rcx
	alignas (16) uint8_t ret_fregs[4][16]; ///< xmm0 - xmm3
	uint8_t *stack;                        ///< the caller's outgoing arguments
	uint64_t caller_fp;                    ///< the caller's frame pointer
	uint64_t saved[5];                     ///< rbx r12 r13 r14 r15, the caller's
};

static_assert (offsetof (InterpArgContext, gregs) == MONO_INTERP_CTX_GREGS);
static_assert (offsetof (InterpArgContext, fregs) == MONO_INTERP_CTX_FREGS);
static_assert (offsetof (InterpArgContext, ret_gregs) == MONO_INTERP_CTX_RET_GREGS);
static_assert (offsetof (InterpArgContext, ret_fregs) == MONO_INTERP_CTX_RET_FREGS);
static_assert (offsetof (InterpArgContext, stack) == MONO_INTERP_CTX_STACK);
static_assert (offsetof (InterpArgContext, caller_fp) == MONO_INTERP_CTX_CALLER_FP);
static_assert (offsetof (InterpArgContext, saved) == MONO_INTERP_CTX_SAVED);
static_assert (sizeof (InterpArgContext) == MONO_INTERP_CTX_SIZE);

/// One move between an InterpArgContext slot and a value's own storage.
struct ArgPiece {
	enum class File : uint8_t {
		Greg,  ///< an integer register, indexed by AT
		Freg,  ///< an SSE register, indexed by AT
		Stack, ///< the caller's arguments, at byte offset AT
	};

	File file = File::Greg;
	uint8_t width = 0;   ///< how many bytes move
	uint32_t at = 0;     ///< the register number, or the byte offset
	uint32_t offset = 0; ///< where they sit within the value
};

/// Where one of the interpreter's arguments arrived.
///
/// The interpreter reads an argument through a pointer, so a value that came in
/// one piece needs no copy: the context slot it landed in is already storage to
/// point at. Only a value LLVM spread across several registers has to be
/// gathered somewhere contiguous first.
struct ArgPlan {
	enum class Where : uint8_t { Greg, Freg, Stack, Pieces };

	Where where = Where::Greg;
	/// The interpreter wants the pointer itself, not the slot holding it.
	bool byref = false;
	/// The register number, the byte offset into the caller's arguments, or the
	/// byte offset into the entry's scratch when Pieces.
	uint32_t at = 0;
	uint32_t size = 0; ///< how many bytes the value is
	/// The half-open range of InterpEntryLayout::pieces to gather from.
	uint32_t first_piece = 0, piece_count = 0;
};

/// How a method's return value gets back to its caller.
struct ReturnPlan {
	enum class Kind : uint8_t {
		None,      ///< the method returns nothing
		Hidden,    ///< written straight into a pointer the caller passed
		Registers, ///< gathered into scratch and scattered into the registers
	};

	Kind kind = Kind::None;
	uint32_t hidden_greg = 0; ///< which register the caller's pointer came in
	uint32_t size = 0;        ///< how many bytes the value is, when Registers
	std::vector<ArgPiece> pieces;
};

/// How a call to one prototype is read out of an InterpArgContext. Shared by
/// every method that has that prototype.
struct InterpEntryLayout {
	bool has_this = false;
	uint32_t this_greg = 0;    ///< where the receiver arrived, when there is one
	std::vector<ArgPlan> args; ///< one per signature parameter, the receiver aside
	std::vector<ArgPiece> pieces;
	ReturnPlan ret;
	uint32_t ret_scratch = 0;  ///< where the return is gathered, when Registers
	uint32_t scratch_size = 0; ///< room a call needs for everything gathered
};

/// What the thunk resolves a MonoMethod * to.
struct InterpEntryPoint {
	const InterpEntryLayout *layout = nullptr;
	void *imethod = nullptr; ///< InterpMethod *, which nothing here looks into
};

/// ORC's re-entry ABI, resolving through a mono lazy-entry frame.
struct LazyEntryABI : public llvm::orc::OrcX86_64_SysV {
	static constexpr unsigned ResolverCodeSize = 0xc2;

	static void writeResolverCode (char *resolver_mem,
	                               llvm::orc::ExecutorAddr resolver_addr,
	                               llvm::orc::ExecutorAddr reentry_fn,
	                               llvm::orc::ExecutorAddr reentry_ctx);
};

/// The frame the re-entry resolver runs on, as a CFI program.
///
/// The frame it declares is the managed caller's rather than the resolver's
/// own, so a walk that arrives during a compile goes on to the code that made
/// the call. The rules are tied to the instruction offsets in the resolver, so
/// the two only stay true together.
std::vector<UnwindRecord> lazy_resolver_frame ();

} // namespace mono::arch

#endif

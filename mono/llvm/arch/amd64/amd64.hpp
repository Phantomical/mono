/**
 * \file
 * \brief amd64 SysV: the types and constants the arch seam is spelled in.
 *
 * Pulled in by arch/arch.hpp, which declares the functions these go with and
 * is what the rest of the backend includes.
 */

#ifndef MONO_LLVM_ARCH_AMD64_AMD64_HPP
#define MONO_LLVM_ARCH_AMD64_AMD64_HPP

#include "arch/amd64/dyn-call-offsets.h"
#include "arch/amd64/interp-entry-offsets.h"
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

/*
 * Stack to reserve for the LMF an interpreter entry links, which is larger
 * because that one carries a whole MonoContext. lmf.cpp static_asserts the fit.
 */
constexpr unsigned interp_frame_size = 512;

/// How many registers of each file a return value's leaves can be spread over.
///
/// The two SSE counts index one register file and run out at different points,
/// so one running count serves both. RetCC_X86_64_C gives f32 and f64 only XMM0
/// and XMM1, and the four-register rule beside it in RetCC_X86Common is for
/// vectors.
///
/// A scalar leaf past the second is not demoted to memory. It comes back on the
/// x87 stack, which neither thunk across the interpreter seam saves.
constexpr unsigned ret_gregs = 3, ret_scalar_fregs = 2, ret_vector_fregs = 4;

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
		Greg,  ///< an integer register, indexed by `at`
		Freg,  ///< an SSE register, indexed by `at`
		Stack, ///< the caller's arguments, at the byte offset `at`
	};

	File file = File::Greg;
	uint8_t width = 0;   ///< how many bytes move
	uint32_t at = 0;
	uint32_t offset = 0; ///< where they sit within the value
};

/// Where one of the interpreter's arguments arrived.
///
/// The interpreter reads an argument through a pointer. A value that arrived
/// in one piece needs no copy: the context slot it landed in is already
/// storage to point at. Only a value LLVM spread across several registers has
/// to be gathered somewhere contiguous first.
struct ArgPlan {
	enum class Where : uint8_t { Greg, Freg, Stack, Pieces };

	Where where = Where::Greg;
	/// The interpreter wants the pointer itself, not the slot holding it.
	bool byref = false;
	/// The register number, the byte offset into the caller's arguments, or the
	/// byte offset into the entry's scratch when Pieces.
	uint32_t at = 0;
	uint32_t size = 0;
	/// The half-open range of InterpEntryLayout::pieces to gather from.
	uint32_t first_piece = 0, piece_count = 0;
};

/// How a method's return value gets back to its caller.
///
/// Shared by both directions of the seam: plan_interp_entry () plans a return
/// arriving this way, and dyn-call.cpp's plan_dyn_call () plans one leaving
/// it. hidden_greg names the same register either direction reads or writes.
struct ReturnPlan {
	enum class Kind : uint8_t {
		None,      ///< the method returns nothing
		Hidden,    ///< written straight into a pointer the caller passed
		Registers, ///< gathered into scratch and scattered into the registers
	};

	Kind kind = Kind::None;
	uint32_t hidden_greg = 0; ///< which register the caller's pointer travels in
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
	void *imethod = nullptr; ///< InterpMethod *, opaque to this backend
};

/*
 * The other direction across the same seam: a call the interpreter makes into
 * a compiled body, whose prototype it only knows at run time. dyn-call.cpp
 * states the convention these types describe - the same one plan_interp_entry
 * () states above, restated outgoing rather than incoming. ArgPiece is what
 * both directions place a leaf as, and DynCallFrame's argument files match
 * InterpArgContext's sizes because both read the same six-greg, eight-freg
 * convention.
 */

/// The registers and stack a dyn call is made through, as dyn-call-thunk.S
/// loads them. The stack area is extended to hold DynCallPlan::stack_words.
struct DynCallFrame {
	uint64_t gregs[6];                  ///< rdi rsi rdx rcx r8 r9
	alignas (16) uint8_t fregs[8][16];  ///< xmm0 - xmm7
	uint64_t has_fp;                    ///< whether the call reads any of fregs
	uint64_t nstack;                    ///< words of stack the call passes
	uint64_t ret_gregs[3];              ///< rax rdx rcx, as the call left them
	alignas (16) uint8_t ret_fregs[4][16]; ///< xmm0 - xmm3, as the call left them
	uint64_t stack[];                   ///< an image of the callee's incoming stack arguments
};

static_assert (offsetof (DynCallFrame, gregs) == MONO_DYN_CALL_GREGS);
static_assert (offsetof (DynCallFrame, fregs) == MONO_DYN_CALL_FREGS);
static_assert (offsetof (DynCallFrame, has_fp) == MONO_DYN_CALL_HAS_FP);
static_assert (offsetof (DynCallFrame, nstack) == MONO_DYN_CALL_NSTACK);
static_assert (offsetof (DynCallFrame, ret_gregs) == MONO_DYN_CALL_RET_GREGS);
static_assert (offsetof (DynCallFrame, ret_fregs) == MONO_DYN_CALL_RET_FREGS);
static_assert (offsetof (DynCallFrame, stack) == MONO_DYN_CALL_STACK);
static_assert (sizeof (DynCallFrame) == MONO_DYN_CALL_SIZE);

/// The leaves of one logical argument, into DynCallPlan::pieces.
struct DynCallArg {
	uint32_t first_piece = 0, piece_count = 0;

	/// How to widen a scalar argument's one leaf when the callee's
	/// declaration promises the caller extends it - the attribute
	/// signature.cpp's integer_extension () left on the parameter. None for
	/// an aggregate's leaves, which carry no such promise.
	enum class Extend : uint8_t { None, Sign, Zero };
	Extend extend = Extend::None;
};

/// How a call of one prototype is made. It holds no metadata and never
/// changes, so a caller that reaches several methods of one prototype can
/// share one.
struct DynCallPlan {
	std::vector<DynCallArg> args; ///< the receiver first, when there is one
	std::vector<ArgPiece> pieces; ///< leaf storage shared by args and, when Kind::Registers, ret
	ReturnPlan ret;
	uint32_t stack_words = 0;
	bool wants_fp = false;
	uint32_t frame_size = 0; ///< bytes of DynCallFrame a call of this needs
};

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
struct LazyEntryABI : public llvm::orc::OrcX86_64_SysV {
	static constexpr unsigned ResolverCodeSize = 0xbe;

	static void writeResolverCode (char *resolver_mem,
	                               llvm::orc::ExecutorAddr resolver_addr,
	                               llvm::orc::ExecutorAddr reentry_fn,
	                               llvm::orc::ExecutorAddr reentry_ctx);
};

/// Builds the frame the re-entry resolver runs on, as a CFI program.
///
/// The frame it declares is the managed caller's rather than the resolver's
/// own. A walk that arrives during a compile therefore goes on to the code
/// that made the call. The rules are tied to the instruction offsets in the
/// resolver, so the two only stay true together.
std::vector<UnwindRecord> lazy_resolver_frame ();

} // namespace mono::arch

#endif

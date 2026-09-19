/**
 * \file
 * \brief The re-entry resolver a stub's first call lands in.
 *
 * A method's stub starts out pointing at an ORC trampoline, so the thread that
 * calls it first is the thread that compiles it. ORC's own resolver saves the
 * call's registers, asks the session for the method's address, and returns
 * into it. For the whole of that compile, the thread runs native code with
 * nothing on the LMF chain to say where it came from.
 *
 * That costs two things the runtime expects of any managed-to-native
 * transition. A signal-safe stack walk (an async abort deciding whether it can
 * hijack the thread, the profiler, the debugger) crosses a native frame only
 * through an LMF, so it reports a thread with no managed frames at all. And an
 * abort that arrives during the compile is therefore not delivered by hijack.
 * It is left as a flag instead, which an ordinary compiled body never polls.
 *
 * So the resolver here is ORC's with a mono lazy-entry frame around the
 * compile: an LMF carrying the caller's frame, and a forced interruption
 * checkpoint once it is unlinked. mini's generic trampoline does exactly this,
 * down to throwing from the caller's frame rather than from the trampoline's -
 * see the tail of mono_arch_create_generic_trampoline ().
 *
 * Methods with Vector256<T> signatures use LazyEntryAvxABI because
 * FXSAVE/FXRSTOR preserve only the low 128 bits of each YMM register.
 */

#include "arch/arch.hpp"

#include "debugging/perf/jitdump.hpp"

#include "mono/metadata/profiler-private.h"

#include <cstdint>
#include <cstring>
#include <iterator>
#include <vector>

#ifdef HOST_WIN32
#include "mono/mini/mini-windows.h"
#endif

using namespace llvm;
using namespace llvm::orc;

namespace mono::arch {

/*
 * The trampoline between the caller and here is what the CFA is picked to skip.
 * It is one `call` instruction and it gets no description, so a walk that steps
 * into it stops. A CFA of 0x18(%rbp) puts the return address a reader takes
 * from CFA-8 on the caller's, at 0x10(%rbp), and the trampoline is passed over.
 *
 * The offsets below are the ones in the code that follows. Move an instruction
 * and these move with it: a rule at the wrong offset unwinds to a wrong answer
 * rather than to none.
 */
std::vector<UnwindRecord>
lazy_resolver_frame ()
{
	const int32_t fp = dwarf_frame_pointer_reg;
	const int32_t sp = dwarf_stack_pointer_reg;

#ifdef HOST_WIN32
	/* The same frame, at the offsets the Microsoft x64 body below puts its
	 * instructions at: it reserves shadow space for each of its three calls,
	 * which moves everything after the prologue. */
	return {
		/* Entry. The trampoline's call sits above the caller's. */
		{ 0x00, MONO_UNWIND_OP_DEF_CFA, sp, 0x10 },

		/* push rbp */
		{ 0x01, MONO_UNWIND_OP_DEF_CFA_OFFSET, 0, 0x18 },
		{ 0x01, MONO_UNWIND_OP_OFFSET, fp, -0x18 },

		/* mov rbp, rsp - the rest of the body stands on this. */
		{ 0x04, MONO_UNWIND_OP_DEF_CFA_REGISTER, fp, 0 },

		/* pop rbp, so rbp is the caller's again and the CFA moves back. */
		{ 0xa4, MONO_UNWIND_OP_DEF_CFA, sp, 0x10 },
		{ 0xa4, MONO_UNWIND_OP_SAME_VALUE, fp, 0 },

		/* The throw path. A jump arrives here, so it needs the body's rules
		 * again rather than the ones the line above leaves. */
		{ 0xa5, MONO_UNWIND_OP_DEF_CFA, fp, 0x18 },
		{ 0xa5, MONO_UNWIND_OP_OFFSET, fp, -0x18 },

		/* The stack is cut back to the caller's return address, and rbp is
		 * the caller's. */
		{ 0xb3, MONO_UNWIND_OP_DEF_CFA, sp, 0x08 },
		{ 0xb3, MONO_UNWIND_OP_SAME_VALUE, fp, 0 },
	};
#else
	return {
		/* Entry. The trampoline's call sits above the caller's. */
		{ 0x00, MONO_UNWIND_OP_DEF_CFA, sp, 0x10 },

		/* pushq %rbp */
		{ 0x01, MONO_UNWIND_OP_DEF_CFA_OFFSET, 0, 0x18 },
		{ 0x01, MONO_UNWIND_OP_OFFSET, fp, -0x18 },

		/* movq %rsp, %rbp - the rest of the body stands on this. */
		{ 0x04, MONO_UNWIND_OP_DEF_CFA_REGISTER, fp, 0 },

		/* popq %rbp, so %rbp is the caller's again and the CFA moves back. */
		{ 0xa0, MONO_UNWIND_OP_DEF_CFA, sp, 0x10 },
		{ 0xa0, MONO_UNWIND_OP_SAME_VALUE, fp, 0 },

		/* The throw path. A jump arrives here, so it needs the body's rules
		 * again rather than the ones the line above leaves. */
		{ 0xa1, MONO_UNWIND_OP_DEF_CFA, fp, 0x18 },
		{ 0xa1, MONO_UNWIND_OP_OFFSET, fp, -0x18 },

		/* The stack is cut back to the caller's return address, and %rbp is
		 * the caller's. */
		{ 0xaf, MONO_UNWIND_OP_DEF_CFA, sp, 0x08 },
		{ 0xaf, MONO_UNWIND_OP_SAME_VALUE, fp, 0 },
	};
#endif
}

/* CFI records for the AVX resolver's larger YMM save area. */
std::vector<UnwindRecord>
lazy_resolver_frame_avx ()
{
	const int32_t fp = dwarf_frame_pointer_reg;
	const int32_t sp = dwarf_stack_pointer_reg;

#ifdef HOST_WIN32
	return {
		{ 0x00, MONO_UNWIND_OP_DEF_CFA, sp, 0x10 },
		{ 0x01, MONO_UNWIND_OP_DEF_CFA_OFFSET, 0, 0x18 },
		{ 0x01, MONO_UNWIND_OP_OFFSET, fp, -0x18 },
		{ 0x04, MONO_UNWIND_OP_DEF_CFA_REGISTER, fp, 0 },

		{ 0x1bf, MONO_UNWIND_OP_DEF_CFA, sp, 0x10 },
		{ 0x1bf, MONO_UNWIND_OP_SAME_VALUE, fp, 0 },

		{ 0x1c0, MONO_UNWIND_OP_DEF_CFA, fp, 0x18 },
		{ 0x1c0, MONO_UNWIND_OP_OFFSET, fp, -0x18 },

		{ 0x1cb, MONO_UNWIND_OP_DEF_CFA, sp, 0x08 },
		{ 0x1cb, MONO_UNWIND_OP_SAME_VALUE, fp, 0 },
	};
#else
	return {
		{ 0x00, MONO_UNWIND_OP_DEF_CFA, sp, 0x10 },
		{ 0x01, MONO_UNWIND_OP_DEF_CFA_OFFSET, 0, 0x18 },
		{ 0x01, MONO_UNWIND_OP_OFFSET, fp, -0x18 },
		{ 0x04, MONO_UNWIND_OP_DEF_CFA_REGISTER, fp, 0 },

		{ 0x1bb, MONO_UNWIND_OP_DEF_CFA, sp, 0x10 },
		{ 0x1bb, MONO_UNWIND_OP_SAME_VALUE, fp, 0 },

		{ 0x1bc, MONO_UNWIND_OP_DEF_CFA, fp, 0x18 },
		{ 0x1bc, MONO_UNWIND_OP_OFFSET, fp, -0x18 },

		{ 0x1c7, MONO_UNWIND_OP_DEF_CFA, sp, 0x08 },
		{ 0x1c7, MONO_UNWIND_OP_SAME_VALUE, fp, 0 },
	};
#endif
}

#ifdef HOST_WIN32

namespace {

/*
 * Pool construction calls the resolver publisher synchronously but does not
 * expose the resolver address. Pass it back through per-thread state so
 * concurrent domain creation remains independent.
 */
thread_local ExecutorAddr published_resolver;

/// One UNWIND_CODE: the offset of the instruction after the one it describes,
/// the operation, and the operation's four-bit operand.
constexpr uint16_t
unwind_slot (unsigned at, unsigned op, unsigned info)
{
	return (uint16_t) (at | (op << 8) | (info << 12));
}

/*
 * The same frame as unwind codes, because Windows reads neither the CFI program
 * above nor the jit dump it goes into. Without these an OS walk - a debugger's,
 * and the one ETW takes for each event - would stop at the resolver.
 *
 * Windows has no CFA to aim at. A walk ends by taking the return address from
 * wherever the codes leave rsp. So the skip the CFI program writes as a CFA of
 * [rbp + 0x18] is the last rule's eight bytes here. The pops bring rsp back to
 * the trampoline's return address, and that rule steps over it onto the
 * caller's.
 *
 * RtlVirtualUnwind reads the codes in the order they are written and each
 * undoes the instruction it names, so the table is the prologue backwards. Its
 * offsets are the code's, the same ones lazy_resolver_frame () is tied to.
 */
constexpr uint16_t resolver_unwind_codes[] = {
	/* sub rsp, 0x40 */
	unwind_slot (0x2d, UWOP_ALLOC_SMALL, (0x40 - 8) / 8),

	/* sub rsp, 0x208, whose size is a slot of its own. */
	unwind_slot (0x21, UWOP_ALLOC_LARGE, 0),
	0x208 / 8,

	/* The spills, which are arch::LazyEntryFrame. */
	unwind_slot (0x1a, UWOP_PUSH_NONVOL, 15), /* push r15 */
	unwind_slot (0x18, UWOP_PUSH_NONVOL, 14), /* push r14 */
	unwind_slot (0x16, UWOP_PUSH_NONVOL, 13), /* push r13 */
	unwind_slot (0x14, UWOP_PUSH_NONVOL, 12), /* push r12 */
	unwind_slot (0x12, UWOP_PUSH_NONVOL, 11), /* push r11 */
	unwind_slot (0x10, UWOP_PUSH_NONVOL, 10), /* push r10 */
	unwind_slot (0x0e, UWOP_PUSH_NONVOL, 9),  /* push r9 */
	unwind_slot (0x0c, UWOP_PUSH_NONVOL, 8),  /* push r8 */
	unwind_slot (0x0a, UWOP_PUSH_NONVOL, 7),  /* push rdi */
	unwind_slot (0x09, UWOP_PUSH_NONVOL, 6),  /* push rsi */
	unwind_slot (0x08, UWOP_PUSH_NONVOL, 2),  /* push rdx */
	unwind_slot (0x07, UWOP_PUSH_NONVOL, 1),  /* push rcx */
	unwind_slot (0x06, UWOP_PUSH_NONVOL, 3),  /* push rbx */
	unwind_slot (0x05, UWOP_PUSH_NONVOL, 0),  /* push rax */

	/* push rbp, which leaves rsp on the trampoline's return address. */
	unwind_slot (0x01, UWOP_PUSH_NONVOL, 5),

	// The step over it, onto the caller's. Offset zero keeps this rule at the
	// resolver's entry, where rbp and both return addresses are the caller's
	// already.
	unwind_slot (0x00, UWOP_ALLOC_SMALL, 0),
};

constexpr unsigned resolver_prologue_size = 0x2d;

/*
 * Where the rules stop being exact. The `add rsp, 0x40` at 0x75 takes the frame
 * apart ahead of the epilogue. Windows' own epilogue scan does not recognise
 * that epilogue either, because an fxrstor64 stands in the middle of it. The
 * range ends here rather than covering code the rules would unwind wrongly.
 * Every call in the resolver returns below it, so a walk loses nothing.
 */
constexpr unsigned resolver_described_size = 0x7c;

/// The x64 ABI requires the record to be DWORD-aligned and its code array to
/// hold an even number of slots.
constexpr unsigned resolver_unwind_offset
	= (LazyEntryABI::ResolverCodeSize + 3) & ~3u;
constexpr unsigned resolver_unwind_slots
	= (std::size (resolver_unwind_codes) + 1) & ~size_t (1);
constexpr unsigned resolver_published_size
	= resolver_unwind_offset + 4 + 2 * resolver_unwind_slots;

static_assert (std::size (resolver_unwind_codes) <= 255,
               "CountOfCodes is one byte");

/// Writes the resolver's unwind record behind its code and registers the range
/// holding both, so RtlLookupFunctionEntry answers for a return address in the
/// resolver.
///
/// The room behind the code is the rest of the page LocalTrampolinePool asked
/// the OS for. It is still writable here, because the pool protects the block
/// after writeResolverCode () returns.
void
publish_resolver_unwind_info (char *resolver_mem, ExecutorAddr resolver_addr)
{
	uint8_t *record = (uint8_t *) resolver_mem + resolver_unwind_offset;

	record[0] = 1; /* version 1, and no language-specific handler */
	record[1] = resolver_prologue_size;
	record[2] = (uint8_t) std::size (resolver_unwind_codes);
	record[3] = 0; /* no frame register: rsp is fixed across the body */

	std::memset (record + 4, 0, 2 * resolver_unwind_slots);
	std::memcpy (record + 4, resolver_unwind_codes,
	             sizeof (resolver_unwind_codes));

	char *code = resolver_addr.toPtr<char *> ();

	mono_arch_unwindinfo_insert_range_in_table (code, resolver_published_size);
	mono_arch_unwindinfo_insert_rt_func_in_table (code, resolver_described_size,
	                                              code + resolver_unwind_offset);

	published_resolver = resolver_addr;
}

/* Windows unwind codes for the AVX resolver's fixed-size stack frame. */
constexpr uint16_t resolver_avx_unwind_codes[] = {
	/* sub rsp, 0x40 */
	unwind_slot (0xba, UWOP_ALLOC_SMALL, (0x40 - 8) / 8),

	/* sub rsp, 0x408, whose size is a slot of its own. */
	unwind_slot (0x21, UWOP_ALLOC_LARGE, 0),
	0x408 / 8,

	/* The spills, which are arch::LazyEntryFrame. */
	unwind_slot (0x1a, UWOP_PUSH_NONVOL, 15), /* push r15 */
	unwind_slot (0x18, UWOP_PUSH_NONVOL, 14), /* push r14 */
	unwind_slot (0x16, UWOP_PUSH_NONVOL, 13), /* push r13 */
	unwind_slot (0x14, UWOP_PUSH_NONVOL, 12), /* push r12 */
	unwind_slot (0x12, UWOP_PUSH_NONVOL, 11), /* push r11 */
	unwind_slot (0x10, UWOP_PUSH_NONVOL, 10), /* push r10 */
	unwind_slot (0x0e, UWOP_PUSH_NONVOL, 9),  /* push r9 */
	unwind_slot (0x0c, UWOP_PUSH_NONVOL, 8),  /* push r8 */
	unwind_slot (0x0a, UWOP_PUSH_NONVOL, 7),  /* push rdi */
	unwind_slot (0x09, UWOP_PUSH_NONVOL, 6),  /* push rsi */
	unwind_slot (0x08, UWOP_PUSH_NONVOL, 2),  /* push rdx */
	unwind_slot (0x07, UWOP_PUSH_NONVOL, 1),  /* push rcx */
	unwind_slot (0x06, UWOP_PUSH_NONVOL, 3),  /* push rbx */
	unwind_slot (0x05, UWOP_PUSH_NONVOL, 0),  /* push rax */

	/* push rbp, which leaves rsp on the trampoline's return address. */
	unwind_slot (0x01, UWOP_PUSH_NONVOL, 5),

	unwind_slot (0x00, UWOP_ALLOC_SMALL, 0),
};

constexpr unsigned resolver_avx_prologue_size = 0xba;

/* Stop before the frame teardown, as in resolver_described_size above. */
constexpr unsigned resolver_avx_described_size = 0x106;

constexpr unsigned resolver_avx_unwind_offset
	= (LazyEntryAvxABI::ResolverCodeSize + 3) & ~3u;
constexpr unsigned resolver_avx_unwind_slots
	= (std::size (resolver_avx_unwind_codes) + 1) & ~size_t (1);
constexpr unsigned resolver_avx_published_size
	= resolver_avx_unwind_offset + 4 + 2 * resolver_avx_unwind_slots;

static_assert (std::size (resolver_avx_unwind_codes) <= 255,
               "CountOfCodes is one byte");

/// Publishes the Windows unwind record for the AVX resolver.
void
publish_resolver_unwind_info_avx (char *resolver_mem, ExecutorAddr resolver_addr)
{
	uint8_t *record = (uint8_t *) resolver_mem + resolver_avx_unwind_offset;

	record[0] = 1;
	record[1] = resolver_avx_prologue_size;
	record[2] = (uint8_t) std::size (resolver_avx_unwind_codes);
	record[3] = 0; /* no frame register: rsp is fixed across the body */

	std::memset (record + 4, 0, 2 * resolver_avx_unwind_slots);
	std::memcpy (record + 4, resolver_avx_unwind_codes,
	             sizeof (resolver_avx_unwind_codes));

	char *code = resolver_addr.toPtr<char *> ();

	mono_arch_unwindinfo_insert_range_in_table (code, resolver_avx_published_size);
	mono_arch_unwindinfo_insert_rt_func_in_table (code, resolver_avx_described_size,
	                                              code + resolver_avx_unwind_offset);

	published_resolver = resolver_addr;
}

} // namespace

#endif /* HOST_WIN32 */

ExecutorAddr
take_published_resolver ()
{
#ifdef HOST_WIN32
	ExecutorAddr resolver = published_resolver;
	published_resolver = ExecutorAddr ();
	return resolver;
#else
	return ExecutorAddr ();
#endif
}

void
unregister_resolver_unwind_info (ExecutorAddr resolver)
{
#ifdef HOST_WIN32
	if (!resolver)
		return;

	mono_arch_unwindinfo_remove_pc_range_in_table (resolver.toPtr<void *> ());
#else
	(void) resolver;
#endif
}

/*
 * ORC's OrcX86_64_SysV::writeResolverCode () with the lazy-entry frame added:
 * everything from `subq $0x20, %rsp` to the `callq` after it, the second half
 * of the leave sequence, and the throw path at the end. The re-entry argument
 * differs as well - ORC passes the trampoline, and this passes the frame, which
 * the callback reads the trampoline back out of along with the call's
 * registers. The rest is theirs instruction for instruction.
 *
 * The frame the resolver is entered on is what the whole thing is built
 * around. A managed `call` pushed the return address, the stub jumped to a
 * trampoline, and the trampoline called here, so once %rbp is pushed:
 *
 *      -0x70(%rbp) the spilled registers, %r15 lowest - arch::LazyEntryFrame
 *      0x00(%rbp)  the caller's %rbp, untouched since the call
 *      0x08(%rbp)  the trampoline's return address - which trampoline this is
 *      0x10(%rbp)  the caller's return address - where the call came from
 *      0x18(%rbp)  the caller's %rsp
 *
 * The last two are what the LMF stands on, and ORC's own trick with the first
 * two is what lets an exception be thrown as if the caller had raised it:
 * writing the landing address over the trampoline's return address and
 * returning enters the method with the caller's frame intact, so cutting the
 * stack back to 0x10(%rbp) and jumping instead puts the throw exactly where
 * mini's trampoline puts it after its `leave`.
 */
void
LazyEntryABI::write_resolver_body (char *resolver_mem, ExecutorAddr reentry_fn,
                                   ExecutorAddr reentry_ctx)
{
	static_assert (managed_frame_size == 0x20,
	               "the frame reservation is an immediate below");

#ifdef HOST_WIN32
	/*
	 * The same body against the Microsoft x64 convention. Three things move:
	 * the helpers take their arguments in rcx, rdx and r8; every call is owed
	 * 32 bytes of shadow space, which is reserved once alongside the frame
	 * rather than around each call; and the spill order stays as it is, so
	 * arch::LazyEntryFrame describes both.
	 */
	const uint8_t resolver_code[] = {
		// resolver_entry:
		0x55,                                     // 0x00: push      rbp
		0x48, 0x89, 0xe5,                         // 0x01: mov       rbp, rsp
		0x50,                                     // 0x04: push      rax
		0x53,                                     // 0x05: push      rbx
		0x51,                                     // 0x06: push      rcx
		0x52,                                     // 0x07: push      rdx
		0x56,                                     // 0x08: push      rsi
		0x57,                                     // 0x09: push      rdi
		0x41, 0x50,                               // 0x0a: push      r8
		0x41, 0x51,                               // 0x0c: push      r9
		0x41, 0x52,                               // 0x0e: push      r10
		0x41, 0x53,                               // 0x10: push      r11
		0x41, 0x54,                               // 0x12: push      r12
		0x41, 0x55,                               // 0x14: push      r13
		0x41, 0x56,                               // 0x16: push      r14
		0x41, 0x57,                               // 0x18: push      r15
		0x48, 0x81, 0xec, 0x08, 0x02, 0x00, 0x00, // 0x1a: sub       rsp, 0x208
		0x48, 0x0f, 0xae, 0x04, 0x24,             // 0x21: fxsave64  [rsp]

		// The lazy-entry frame, standing for the caller across the compile,
		// with the callees' shadow space under it.
		0x48, 0x81, 0xec, 0x40, 0x00, 0x00, 0x00, // 0x26: sub       rsp, 0x40
		0x48, 0x8d, 0x4c, 0x24, 0x20,             // 0x2d: lea       rcx, [rsp + 0x20]
		0x48, 0x8b, 0x55, 0x00,                   // 0x32: mov       rdx, [rbp]
		0x4c, 0x8d, 0x45, 0x18,                   // 0x36: lea       r8, [rbp + 0x18]
		0x48, 0xb8,                               // 0x3a: movabs    rax, <enter>

		// 0x3c: lazy_frame_enter ().
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,

		0xff, 0xd0,                               // 0x44: call      rax
		0x48, 0xb9,                               // 0x46: movabs    rcx, <CBMgr>

		// 0x48: JIT re-entry ctx addr.
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,

		0x48, 0x8d, 0x55, 0x90,                   // 0x50: lea       rdx, [rbp - 0x70]
		0x48, 0xb8,                               // 0x54: movabs    rax, <REntry>

		// 0x56: JIT re-entry fn addr.
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,

		0xff, 0xd0,                               // 0x5e: call      rax
		0x48, 0x89, 0x45, 0x08,                   // 0x60: mov       [rbp + 8], rax
		0x48, 0x8d, 0x4c, 0x24, 0x20,             // 0x64: lea       rcx, [rsp + 0x20]
		0x48, 0xb8,                               // 0x69: movabs    rax, <leave>

		// 0x6b: lazy_frame_leave ().
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,

		0xff, 0xd0,                               // 0x73: call      rax
		0x48, 0x81, 0xc4, 0x40, 0x00, 0x00, 0x00, // 0x75: add       rsp, 0x40
		0x48, 0x85, 0xc0,                         // 0x7c: test      rax, rax
		0x75, 0x24,                               // 0x7f: jne       throw

		0x48, 0x0f, 0xae, 0x0c, 0x24,             // 0x81: fxrstor64 [rsp]
		0x48, 0x81, 0xc4, 0x08, 0x02, 0x00, 0x00, // 0x86: add       rsp, 0x208
		0x41, 0x5f,                               // 0x8d: pop       r15
		0x41, 0x5e,                               // 0x8f: pop       r14
		0x41, 0x5d,                               // 0x91: pop       r13
		0x41, 0x5c,                               // 0x93: pop       r12
		0x41, 0x5b,                               // 0x95: pop       r11
		0x41, 0x5a,                               // 0x97: pop       r10
		0x41, 0x59,                               // 0x99: pop       r9
		0x41, 0x58,                               // 0x9b: pop       r8
		0x5f,                                     // 0x9d: pop       rdi
		0x5e,                                     // 0x9e: pop       rsi
		0x5a,                                     // 0x9f: pop       rdx
		0x59,                                     // 0xa0: pop       rcx
		0x5b,                                     // 0xa1: pop       rbx
		0x58,                                     // 0xa2: pop       rax
		0x5d,                                     // 0xa3: pop       rbp
		0xc3,                                     // 0xa4: ret

		// throw: the exception is in rax and the callee-saved registers are
		// already the caller's, so all that is left is to cut the stack back
		// to the call and enter the throw trampoline in the caller's place.
		0x48, 0x89, 0xc1,                         // 0xa5: mov       rcx, rax
		0x4c, 0x8b, 0x5d, 0x00,                   // 0xa8: mov       r11, [rbp]
		0x48, 0x8d, 0x65, 0x10,                   // 0xac: lea       rsp, [rbp + 0x10]
		0x4c, 0x89, 0xdd,                         // 0xb0: mov       rbp, r11
		0x48, 0xb8,                               // 0xb3: movabs    rax, <slot>

		// 0xb5: where the rethrow trampoline's address is kept.
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,

		0x48, 0x8b, 0x00,                         // 0xbd: mov       rax, [rax]
		0xff, 0xe0,                               // 0xc0: jmp       rax
	};

	const unsigned enter_fn_offset = 0x3c;
	const unsigned reentry_ctx_offset = 0x48;
	const unsigned reentry_fn_offset = 0x56;
	const unsigned leave_fn_offset = 0x6b;
	const unsigned rethrow_slot_offset = 0xb5;
#else
	const uint8_t resolver_code[] = {
		// resolver_entry:
		0x55,                                     // 0x00: pushq     %rbp
		0x48, 0x89, 0xe5,                         // 0x01: movq      %rsp, %rbp
		0x50,                                     // 0x04: pushq     %rax
		0x53,                                     // 0x05: pushq     %rbx
		0x51,                                     // 0x06: pushq     %rcx
		0x52,                                     // 0x07: pushq     %rdx
		0x56,                                     // 0x08: pushq     %rsi
		0x57,                                     // 0x09: pushq     %rdi
		0x41, 0x50,                               // 0x0a: pushq     %r8
		0x41, 0x51,                               // 0x0c: pushq     %r9
		0x41, 0x52,                               // 0x0e: pushq     %r10
		0x41, 0x53,                               // 0x10: pushq     %r11
		0x41, 0x54,                               // 0x12: pushq     %r12
		0x41, 0x55,                               // 0x14: pushq     %r13
		0x41, 0x56,                               // 0x16: pushq     %r14
		0x41, 0x57,                               // 0x18: pushq     %r15
		0x48, 0x81, 0xec, 0x08, 0x02, 0x00, 0x00, // 0x1a: subq      $0x208, %rsp
		0x48, 0x0f, 0xae, 0x04, 0x24,             // 0x21: fxsave64  (%rsp)

		// The lazy-entry frame, standing for the caller across the compile.
		0x48, 0x81, 0xec, 0x20, 0x00, 0x00, 0x00, // 0x26: subq      $0x20, %rsp
		0x48, 0x89, 0xe7,                         // 0x2d: movq      %rsp, %rdi
		0x48, 0x8b, 0x75, 0x00,                   // 0x30: movq      (%rbp), %rsi
		0x48, 0x8d, 0x55, 0x18,                   // 0x34: leaq      0x18(%rbp), %rdx
		0x48, 0xb8,                               // 0x38: movabsq   <enter>, %rax

		// 0x3a: lazy_frame_enter ().
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,

		0xff, 0xd0,                               // 0x42: callq     *%rax
		0x48, 0xbf,                               // 0x44: movabsq   <CBMgr>, %rdi

		// 0x46: JIT re-entry ctx addr.
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,

		0x48, 0x8d, 0x75, 0x90,                   // 0x4e: leaq      -0x70(%rbp), %rsi
		0x48, 0xb8,                               // 0x52: movabsq   <REntry>, %rax

		// 0x54: JIT re-entry fn addr.
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,

		0xff, 0xd0,                               // 0x5c: callq     *%rax
		0x48, 0x89, 0x45, 0x08,                   // 0x5e: movq      %rax, 8(%rbp)
		0x48, 0x89, 0xe7,                         // 0x62: movq      %rsp, %rdi
		0x48, 0xb8,                               // 0x65: movabsq   <leave>, %rax

		// 0x67: lazy_frame_leave ().
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,

		0xff, 0xd0,                               // 0x6f: callq     *%rax
		0x48, 0x81, 0xc4, 0x20, 0x00, 0x00, 0x00, // 0x71: addq      $0x20, %rsp
		0x48, 0x85, 0xc0,                         // 0x78: testq     %rax, %rax
		0x75, 0x24,                               // 0x7b: jne       throw

		0x48, 0x0f, 0xae, 0x0c, 0x24,             // 0x7d: fxrstor64 (%rsp)
		0x48, 0x81, 0xc4, 0x08, 0x02, 0x00, 0x00, // 0x82: addq      $0x208, %rsp
		0x41, 0x5f,                               // 0x89: popq      %r15
		0x41, 0x5e,                               // 0x8b: popq      %r14
		0x41, 0x5d,                               // 0x8d: popq      %r13
		0x41, 0x5c,                               // 0x8f: popq      %r12
		0x41, 0x5b,                               // 0x91: popq      %r11
		0x41, 0x5a,                               // 0x93: popq      %r10
		0x41, 0x59,                               // 0x95: popq      %r9
		0x41, 0x58,                               // 0x97: popq      %r8
		0x5f,                                     // 0x99: popq      %rdi
		0x5e,                                     // 0x9a: popq      %rsi
		0x5a,                                     // 0x9b: popq      %rdx
		0x59,                                     // 0x9c: popq      %rcx
		0x5b,                                     // 0x9d: popq      %rbx
		0x58,                                     // 0x9e: popq      %rax
		0x5d,                                     // 0x9f: popq      %rbp
		0xc3,                                     // 0xa0: retq

		// throw: the exception is in %rax and the callee-saved registers are
		// already the caller's, so all that is left is to cut the stack back
		// to the call and enter the throw trampoline in the caller's place.
		0x48, 0x89, 0xc7,                         // 0xa1: movq      %rax, %rdi
		0x4c, 0x8b, 0x5d, 0x00,                   // 0xa4: movq      (%rbp), %r11
		0x48, 0x8d, 0x65, 0x10,                   // 0xa8: leaq      0x10(%rbp), %rsp
		0x4c, 0x89, 0xdd,                         // 0xac: movq      %r11, %rbp
		0x48, 0xb8,                               // 0xaf: movabsq   <slot>, %rax

		// 0xb1: where the rethrow trampoline's address is kept.
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,

		0x48, 0x8b, 0x00,                         // 0xb9: movq      (%rax), %rax
		0xff, 0xe0,                               // 0xbc: jmpq      *%rax
	};

	const unsigned enter_fn_offset = 0x3a;
	const unsigned reentry_ctx_offset = 0x46;
	const unsigned reentry_fn_offset = 0x54;
	const unsigned leave_fn_offset = 0x67;
	const unsigned rethrow_slot_offset = 0xb1;
#endif

	static_assert (sizeof (resolver_code) == ResolverCodeSize,
	               "the resolver does not fit what the pool allocated for it");

	void (*enter_fn) (void *, uint64_t, uint64_t) = &lazy_frame_enter;
	void *(*leave_fn) (void *) = &lazy_frame_leave;
	void **rethrow_slot = rethrow_trampoline_slot ();

	std::memcpy (resolver_mem, resolver_code, sizeof (resolver_code));
	std::memcpy (resolver_mem + enter_fn_offset, &enter_fn, sizeof (enter_fn));
	std::memcpy (resolver_mem + reentry_ctx_offset, &reentry_ctx,
	             sizeof (uint64_t));
	std::memcpy (resolver_mem + reentry_fn_offset, &reentry_fn,
	             sizeof (uint64_t));
	std::memcpy (resolver_mem + leave_fn_offset, &leave_fn, sizeof (leave_fn));
	std::memcpy (resolver_mem + rethrow_slot_offset, &rethrow_slot,
	             sizeof (rethrow_slot));
}

void
LazyEntryABI::writeResolverCode (char *resolver_mem, ExecutorAddr resolver_addr,
                                 ExecutorAddr reentry_fn,
                                 ExecutorAddr reentry_ctx)
{
	write_resolver_body (resolver_mem, reentry_fn, reentry_ctx);

#ifdef HOST_WIN32
	publish_resolver_unwind_info (resolver_mem, resolver_addr);
#endif

	perf::FrameFunction fn;
	fn.size = ResolverCodeSize;
	fn.records = lazy_resolver_frame ();

	std::vector<perf::FrameFunction> functions;
	functions.push_back (std::move (fn));

	const char *name = "mono_lazy_entry_resolver";

	/* The pool gives the resolver a mapping of its own, so the room behind it
	 * is the rest of a page. */
	perf::publish (name,
	               { resolver_addr.toPtr<const uint8_t *> (), ResolverCodeSize,
	                 ResolverCodeSize + perf::code_slack () },
	               std::move (functions));

	/* The resolver carries no MonoTrampInfo, so raise_code_stub ()
	 * (mono/mini/mini-runtime.c) never reaches it. */
	MONO_PROFILER_RAISE (jit_code_stub,
	                     (resolver_addr.toPtr<const mono_byte *> (),
	                      ResolverCodeSize, name));
}

/*
 * AVX resolver body. It follows LazyEntryABI but saves and restores all YMM
 * registers around the calls. Recover the saved-register stack pointer from
 * rbp because the calls may clobber every volatile register.
 */
void
LazyEntryAvxABI::write_resolver_body (char *resolver_mem, ExecutorAddr reentry_fn,
                                      ExecutorAddr reentry_ctx)
{
	static_assert (managed_frame_size == 0x20,
	               "the frame reservation is an immediate below");

#ifdef HOST_WIN32
	/* Microsoft x64 variant, including callee shadow space. */
	const uint8_t resolver_code[] = {
		// resolver_entry:
		0x55,                                     // 0x00: push      rbp
		0x48, 0x89, 0xe5,                         // 0x01: mov       rbp, rsp
		0x50,                                     // 0x04: push      rax
		0x53,                                     // 0x05: push      rbx
		0x51,                                     // 0x06: push      rcx
		0x52,                                     // 0x07: push      rdx
		0x56,                                     // 0x08: push      rsi
		0x57,                                     // 0x09: push      rdi
		0x41, 0x50,                               // 0x0a: push      r8
		0x41, 0x51,                               // 0x0c: push      r9
		0x41, 0x52,                               // 0x0e: push      r10
		0x41, 0x53,                               // 0x10: push      r11
		0x41, 0x54,                               // 0x12: push      r12
		0x41, 0x55,                               // 0x14: push      r13
		0x41, 0x56,                               // 0x16: push      r14
		0x41, 0x57,                               // 0x18: push      r15
		0x48, 0x81, 0xec, 0x08, 0x04, 0x00, 0x00, // 0x1a: sub       rsp, 0x408
		0x48, 0x0f, 0xae, 0x04, 0x24,             // 0x21: fxsave64  [rsp]
		0xc5, 0xfc, 0x11, 0x84, 0x24,
		0x00, 0x02, 0x00, 0x00,                   // 0x26: vmovups   [rsp+0x200], ymm0
		0xc5, 0xfc, 0x11, 0x8c, 0x24,
		0x20, 0x02, 0x00, 0x00,                   // 0x2f: vmovups   [rsp+0x220], ymm1
		0xc5, 0xfc, 0x11, 0x94, 0x24,
		0x40, 0x02, 0x00, 0x00,                   // 0x38: vmovups   [rsp+0x240], ymm2
		0xc5, 0xfc, 0x11, 0x9c, 0x24,
		0x60, 0x02, 0x00, 0x00,                   // 0x41: vmovups   [rsp+0x260], ymm3
		0xc5, 0xfc, 0x11, 0xa4, 0x24,
		0x80, 0x02, 0x00, 0x00,                   // 0x4a: vmovups   [rsp+0x280], ymm4
		0xc5, 0xfc, 0x11, 0xac, 0x24,
		0xa0, 0x02, 0x00, 0x00,                   // 0x53: vmovups   [rsp+0x2a0], ymm5
		0xc5, 0xfc, 0x11, 0xb4, 0x24,
		0xc0, 0x02, 0x00, 0x00,                   // 0x5c: vmovups   [rsp+0x2c0], ymm6
		0xc5, 0xfc, 0x11, 0xbc, 0x24,
		0xe0, 0x02, 0x00, 0x00,                   // 0x65: vmovups   [rsp+0x2e0], ymm7
		0xc5, 0x7c, 0x11, 0x84, 0x24,
		0x00, 0x03, 0x00, 0x00,                   // 0x6e: vmovups   [rsp+0x300], ymm8
		0xc5, 0x7c, 0x11, 0x8c, 0x24,
		0x20, 0x03, 0x00, 0x00,                   // 0x77: vmovups   [rsp+0x320], ymm9
		0xc5, 0x7c, 0x11, 0x94, 0x24,
		0x40, 0x03, 0x00, 0x00,                   // 0x80: vmovups   [rsp+0x340], ymm10
		0xc5, 0x7c, 0x11, 0x9c, 0x24,
		0x60, 0x03, 0x00, 0x00,                   // 0x89: vmovups   [rsp+0x360], ymm11
		0xc5, 0x7c, 0x11, 0xa4, 0x24,
		0x80, 0x03, 0x00, 0x00,                   // 0x92: vmovups   [rsp+0x380], ymm12
		0xc5, 0x7c, 0x11, 0xac, 0x24,
		0xa0, 0x03, 0x00, 0x00,                   // 0x9b: vmovups   [rsp+0x3a0], ymm13
		0xc5, 0x7c, 0x11, 0xb4, 0x24,
		0xc0, 0x03, 0x00, 0x00,                   // 0xa4: vmovups   [rsp+0x3c0], ymm14
		0xc5, 0x7c, 0x11, 0xbc, 0x24,
		0xe0, 0x03, 0x00, 0x00,                   // 0xad: vmovups   [rsp+0x3e0], ymm15

		// The lazy-entry frame, standing for the caller across the compile,
		// with the callees' shadow space under it.
		0x48, 0x83, 0xec, 0x40,                   // 0xb6: sub       rsp, 0x40
		0x48, 0x8d, 0x4c, 0x24, 0x20,             // 0xba: lea       rcx, [rsp + 0x20]
		0x48, 0x8b, 0x55, 0x00,                   // 0xbf: mov       rdx, [rbp]
		0x4c, 0x8d, 0x45, 0x18,                   // 0xc3: lea       r8, [rbp + 0x18]
		0x48, 0xb8,                               // 0xc7: movabs    rax, <enter>

		// 0xc9: lazy_frame_enter ().
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,

		0xff, 0xd0,                               // 0xd1: call      rax
		0x48, 0xb9,                               // 0xd3: movabs    rcx, <CBMgr>

		// 0xd5: JIT re-entry ctx addr.
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,

		0x48, 0x8d, 0x55, 0x90,                   // 0xdd: lea       rdx, [rbp - 0x70]
		0x48, 0xb8,                               // 0xe1: movabs    rax, <REntry>

		// 0xe3: JIT re-entry fn addr.
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,

		0xff, 0xd0,                               // 0xeb: call      rax
		0x48, 0x89, 0x45, 0x08,                   // 0xed: mov       [rbp + 8], rax
		0x48, 0x8d, 0x4c, 0x24, 0x20,             // 0xf1: lea       rcx, [rsp + 0x20]
		0x48, 0xb8,                               // 0xf6: movabs    rax, <leave>

		// 0xf8: lazy_frame_leave ().
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,

		0xff, 0xd0,                               // 0x100: call      rax
		0x48, 0x83, 0xc4, 0x40,                   // 0x102: add       rsp, 0x40
		0x48, 0x85, 0xc0,                         // 0x106: test      rax, rax
		0x0f, 0x85, 0xb1, 0x00, 0x00, 0x00,       // 0x109: jne       throw

		0x48, 0x0f, 0xae, 0x0c, 0x24,             // 0x10f: fxrstor64 [rsp]
		0xc5, 0xfc, 0x10, 0x84, 0x24,
		0x00, 0x02, 0x00, 0x00,                   // 0x114: vmovups   ymm0, [rsp+0x200]
		0xc5, 0xfc, 0x10, 0x8c, 0x24,
		0x20, 0x02, 0x00, 0x00,                   // 0x11d: vmovups   ymm1, [rsp+0x220]
		0xc5, 0xfc, 0x10, 0x94, 0x24,
		0x40, 0x02, 0x00, 0x00,                   // 0x126: vmovups   ymm2, [rsp+0x240]
		0xc5, 0xfc, 0x10, 0x9c, 0x24,
		0x60, 0x02, 0x00, 0x00,                   // 0x12f: vmovups   ymm3, [rsp+0x260]
		0xc5, 0xfc, 0x10, 0xa4, 0x24,
		0x80, 0x02, 0x00, 0x00,                   // 0x138: vmovups   ymm4, [rsp+0x280]
		0xc5, 0xfc, 0x10, 0xac, 0x24,
		0xa0, 0x02, 0x00, 0x00,                   // 0x141: vmovups   ymm5, [rsp+0x2a0]
		0xc5, 0xfc, 0x10, 0xb4, 0x24,
		0xc0, 0x02, 0x00, 0x00,                   // 0x14a: vmovups   ymm6, [rsp+0x2c0]
		0xc5, 0xfc, 0x10, 0xbc, 0x24,
		0xe0, 0x02, 0x00, 0x00,                   // 0x153: vmovups   ymm7, [rsp+0x2e0]
		0xc5, 0x7c, 0x10, 0x84, 0x24,
		0x00, 0x03, 0x00, 0x00,                   // 0x15c: vmovups   ymm8, [rsp+0x300]
		0xc5, 0x7c, 0x10, 0x8c, 0x24,
		0x20, 0x03, 0x00, 0x00,                   // 0x165: vmovups   ymm9, [rsp+0x320]
		0xc5, 0x7c, 0x10, 0x94, 0x24,
		0x40, 0x03, 0x00, 0x00,                   // 0x16e: vmovups   ymm10, [rsp+0x340]
		0xc5, 0x7c, 0x10, 0x9c, 0x24,
		0x60, 0x03, 0x00, 0x00,                   // 0x177: vmovups   ymm11, [rsp+0x360]
		0xc5, 0x7c, 0x10, 0xa4, 0x24,
		0x80, 0x03, 0x00, 0x00,                   // 0x180: vmovups   ymm12, [rsp+0x380]
		0xc5, 0x7c, 0x10, 0xac, 0x24,
		0xa0, 0x03, 0x00, 0x00,                   // 0x189: vmovups   ymm13, [rsp+0x3a0]
		0xc5, 0x7c, 0x10, 0xb4, 0x24,
		0xc0, 0x03, 0x00, 0x00,                   // 0x192: vmovups   ymm14, [rsp+0x3c0]
		0xc5, 0x7c, 0x10, 0xbc, 0x24,
		0xe0, 0x03, 0x00, 0x00,                   // 0x19b: vmovups   ymm15, [rsp+0x3e0]
		0x48, 0x8d, 0x65, 0x90,                   // 0x1a4: lea       rsp, [rbp - 0x70]

		0x41, 0x5f,                               // 0x1a8: pop       r15
		0x41, 0x5e,                               // 0x1aa: pop       r14
		0x41, 0x5d,                               // 0x1ac: pop       r13
		0x41, 0x5c,                               // 0x1ae: pop       r12
		0x41, 0x5b,                               // 0x1b0: pop       r11
		0x41, 0x5a,                               // 0x1b2: pop       r10
		0x41, 0x59,                               // 0x1b4: pop       r9
		0x41, 0x58,                               // 0x1b6: pop       r8
		0x5f,                                     // 0x1b8: pop       rdi
		0x5e,                                     // 0x1b9: pop       rsi
		0x5a,                                     // 0x1ba: pop       rdx
		0x59,                                     // 0x1bb: pop       rcx
		0x5b,                                     // 0x1bc: pop       rbx
		0x58,                                     // 0x1bd: pop       rax
		0x5d,                                     // 0x1be: pop       rbp
		0xc3,                                     // 0x1bf: ret

		// throw: the exception is in rax and the callee-saved registers are
		// already the caller's, so all that is left is to cut the stack back
		// to the call and enter the throw trampoline in the caller's place.
		0x48, 0x89, 0xc1,                         // 0x1c0: mov       rcx, rax
		0x4c, 0x8b, 0x5d, 0x00,                   // 0x1c3: mov       r11, [rbp]
		0x48, 0x8d, 0x65, 0x10,                   // 0x1c7: lea       rsp, [rbp + 0x10]
		0x4c, 0x89, 0xdd,                         // 0x1cb: mov       rbp, r11
		0x48, 0xb8,                               // 0x1ce: movabs    rax, <slot>

		// 0x1d0: where the rethrow trampoline's address is kept.
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,

		0x48, 0x8b, 0x00,                         // 0x1d8: mov       rax, [rax]
		0xff, 0xe0,                               // 0x1db: jmp       rax
	};

	const unsigned enter_fn_offset = 0xc9;
	const unsigned reentry_ctx_offset = 0xd5;
	const unsigned reentry_fn_offset = 0xe3;
	const unsigned leave_fn_offset = 0xf8;
	const unsigned rethrow_slot_offset = 0x1d0;
#else
	const uint8_t resolver_code[] = {
		// resolver_entry:
		0x55,                                     // 0x00: pushq     %rbp
		0x48, 0x89, 0xe5,                         // 0x01: movq      %rsp, %rbp
		0x50,                                     // 0x04: pushq     %rax
		0x53,                                     // 0x05: pushq     %rbx
		0x51,                                     // 0x06: pushq     %rcx
		0x52,                                     // 0x07: pushq     %rdx
		0x56,                                     // 0x08: pushq     %rsi
		0x57,                                     // 0x09: pushq     %rdi
		0x41, 0x50,                               // 0x0a: pushq     %r8
		0x41, 0x51,                               // 0x0c: pushq     %r9
		0x41, 0x52,                               // 0x0e: pushq     %r10
		0x41, 0x53,                               // 0x10: pushq     %r11
		0x41, 0x54,                               // 0x12: pushq     %r12
		0x41, 0x55,                               // 0x14: pushq     %r13
		0x41, 0x56,                               // 0x16: pushq     %r14
		0x41, 0x57,                               // 0x18: pushq     %r15
		0x48, 0x81, 0xec, 0x08, 0x04, 0x00, 0x00, // 0x1a: subq      $0x408, %rsp
		0x48, 0x0f, 0xae, 0x04, 0x24,             // 0x21: fxsave64  (%rsp)
		0xc5, 0xfc, 0x11, 0x84, 0x24,
		0x00, 0x02, 0x00, 0x00,                   // 0x26: vmovups   %ymm0, 0x200(%rsp)
		0xc5, 0xfc, 0x11, 0x8c, 0x24,
		0x20, 0x02, 0x00, 0x00,                   // 0x2f: vmovups   %ymm1, 0x220(%rsp)
		0xc5, 0xfc, 0x11, 0x94, 0x24,
		0x40, 0x02, 0x00, 0x00,                   // 0x38: vmovups   %ymm2, 0x240(%rsp)
		0xc5, 0xfc, 0x11, 0x9c, 0x24,
		0x60, 0x02, 0x00, 0x00,                   // 0x41: vmovups   %ymm3, 0x260(%rsp)
		0xc5, 0xfc, 0x11, 0xa4, 0x24,
		0x80, 0x02, 0x00, 0x00,                   // 0x4a: vmovups   %ymm4, 0x280(%rsp)
		0xc5, 0xfc, 0x11, 0xac, 0x24,
		0xa0, 0x02, 0x00, 0x00,                   // 0x53: vmovups   %ymm5, 0x2a0(%rsp)
		0xc5, 0xfc, 0x11, 0xb4, 0x24,
		0xc0, 0x02, 0x00, 0x00,                   // 0x5c: vmovups   %ymm6, 0x2c0(%rsp)
		0xc5, 0xfc, 0x11, 0xbc, 0x24,
		0xe0, 0x02, 0x00, 0x00,                   // 0x65: vmovups   %ymm7, 0x2e0(%rsp)
		0xc5, 0x7c, 0x11, 0x84, 0x24,
		0x00, 0x03, 0x00, 0x00,                   // 0x6e: vmovups   %ymm8, 0x300(%rsp)
		0xc5, 0x7c, 0x11, 0x8c, 0x24,
		0x20, 0x03, 0x00, 0x00,                   // 0x77: vmovups   %ymm9, 0x320(%rsp)
		0xc5, 0x7c, 0x11, 0x94, 0x24,
		0x40, 0x03, 0x00, 0x00,                   // 0x80: vmovups   %ymm10, 0x340(%rsp)
		0xc5, 0x7c, 0x11, 0x9c, 0x24,
		0x60, 0x03, 0x00, 0x00,                   // 0x89: vmovups   %ymm11, 0x360(%rsp)
		0xc5, 0x7c, 0x11, 0xa4, 0x24,
		0x80, 0x03, 0x00, 0x00,                   // 0x92: vmovups   %ymm12, 0x380(%rsp)
		0xc5, 0x7c, 0x11, 0xac, 0x24,
		0xa0, 0x03, 0x00, 0x00,                   // 0x9b: vmovups   %ymm13, 0x3a0(%rsp)
		0xc5, 0x7c, 0x11, 0xb4, 0x24,
		0xc0, 0x03, 0x00, 0x00,                   // 0xa4: vmovups   %ymm14, 0x3c0(%rsp)
		0xc5, 0x7c, 0x11, 0xbc, 0x24,
		0xe0, 0x03, 0x00, 0x00,                   // 0xad: vmovups   %ymm15, 0x3e0(%rsp)

		// The lazy-entry frame, standing for the caller across the compile.
		0x48, 0x83, 0xec, 0x20,                   // 0xb6: subq      $0x20, %rsp
		0x48, 0x89, 0xe7,                         // 0xba: movq      %rsp, %rdi
		0x48, 0x8b, 0x75, 0x00,                   // 0xbd: movq      (%rbp), %rsi
		0x48, 0x8d, 0x55, 0x18,                   // 0xc1: leaq      0x18(%rbp), %rdx
		0x48, 0xb8,                               // 0xc5: movabsq   <enter>, %rax

		// 0xc7: lazy_frame_enter ().
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,

		0xff, 0xd0,                               // 0xcf: callq     *%rax
		0x48, 0xbf,                               // 0xd1: movabsq   <CBMgr>, %rdi

		// 0xd3: JIT re-entry ctx addr.
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,

		0x48, 0x8d, 0x75, 0x90,                   // 0xdb: leaq      -0x70(%rbp), %rsi
		0x48, 0xb8,                               // 0xdf: movabsq   <REntry>, %rax

		// 0xe1: JIT re-entry fn addr.
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,

		0xff, 0xd0,                               // 0xe9: callq     *%rax
		0x48, 0x89, 0x45, 0x08,                   // 0xeb: movq      %rax, 8(%rbp)
		0x48, 0x89, 0xe7,                         // 0xef: movq      %rsp, %rdi
		0x48, 0xb8,                               // 0xf2: movabsq   <leave>, %rax

		// 0xf4: lazy_frame_leave ().
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,

		0xff, 0xd0,                               // 0xfc: callq     *%rax
		0x48, 0x83, 0xc4, 0x20,                   // 0xfe: addq      $0x20, %rsp
		0x48, 0x85, 0xc0,                         // 0x102: testq     %rax, %rax
		0x0f, 0x85, 0xb1, 0x00, 0x00, 0x00,       // 0x105: jne       throw

		0x48, 0x0f, 0xae, 0x0c, 0x24,             // 0x10b: fxrstor64 (%rsp)
		0xc5, 0xfc, 0x10, 0x84, 0x24,
		0x00, 0x02, 0x00, 0x00,                   // 0x110: vmovups   0x200(%rsp), %ymm0
		0xc5, 0xfc, 0x10, 0x8c, 0x24,
		0x20, 0x02, 0x00, 0x00,                   // 0x119: vmovups   0x220(%rsp), %ymm1
		0xc5, 0xfc, 0x10, 0x94, 0x24,
		0x40, 0x02, 0x00, 0x00,                   // 0x122: vmovups   0x240(%rsp), %ymm2
		0xc5, 0xfc, 0x10, 0x9c, 0x24,
		0x60, 0x02, 0x00, 0x00,                   // 0x12b: vmovups   0x260(%rsp), %ymm3
		0xc5, 0xfc, 0x10, 0xa4, 0x24,
		0x80, 0x02, 0x00, 0x00,                   // 0x134: vmovups   0x280(%rsp), %ymm4
		0xc5, 0xfc, 0x10, 0xac, 0x24,
		0xa0, 0x02, 0x00, 0x00,                   // 0x13d: vmovups   0x2a0(%rsp), %ymm5
		0xc5, 0xfc, 0x10, 0xb4, 0x24,
		0xc0, 0x02, 0x00, 0x00,                   // 0x146: vmovups   0x2c0(%rsp), %ymm6
		0xc5, 0xfc, 0x10, 0xbc, 0x24,
		0xe0, 0x02, 0x00, 0x00,                   // 0x14f: vmovups   0x2e0(%rsp), %ymm7
		0xc5, 0x7c, 0x10, 0x84, 0x24,
		0x00, 0x03, 0x00, 0x00,                   // 0x158: vmovups   0x300(%rsp), %ymm8
		0xc5, 0x7c, 0x10, 0x8c, 0x24,
		0x20, 0x03, 0x00, 0x00,                   // 0x161: vmovups   0x320(%rsp), %ymm9
		0xc5, 0x7c, 0x10, 0x94, 0x24,
		0x40, 0x03, 0x00, 0x00,                   // 0x16a: vmovups   0x340(%rsp), %ymm10
		0xc5, 0x7c, 0x10, 0x9c, 0x24,
		0x60, 0x03, 0x00, 0x00,                   // 0x173: vmovups   0x360(%rsp), %ymm11
		0xc5, 0x7c, 0x10, 0xa4, 0x24,
		0x80, 0x03, 0x00, 0x00,                   // 0x17c: vmovups   0x380(%rsp), %ymm12
		0xc5, 0x7c, 0x10, 0xac, 0x24,
		0xa0, 0x03, 0x00, 0x00,                   // 0x185: vmovups   0x3a0(%rsp), %ymm13
		0xc5, 0x7c, 0x10, 0xb4, 0x24,
		0xc0, 0x03, 0x00, 0x00,                   // 0x18e: vmovups   0x3c0(%rsp), %ymm14
		0xc5, 0x7c, 0x10, 0xbc, 0x24,
		0xe0, 0x03, 0x00, 0x00,                   // 0x197: vmovups   0x3e0(%rsp), %ymm15
		0x48, 0x8d, 0x65, 0x90,                   // 0x1a0: leaq      -0x70(%rbp), %rsp

		0x41, 0x5f,                               // 0x1a4: popq      %r15
		0x41, 0x5e,                               // 0x1a6: popq      %r14
		0x41, 0x5d,                               // 0x1a8: popq      %r13
		0x41, 0x5c,                               // 0x1aa: popq      %r12
		0x41, 0x5b,                               // 0x1ac: popq      %r11
		0x41, 0x5a,                               // 0x1ae: popq      %r10
		0x41, 0x59,                               // 0x1b0: popq      %r9
		0x41, 0x58,                               // 0x1b2: popq      %r8
		0x5f,                                     // 0x1b4: popq      %rdi
		0x5e,                                     // 0x1b5: popq      %rsi
		0x5a,                                     // 0x1b6: popq      %rdx
		0x59,                                     // 0x1b7: popq      %rcx
		0x5b,                                     // 0x1b8: popq      %rbx
		0x58,                                     // 0x1b9: popq      %rax
		0x5d,                                     // 0x1ba: popq      %rbp
		0xc3,                                     // 0x1bb: retq

		// throw: the exception is in %rax and the callee-saved registers are
		// already the caller's, so all that is left is to cut the stack back
		// to the call and enter the throw trampoline in the caller's place.
		0x48, 0x89, 0xc7,                         // 0x1bc: movq      %rax, %rdi
		0x4c, 0x8b, 0x5d, 0x00,                   // 0x1bf: movq      (%rbp), %r11
		0x48, 0x8d, 0x65, 0x10,                   // 0x1c3: leaq      0x10(%rbp), %rsp
		0x4c, 0x89, 0xdd,                         // 0x1c7: movq      %r11, %rbp
		0x48, 0xb8,                               // 0x1ca: movabsq   <slot>, %rax

		// 0x1cc: where the rethrow trampoline's address is kept.
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,

		0x48, 0x8b, 0x00,                         // 0x1d4: movq      (%rax), %rax
		0xff, 0xe0,                               // 0x1d7: jmpq      *%rax
	};

	const unsigned enter_fn_offset = 0xc7;
	const unsigned reentry_ctx_offset = 0xd3;
	const unsigned reentry_fn_offset = 0xe1;
	const unsigned leave_fn_offset = 0xf4;
	const unsigned rethrow_slot_offset = 0x1cc;
#endif

	static_assert (sizeof (resolver_code) == ResolverCodeSize,
	               "the resolver does not fit what the pool allocated for it");

	void (*enter_fn) (void *, uint64_t, uint64_t) = &lazy_frame_enter;
	void *(*leave_fn) (void *) = &lazy_frame_leave;
	void **rethrow_slot = rethrow_trampoline_slot ();

	std::memcpy (resolver_mem, resolver_code, sizeof (resolver_code));
	std::memcpy (resolver_mem + enter_fn_offset, &enter_fn, sizeof (enter_fn));
	std::memcpy (resolver_mem + reentry_ctx_offset, &reentry_ctx,
	             sizeof (uint64_t));
	std::memcpy (resolver_mem + reentry_fn_offset, &reentry_fn,
	             sizeof (uint64_t));
	std::memcpy (resolver_mem + leave_fn_offset, &leave_fn, sizeof (leave_fn));
	std::memcpy (resolver_mem + rethrow_slot_offset, &rethrow_slot,
	             sizeof (rethrow_slot));
}

void
LazyEntryAvxABI::writeResolverCode (char *resolver_mem, ExecutorAddr resolver_addr,
                                    ExecutorAddr reentry_fn,
                                    ExecutorAddr reentry_ctx)
{
	write_resolver_body (resolver_mem, reentry_fn, reentry_ctx);

#ifdef HOST_WIN32
	publish_resolver_unwind_info_avx (resolver_mem, resolver_addr);
#endif

	perf::FrameFunction fn;
	fn.size = ResolverCodeSize;
	fn.records = lazy_resolver_frame_avx ();

	std::vector<perf::FrameFunction> functions;
	functions.push_back (std::move (fn));

	const char *name = "mono_lazy_entry_resolver_avx";

	perf::publish (name,
	               { resolver_addr.toPtr<const uint8_t *> (), ResolverCodeSize,
	                 ResolverCodeSize + perf::code_slack () },
	               std::move (functions));

	MONO_PROFILER_RAISE (jit_code_stub,
	                     (resolver_addr.toPtr<const mono_byte *> (),
	                      ResolverCodeSize, name));
}

} // namespace mono::arch

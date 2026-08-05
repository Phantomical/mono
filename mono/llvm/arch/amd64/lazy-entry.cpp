/**
 * \file
 * \brief The re-entry resolver a stub's first call lands in.
 *
 * A method's stub starts out pointing at an ORC trampoline, so the thread that
 * calls it first is the thread that compiles it. ORC's own resolver saves the
 * call's registers, asks the session for the method's address and returns into
 * it - and for the whole of that compile the thread is running native code
 * with nothing on the LMF chain to say where it came from.
 *
 * That costs two things the runtime expects of any managed-to-native
 * transition. A signal-safe stack walk (an async abort deciding whether it can
 * hijack the thread, the profiler, the debugger) crosses a native frame only
 * through an LMF, so it reports a thread with no managed frames at all. And an
 * abort that arrives during the compile is therefore not delivered by hijack;
 * it is left as a flag, which an ordinary compiled body never polls.
 *
 * So the resolver here is ORC's with a mono lazy-entry frame around the
 * compile: an LMF carrying the caller's frame, and a forced interruption
 * checkpoint once it is unlinked. mini's generic trampoline does exactly this,
 * down to throwing from the caller's frame rather than from the trampoline's -
 * see the tail of mono_arch_create_generic_trampoline ().
 */

#include "arch/arch.hpp"

#include <cstring>

using namespace llvm;
using namespace llvm::orc;

namespace mono::arch {

/*
 * ORC's OrcX86_64_SysV::writeResolverCode () with the lazy-entry frame added:
 * everything from `subq $0x20, %rsp` to the `callq` after it, the second half
 * of the leave sequence, and the throw path at the end. The rest is theirs
 * instruction for instruction, so the two can be diffed.
 *
 * The frame the resolver is entered on is what the whole thing is built
 * around. A managed `call` pushed the return address, the stub jumped to a
 * trampoline, and the trampoline called here, so once %rbp is pushed:
 *
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
LazyEntryABI::writeResolverCode (char *resolver_mem, ExecutorAddr resolver_addr,
                                 ExecutorAddr reentry_fn,
                                 ExecutorAddr reentry_ctx)
{
	(void) resolver_addr; /* Nothing here is written relative to itself. */

	static_assert (lazy_frame_size == 0x20,
	               "the frame reservation is an immediate below");

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

		0x48, 0x8b, 0x75, 0x08,                   // 0x4e: movq      8(%rbp), %rsi
		0x48, 0x83, 0xee, 0x06,                   // 0x52: subq      $6, %rsi
		0x48, 0xb8,                               // 0x56: movabsq   <REntry>, %rax

		// 0x58: JIT re-entry fn addr.
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,

		0xff, 0xd0,                               // 0x60: callq     *%rax
		0x48, 0x89, 0x45, 0x08,                   // 0x62: movq      %rax, 8(%rbp)
		0x48, 0x89, 0xe7,                         // 0x66: movq      %rsp, %rdi
		0x48, 0xb8,                               // 0x69: movabsq   <leave>, %rax

		// 0x6b: lazy_frame_leave ().
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,

		0xff, 0xd0,                               // 0x73: callq     *%rax
		0x48, 0x81, 0xc4, 0x20, 0x00, 0x00, 0x00, // 0x75: addq      $0x20, %rsp
		0x48, 0x85, 0xc0,                         // 0x7c: testq     %rax, %rax
		0x75, 0x24,                               // 0x7f: jne       throw

		0x48, 0x0f, 0xae, 0x0c, 0x24,             // 0x81: fxrstor64 (%rsp)
		0x48, 0x81, 0xc4, 0x08, 0x02, 0x00, 0x00, // 0x86: addq      $0x208, %rsp
		0x41, 0x5f,                               // 0x8d: popq      %r15
		0x41, 0x5e,                               // 0x8f: popq      %r14
		0x41, 0x5d,                               // 0x91: popq      %r13
		0x41, 0x5c,                               // 0x93: popq      %r12
		0x41, 0x5b,                               // 0x95: popq      %r11
		0x41, 0x5a,                               // 0x97: popq      %r10
		0x41, 0x59,                               // 0x99: popq      %r9
		0x41, 0x58,                               // 0x9b: popq      %r8
		0x5f,                                     // 0x9d: popq      %rdi
		0x5e,                                     // 0x9e: popq      %rsi
		0x5a,                                     // 0x9f: popq      %rdx
		0x59,                                     // 0xa0: popq      %rcx
		0x5b,                                     // 0xa1: popq      %rbx
		0x58,                                     // 0xa2: popq      %rax
		0x5d,                                     // 0xa3: popq      %rbp
		0xc3,                                     // 0xa4: retq

		// throw: the exception is in %rax and the callee-saved registers are
		// already the caller's, so all that is left is to cut the stack back
		// to the call and enter the throw trampoline in the caller's place.
		0x48, 0x89, 0xc7,                         // 0xa5: movq      %rax, %rdi
		0x4c, 0x8b, 0x5d, 0x00,                   // 0xa8: movq      (%rbp), %r11
		0x48, 0x8d, 0x65, 0x10,                   // 0xac: leaq      0x10(%rbp), %rsp
		0x4c, 0x89, 0xdd,                         // 0xb0: movq      %r11, %rbp
		0x48, 0xb8,                               // 0xb3: movabsq   <slot>, %rax

		// 0xb5: where the rethrow trampoline's address is kept.
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,

		0x48, 0x8b, 0x00,                         // 0xbd: movq      (%rax), %rax
		0xff, 0xe0,                               // 0xc0: jmpq      *%rax
	};

	static_assert (sizeof (resolver_code) == ResolverCodeSize,
	               "the resolver does not fit what the pool allocated for it");

	const unsigned enter_fn_offset = 0x3a;
	const unsigned reentry_ctx_offset = 0x46;
	const unsigned reentry_fn_offset = 0x58;
	const unsigned leave_fn_offset = 0x6b;
	const unsigned rethrow_slot_offset = 0xb5;

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

} // namespace mono::arch

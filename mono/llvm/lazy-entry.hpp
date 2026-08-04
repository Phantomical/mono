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

#ifndef MONO_LLVM_LAZY_ENTRY_HPP
#define MONO_LLVM_LAZY_ENTRY_HPP

#include <llvm/ExecutionEngine/Orc/OrcABISupport.h>

#include <cstddef>
#include <cstdint>

namespace mono {

/*
 * Stack the resolver reserves for the lazy-entry frame. The runtime side casts
 * it to its own struct and static_asserts it fits; 32 keeps the frame that
 * follows 16-aligned.
 */
constexpr unsigned lazy_frame_size = 32;

/// Link FRAME onto the LMF chain, standing for the managed frame that called
/// the stub. Does nothing on a thread that is not running managed code.
void lazy_frame_enter (void *frame, uint64_t caller_rbp, uint64_t caller_rsp);

/// Unlink FRAME and take any interruption that arrived while it was linked,
/// returning the exception to throw or null.
void *lazy_frame_leave (void *frame);

/// The slot holding the runtime's rethrow-preserving throw trampoline. Read
/// through at throw time, so this is callable before the runtime has one.
void **rethrow_trampoline_slot ();

/// ORC's x86-64 SysV re-entry ABI, resolving through a mono lazy-entry frame.
struct MonoOrcX86_64_SysV : public llvm::orc::OrcX86_64_SysV {
	static constexpr unsigned ResolverCodeSize = 0xc2;

	static void writeResolverCode (char *resolver_mem,
	                               llvm::orc::ExecutorAddr resolver_addr,
	                               llvm::orc::ExecutorAddr reentry_fn,
	                               llvm::orc::ExecutorAddr reentry_ctx);
};

} // namespace mono

#endif

/**
 * \file
 * \brief What the JIT knows about the machine it generates code for.
 *
 * Everything that names a register, encodes an instruction, or restates the
 * runtime's calling convention lives behind this header, under arch/<target>/.
 * The rest of mono/llvm is written against the names below, so a port is a new
 * sibling directory rather than a hunt through the backend for the amd64 in it.
 *
 * The declarations come in two halves. The functions are the same everywhere
 * and are declared here once; the types and constants their signatures are
 * spelled in - the resolver's ABI class, the size of a stub - only the target
 * can supply, so its own header comes in first and fills those in.
 *
 * Nothing here may pull mono's metadata headers. jit.cpp sits on this side of
 * the seam and is deliberately runtime-free so the unit tests can drive it.
 */

#ifndef MONO_LLVM_ARCH_ARCH_HPP
#define MONO_LLVM_ARCH_ARCH_HPP

#include "config.h"

#if defined(TARGET_AMD64)
#include "arch/amd64/amd64.hpp"
#else
#error "the LLVM JIT has no arch/ directory for this target"
#endif

#include <llvm/ADT/StringRef.h>
#include <llvm/IR/PassManager.h>

#include <cstdint>

namespace llvm {
class Function;
class IRBuilderBase;
class Module;
class Value;
} // namespace llvm

/* Only ever read through, so the tag declaration is all this seam needs. */
typedef struct _MonoMethodSignature MonoMethodSignature;

namespace mono::arch {

/* -- The lazy-entry resolver --------------------------------------------- */

/// Link FRAME onto the LMF chain, standing for the managed frame that called
/// the stub. Does nothing on a thread that is not running managed code.
void lazy_frame_enter (void *frame, uint64_t caller_fp, uint64_t caller_sp);

/// Unlink FRAME and take any interruption that arrived while it was linked,
/// returning the exception to throw or null.
void *lazy_frame_leave (void *frame);

/// The slot holding the runtime's rethrow-preserving throw trampoline. Read
/// through at throw time, so this is callable before the runtime has one.
void **rethrow_trampoline_slot ();

/* -- Redirectable stubs --------------------------------------------------- */

/// Write a stub at CODE jumping through SLOT, filling the whole stub block.
void write_jump_stub (char *code, const void *slot);

/* -- Unwinding and dispatch ----------------------------------------------- */

/// Can a stack walk rebuild HW_REG for the frame it is looking at?
///
/// True for the stack pointer and for the callee-saved registers the unwind
/// info restores; a caller-saved register's value is long gone by the time the
/// walk reaches the frame that set it.
bool reg_is_recoverable (int hw_reg);

/// Read the exception a filter is entered with, out of the register the
/// runtime's dispatcher leaves it in.
llvm::Value *emit_entered_exception (llvm::IRBuilderBase &b);

/* -- The LMF a wrapper links ---------------------------------------------- */

/// Clobber the callee-saved registers so the prologue saves all of them.
///
/// Unwinding through an LMF hop rebuilds them from the frame's own unwind
/// info, which can only restore what the frame bothered to save.
void emit_callee_saved_clobber (llvm::IRBuilderBase &b);

/// Store the frame's settled frame- and stack-pointer values into the LMF at
/// SLOT: the two registers a stack walk resumes from.
void emit_lmf_capture_registers (llvm::IRBuilderBase &b, llvm::Value *slot);

/// Read the address of this thread's LMF chain head straight out of thread-local
/// storage, or null when this machine cannot reach it without a call.
llvm::Value *emit_lmf_address (llvm::IRBuilderBase &b);

/* -- The boundary calling convention -------------------------------------- */

/// The attribute naming a call (or a declaration every call to which) crosses
/// the legacy boundary. Its value is one of the flavor strings below.
constexpr llvm::StringRef legacy_cc_attribute = "mono-legacycc";

/// Which side of the boundary the callee is, and where the runtime's
/// convention puts a hidden return pointer if the return needs one.
enum class LegacyFlavor {
	/// Code compiled by mini for a managed signature: value types ride the
	/// integer file, a big return travels through a pointer at argument 0.
	Managed,
	/// Managed, but the hidden return pointer sits at argument 1: the first
	/// argument is a this (or a reference the trampolines treat as one) that
	/// the runtime insists on finding in the first register.
	ManagedVret1,
	/// A native C function: the C classification, floats in the float file.
	Pinvoke,
};

llvm::StringRef legacy_flavor_value (LegacyFlavor flavor);

/// Which of the two managed flavors a call through SIG is. Whether the
/// signature is managed at all is the caller's question.
LegacyFlavor managed_call_flavor (MonoMethodSignature *sig);

/// Rewrites every `mono-legacycc` call into the runtime's convention. Runs
/// after the optimization pipeline; the marked calls are opaque to it either
/// way, but the natural-typed IR is what it should be optimizing.
class LegacyAbiPass : public llvm::PassInfoMixin<LegacyAbiPass> {
public:
	llvm::PreservedAnalyses run (llvm::Module &m,
	                             llvm::ModuleAnalysisManager &mam);
};

/// Create NAME in M: a legacy-convention entry point that unpacks its
/// arguments out of the convention into natural values and calls TARGET (a
/// fastcc function in M) with them. This is what the runtime publishes for a
/// method - every caller that is not generated code enters through it.
///
/// THROUGH, when given, is the address the call is actually made to; TARGET
/// then only supplies the shape of the call. That is how an entry emitted
/// beside the body it enters still reaches it through the body's stub.
///
/// THIS_ADJUST, when nonzero, is added to the first argument before it is
/// passed on: the unboxing entry a value type's virtual method is reached
/// through steps its receiver past the object header.
llvm::Function *create_legacy_entry_thunk (llvm::Module &m, llvm::StringRef name,
                                           llvm::Function *target,
                                           LegacyFlavor flavor,
                                           llvm::Value *through = nullptr,
                                           unsigned this_adjust = 0);

} // namespace mono::arch

#endif

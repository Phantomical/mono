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
#include <llvm/Support/Error.h>

#include <cstddef>
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

/// Link a frame onto the LMF chain for the length of a call into the
/// interpreter, standing for the managed frame that made the call. FRAME is
/// interp_frame_size bytes.
void interp_frame_enter (void *frame, const InterpArgContext *args);

/// Unlink a frame linked by interp_frame_enter.
void interp_frame_leave (void *frame);

/// The slot holding the runtime's rethrow-preserving throw trampoline. Read
/// through at throw time, so this is callable before the runtime has one.
void **rethrow_trampoline_slot ();

/* -- Redirectable stubs --------------------------------------------------- */

/// Write a stub at CODE jumping through SLOT, filling the whole stub block.
void write_jump_stub (char *code, const void *slot);

/// Write a stub at CODE that loads KEY into the register a callee's key
/// travels in and then jumps through SLOT, filling the whole stub block.
///
/// The register is the one the `nest` attribute pins an argument to, so a
/// function entered this way reads KEY as its nest parameter. That is how one
/// body shared by many methods is told which of them it was entered for.
void write_keyed_jump_stub (char *code, const void *slot, const void *key);

/* -- Call counting -------------------------------------------------------- */

/// Lay out a call-counting thunk in BLOCK, COUNTER_THUNK_SIZE bytes at
/// COUNTER_THUNK_ALIGNMENT, and return the address callers enter it at.
///
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

/* -- Crossing into C ------------------------------------------------------ */

/// The attribute naming a call (or a declaration every call to which) crosses
/// into C. It is a marker and carries no value.
constexpr llvm::StringRef mono_cc_attribute = "monocc";

/// Rewrites every `monocc` call into the C convention. Runs after the
/// optimization pipeline; the marked calls are opaque to it either way, but the
/// natural-typed IR is what it should be optimizing.
class MonoAbiPass : public llvm::PassInfoMixin<MonoAbiPass> {
public:
	llvm::PreservedAnalyses run (llvm::Module &m,
	                             llvm::ModuleAnalysisManager &mam);
};

/// Create \p name in \p m: a C-convention entry point that unpacks its
/// arguments out of the convention into natural values and calls \p target (a
/// function in \p m) with them. This is what a wrapper generated for native
/// code to enter is published as.
///
/// \p through, when given, is the address the call is actually made to, and
/// \p target then only supplies the shape of the call. That is how an entry
/// emitted beside the body it enters still reaches it through the body's stub.
llvm::Function *create_mono_entry_thunk (llvm::Module &m, llvm::StringRef name,
                                         llvm::Function *target,
                                         llvm::Value *through = nullptr);

/// Create NAME in M: TARGET's own prototype, with ADJUST added to the receiver
/// before the call is passed on to THROUGH. This is the entry a call off a value
/// type's vtable or IMT arrives at, stepping the boxed receiver past the object
/// header.
///
/// Both ends speak the same convention, so the forward is a jump and the entry
/// leaves no frame of its own behind.
llvm::Function *create_unbox_entry (llvm::Module &m, llvm::StringRef name,
                                    llvm::Function *target, llvm::Value *through,
                                    unsigned adjust);

/* -- Entering the interpreter --------------------------------------------- */

/// How a call to a method is taken apart into the arguments the interpreter
/// wants. The shape is a declaration of the method in this backend's own
/// convention and only its type is read; the signature is the method's.
///
/// An error says this machine's entry cannot carry a call like that, and the
/// method has to be compiled rather than interpreted.
llvm::Expected<InterpEntryLayout> plan_interp_entry (llvm::Function *shape,
                                                     MonoMethodSignature *sig);

/// The code an interpreted method's stub is pointed at, entered with the
/// MonoMethod * in the key register. Registered with the runtime on first call
/// so that a stack walk can cross it.
void *interp_entry_thunk ();

} // namespace mono::arch

#endif

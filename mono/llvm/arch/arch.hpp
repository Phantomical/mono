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
 * and are declared here once. The types and constants their signatures are
 * spelled in - the resolver's ABI class, the size of a stub - only the target
 * can supply. Its own header comes in first and fills those in.
 *
 * The arch/ headers must not pull in mono's metadata headers. jit.cpp sits on
 * the same side of that seam and is deliberately runtime-free, so the unit
 * tests can drive it.
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

/// Links \p frame onto the LMF chain, standing for the managed frame that
/// called the stub. Does nothing on a thread that is not running managed
/// code.
void lazy_frame_enter (void *frame, uint64_t caller_fp, uint64_t caller_sp);

/// Unlinks \p frame and takes any interruption that arrived while it was
/// linked, returning the exception to throw or null.
void *lazy_frame_leave (void *frame);

/// Links a frame onto the LMF chain for the length of a call into the
/// interpreter, standing for the managed frame that made the call. \p frame
/// is interp_frame_size bytes.
void interp_frame_enter (void *frame, const InterpArgContext *args);

/// Unlinks a frame linked by interp_frame_enter.
void interp_frame_leave (void *frame);

/// Returns the slot holding the runtime's rethrow-preserving throw
/// trampoline. The slot's value is read at throw time, so this can be called
/// before the runtime installs the trampoline.
void **rethrow_trampoline_slot ();

/* -- Unwinding and dispatch ----------------------------------------------- */

/// Whether a stack walk can rebuild \p hw_reg for the frame it is looking at.
///
/// True for the stack pointer and for the callee-saved registers the unwind
/// info restores. A caller-saved register's value is long gone by the time
/// the walk reaches the frame that set it.
bool reg_is_recoverable (int hw_reg);

/// Reads the exception a filter is entered with, out of the register where
/// the unwinder leaves it.
llvm::Value *emit_entered_exception (llvm::IRBuilderBase &b);

/* -- The LMF a wrapper links ---------------------------------------------- */

/// Clobbers the callee-saved registers so the prologue saves all of them.
///
/// Unwinding through an LMF hop rebuilds them from the frame's own unwind
/// info, which can only restore what the frame actually saved.
void emit_callee_saved_clobber (llvm::IRBuilderBase &b);

/// Stores the frame's settled frame- and stack-pointer values into the LMF at
/// \p slot: the two registers a stack walk uses to resume.
void emit_lmf_capture_registers (llvm::IRBuilderBase &b, llvm::Value *slot);

/// Reads the address of this thread's LMF chain head straight out of
/// thread-local storage, or null when this machine cannot reach it without a
/// call.
llvm::Value *emit_lmf_address (llvm::IRBuilderBase &b);

/* -- Crossing into C ------------------------------------------------------ */

/// The attribute marking a call, or every call to a declaration, as crossing
/// into C. It is a marker and carries no value.
constexpr llvm::StringRef mono_cc_attribute = "monocc";

/// Rewrites every `monocc` call into the C convention. Runs after the
/// optimization pipeline. The marked calls are opaque to the optimizer either
/// way, but it must see natural-typed IR rather than the lowered form.
class MonoAbiPass : public llvm::PassInfoMixin<MonoAbiPass> {
public:
	llvm::PreservedAnalyses run (llvm::Module &m,
	                             llvm::ModuleAnalysisManager &mam);
};

/// Creates \p name in \p m: a C-convention entry point that unpacks its
/// arguments out of the convention into natural values and calls \p target
/// with them. \p target must be a function in \p m. This is what a wrapper
/// generated for native code to enter is published as.
///
/// \p through, when given, is the call's actual target address, and \p target
/// then only supplies the shape of the call. That is how an entry emitted
/// beside the body it enters still reaches it through the body's stub.
llvm::Function *create_mono_entry_thunk (llvm::Module &m, llvm::StringRef name,
                                         llvm::Function *target,
                                         llvm::Value *through = nullptr);

/* -- The context stub ----------------------------------------------------- */

/// Writes at \p at the stub that enters \p target, carrying \p context in the
/// register that holds a shared body's runtime generic context.
///
/// \p at needs context_stub_size bytes. It has to be able to reach \p target
/// with the arch's smallest jump. Every address in a domain's code arena can
/// reach every other, so a target out of range is a fatal error rather than a
/// wrong jump.
void write_context_stub (char *at, void *context, void *target);

/* -- Entering the interpreter --------------------------------------------- */

/// Plans how a call to a method is taken apart into the arguments the
/// interpreter wants.
///
/// \p shape is a declaration of the method in this backend's own convention,
/// and only its type is read. \p sig is the method's signature.
///
/// An error says this machine's entry cannot carry a call like that, and the
/// method has to be compiled rather than interpreted.
llvm::Expected<InterpEntryLayout> plan_interp_entry (llvm::Function *shape,
                                                     MonoMethodSignature *sig);

/// Returns the code an interpreted method's stub is pointed at, entered with
/// the MonoMethod * in a register the stub set up. It registers with the
/// runtime on first call, so a stack walk can cross it.
void *interp_entry_thunk ();

} // namespace mono::arch

#endif

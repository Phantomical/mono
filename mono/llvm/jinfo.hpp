/**
 * \file
 * \brief Publishing a compiled method's MonoJitInfo from its side tables.
 */

#ifndef MONO_LLVM_JINFO_HPP
#define MONO_LLVM_JINFO_HPP

#include "jit.hpp"
#include "method-to-llvm.hpp"

#include <llvm/Support/Error.h>

typedef struct _MonoDomain MonoDomain;
typedef struct _MonoMethod MonoMethod;
typedef struct _MonoMethodHeader MonoMethodHeader;
typedef struct _MonoJitInfo MonoJitInfo;
typedef struct _MonoLLVMBreakpointSwitch MonoLLVMBreakpointSwitch;

namespace mono {

/// What a registered code range is to the method it was compiled for, which is
/// what decides whether a stack walk reaching it has found a frame of the
/// method's.
enum class CodeKind {
	/// A translation of the method's IL: its body, one of its filter bodies,
	/// or the stand-in that raises what the metadata would not load.
	Body,
	/// A calling-convention adapter around the body - an entry thunk - holding
	/// none of the method's IL.
	AbiThunk,
};

/// Builds the MonoJitInfo for a piece of compiled code and registers it, so
/// the runtime's unwinder and stack walks can see the frame.
///
/// With debug info on, this also publishes the code's line table through
/// mono_debug_add_method (). The table carries the IL offset in effect at each
/// code offset, and where each argument and local lives. It is the only way a
/// stack frame below the top one gets an IL offset.
///
/// \param domain the domain whose linker holds the code. The record lives and
///        dies with that domain. It is therefore never the thread's current
///        domain, which mid-unload managed code runs with set elsewhere.
/// \param header the method's IL header, or null for code that has no clauses
///        of its own: an entry thunk, or a filter body.
/// \param compiled the compiled object. Its code member selects which of the
///        object's functions is registered. Code that shares a module with the
///        body it enters names its own range, and leaves the tables it has no
///        records in null.
/// \param kind whether that code stands for the method in a stack trace.
/// \param filters the entry of each IL clause's compiled filter body, which
///        the published clause hands to the runtime's search pass.
/// \param bp_switch the body's soft-debugger breakpoint switch.
/// \param seq_points the body's sequence-point graph. It and \p bp_switch are
///        present when the body was translated with sequence points in it.
///
/// \return the registered record, which mono_jit_info_table_remove () takes to
///         unregister it again.
llvm::Expected<MonoJitInfo *>
register_jit_info (MonoDomain *domain, MonoMethod *method,
                   MonoMethodHeader *header, const CompiledMethod &compiled,
                   CodeKind kind,
                   const std::vector<std::pair<uint32_t, void *>> &filters = {},
                   MonoLLVMBreakpointSwitch *bp_switch = nullptr,
                   const SeqPointGraph &seq_points = {});

} // namespace mono

#endif /* MONO_LLVM_JINFO_HPP */

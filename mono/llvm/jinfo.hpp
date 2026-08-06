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

/// Build METHOD's MonoJitInfo - unwind description, and the clause table where
/// the method has clauses - from COMPILED's side tables, and register it so the
/// runtime's unwinder and stack walks can see the frame. A null HEADER
/// registers clauseless code compiled for the method (its legacy entry).
///
/// COMPILED.code picks which of the object's functions is being registered,
/// so a forwarder sharing a module with the body it enters is registered by
/// naming its own code range and leaving the tables it has no records in null.
/// FILTERS maps an IL clause index to the entry of its compiled filter body,
/// which the published clause hands the runtime's search pass. BP_SWITCH is
/// the body's soft-debugger breakpoint switch and SEQ_POINTS its sequence-point
/// graph, both present when it was translated with sequence points in it.
/// DOMAIN is the domain whose linker holds the code: the record lives and dies
/// with it, so it is never the thread's current domain, which mid-unload managed
/// code - AppDomain:InvokeInDomain most visibly - runs with set elsewhere.
///
/// Where the runtime was started with debug info on, this also publishes the
/// code's line table through mono_debug_add_method (), which is the only way a
/// stack frame below the top one gets an IL offset, together with where each
/// argument and local lives when the translator pinned them to the frame.
///
/// Returns the registered record, which mono_jit_info_table_remove () takes to
/// unregister it again.
llvm::Expected<MonoJitInfo *>
register_jit_info (MonoDomain *domain, MonoMethod *method,
                   MonoMethodHeader *header, const CompiledMethod &compiled,
                   const std::vector<std::pair<uint32_t, void *>> &filters = {},
                   MonoLLVMBreakpointSwitch *bp_switch = nullptr,
                   const SeqPointGraph &seq_points = {});

} // namespace mono

#endif /* MONO_LLVM_JINFO_HPP */

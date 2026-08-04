/**
 * \file
 * \brief Publishing a compiled method's MonoJitInfo from its side tables.
 */

#ifndef MONO_LLVM_JINFO_HPP
#define MONO_LLVM_JINFO_HPP

#include "jit.hpp"

#include <llvm/Support/Error.h>

typedef struct _MonoDomain MonoDomain;
typedef struct _MonoMethod MonoMethod;
typedef struct _MonoMethodHeader MonoMethodHeader;
typedef struct _MonoJitInfo MonoJitInfo;

namespace mono {

/// Build METHOD's MonoJitInfo - unwind description, and the clause table where
/// the method has clauses - from COMPILED's side tables, and register it so the
/// runtime's unwinder and stack walks can see the frame. A null HEADER
/// registers clauseless code compiled for the method (its interop thunk).
/// FILTERS maps an IL clause index to the entry of its compiled filter body,
/// which the published clause hands the runtime's search pass. DOMAIN is the
/// domain whose linker holds the code: the record lives and dies with it, so
/// it is never the thread's current domain, which mid-unload managed code -
/// AppDomain:InvokeInDomain most visibly - runs with set elsewhere.
///
/// Returns the registered record, which mono_jit_info_table_remove () takes to
/// unregister it again.
llvm::Expected<MonoJitInfo *>
register_jit_info (MonoDomain *domain, MonoMethod *method,
                   MonoMethodHeader *header, const CompiledMethod &compiled,
                   const std::vector<std::pair<uint32_t, void *>> &filters = {});

} // namespace mono

#endif /* MONO_LLVM_JINFO_HPP */

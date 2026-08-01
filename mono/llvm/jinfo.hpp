/**
 * \file
 * \brief Publishing a compiled method's MonoJitInfo from its side tables.
 */

#ifndef MONO_LLVM_JINFO_HPP
#define MONO_LLVM_JINFO_HPP

#include "jit.hpp"

#include <llvm/Support/Error.h>

typedef struct _MonoMethod MonoMethod;
typedef struct _MonoMethodHeader MonoMethodHeader;

namespace mono {

/// Build METHOD's MonoJitInfo - unwind description, and the clause table where
/// the method has clauses - from COMPILED's side tables, and register it so the
/// runtime's unwinder and stack walks can see the frame.
llvm::Error register_jit_info (MonoMethod *method, MonoMethodHeader *header,
                               const CompiledMethod &compiled);

} // namespace mono

#endif /* MONO_LLVM_JINFO_HPP */

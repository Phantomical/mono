/**
 * \file
 * \brief The entries an interpreted method is reached through.
 *
 * The hand-written thunk every interpreted method shares is keyed only by the
 * MonoMethod. The thunk needs the argument layout for the prototype and the
 * interpreter's own handle for the method. Both therefore come from the
 * method's record, not from whichever engine compiled the caller.
 */

#ifndef MONO_LLVM_RUNTIME_INTERP_HPP
#define MONO_LLVM_RUNTIME_INTERP_HPP

#include "arch/arch.hpp"

#include <mono/mini/domain-method.hpp>

#include <llvm/Support/Error.h>

namespace mono {

/// Returns the entry \p dm is interpreted through, computed on first request.
llvm::Expected<arch::InterpEntryPoint> interp_entry (MonoDomainMethod &dm);

} // namespace mono

#endif

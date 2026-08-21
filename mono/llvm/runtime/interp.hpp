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

#include <llvm/IR/Function.h>
#include <llvm/Support/Error.h>

#include <string>

namespace mono {

/// Returns the entry \p dm is interpreted through, computed on first request.
llvm::Expected<arch::InterpEntryPoint> interp_entry (MonoDomainMethod &dm);

/// Names the part of a call's layout the LLVM prototype settles: the shapes of
/// the types, the calling convention, and the attributes that move a value.
///
/// A layout cannot be shared on this alone. Whether a parameter is byref, and
/// where the receiver stops, come from the signature and must be added to it.
///
/// A struct counts by its shape and not by its name, because one managed type
/// name can stand for two layouts in a process.
std::string prototype_key (llvm::Function *f);

} // namespace mono

#endif

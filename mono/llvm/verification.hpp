#ifndef MONO_LLVM_VERIFICATION_HPP
#define MONO_LLVM_VERIFICATION_HPP

#include "config.h"

#include <glib.h>

#include "mono/metadata/class-internals.h"

// This breaks some LLVM headers
#undef PIC

#include <llvm/Support/Error.h>

namespace mono {

/// Whether METHOD's body still has to go through the IL verifier.
///
/// False whenever verification was not asked for, which is the default, and
/// false once a method has been verified successfully: the answer is cached on
/// the metadata, so a method compiled twice is only ever verified once.
bool needs_verification (MonoMethod *method);

/// Put METHOD's body through the IL verifier, if verification was asked for and
/// this method has not already passed.
///
/// A verdict the verifier calls fatal comes back as the managed exception it
/// names - VerificationException, MethodAccessException and so on - carried as a
/// RuntimeError.
llvm::Error verify_method (MonoMethod *method);

} // namespace mono

#endif

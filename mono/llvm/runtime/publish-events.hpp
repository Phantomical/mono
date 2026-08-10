/**
 * \file
 * \brief Telling the rest of the process that a method now has code.
 */

#ifndef MONO_LLVM_RUNTIME_PUBLISH_EVENTS_HPP
#define MONO_LLVM_RUNTIME_PUBLISH_EVENTS_HPP

#include "jit.hpp"

typedef struct _MonoJitInfo MonoJitInfo;
typedef struct _MonoMethod MonoMethod;

namespace mono {

/// Raise the profiler's end of a method's compilation, and name its code where
/// a profile would otherwise show a bare address.
///
/// Called with no engine lock held: the soft debugger's handler for this parks
/// the compiling thread and lets its own thread look the method up, through a
/// door that takes that same lock.
void raise_jit_done (MonoMethod *method, MonoJitInfo *jinfo);

/// Hand every function a linked object defines to --jitdump.
void dump_object_code (MonoMethod *method, const CompiledMethod &compiled);

} // namespace mono

#endif

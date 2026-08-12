/**
 * \file
 * \brief Adding a compiled method to the perf jit dump.
 *
 * Where the backend's own types meet perf. The rest of this directory is written
 * against a name and a range of bytes and knows nothing of mono; this is the
 * translation, and a second thing the profiler wants to hear about belongs here
 * beside it rather than back in the engine.
 */

#ifndef MONO_LLVM_DEBUGGING_PERF_DUMP_METHOD_HPP
#define MONO_LLVM_DEBUGGING_PERF_DUMP_METHOD_HPP

#include "jit.hpp"

typedef struct _MonoMethod MonoMethod;

namespace mono::perf {

/// Describe the frame of every function a linked object defines, so a profile
/// can unwind out of it.
///
/// The object goes in as one record under the name of the function it starts
/// with, so a sample in a thunk or a linker stub prints the method it serves.
///
/// Does nothing unless a dump is open, so a caller needs no guard of its own.
void dump_method (MonoMethod *method, const CompiledMethod &compiled);

} // namespace mono::perf

#endif /* MONO_LLVM_DEBUGGING_PERF_DUMP_METHOD_HPP */

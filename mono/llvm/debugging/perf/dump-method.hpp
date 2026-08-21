/**
 * \file
 * \brief Adding a compiled method to the perf jit dump.
 *
 * Where the backend's own types meet perf. The rest of this directory is written
 * against a name and a range of bytes, and knows nothing of mono. This is the
 * translation: a second thing the profiler wants to hear about belongs here,
 * beside it, rather than back in the engine.
 */

#ifndef MONO_LLVM_DEBUGGING_PERF_DUMP_METHOD_HPP
#define MONO_LLVM_DEBUGGING_PERF_DUMP_METHOD_HPP

#include "jit.hpp"

typedef struct _MonoMethod MonoMethod;

namespace mono::perf {

/// Describe the frame of a compiled method's own functions, so a profile can
/// unwind out of them.
///
/// This publishes as one record, named for the function it starts with. So a
/// sample in a filter body or a linker stub prints under that name too.
///
/// Does nothing unless a dump is open, so a caller needs no guard of its own.
void dump_method (MonoMethod *method, const CompiledMethod &compiled);

} // namespace mono::perf

#endif /* MONO_LLVM_DEBUGGING_PERF_DUMP_METHOD_HPP */

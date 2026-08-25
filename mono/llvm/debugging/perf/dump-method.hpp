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

/// Name a compiled method's code in the dump, with the frame description that
/// lets a profile unwind out of it.
///
/// A record covers a run of the method's code that the object puts nothing else
/// between, so a method that shares an object with a batch claims its own bytes
/// and no more. It takes its name from the function it starts with, and a sample
/// in a filter body behind that function prints under the same name.
///
/// The linker's stubs go in under a name that says what they are. They belong to
/// the object, and the method that carries them is whichever one of a batch came
/// first.
///
/// Does nothing unless a dump is open, so a caller needs no guard of its own.
void dump_method (MonoMethod *method, const CompiledMethod &compiled);

} // namespace mono::perf

#endif /* MONO_LLVM_DEBUGGING_PERF_DUMP_METHOD_HPP */

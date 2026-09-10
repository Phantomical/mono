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
typedef struct _MonoJitInfo MonoJitInfo;

namespace mono::perf {

/// Name a body the classic compiler emitted in the dump, under the name the
/// same method's compiled tiers get, with the frame description that lets a
/// profile unwind out of it and the IL offset in effect at each address.
///
/// Does nothing unless a dump is open, so a caller needs no guard of its own.
void dump_method (MonoMethod *method, MonoJitInfo *jinfo);

/// Name a compiled method's code in the dump, with the frame description that
/// lets a profile unwind out of it.
///
/// One record for each function the object defines for the method, under that
/// function's own name, and one for each section of linker stubs the method
/// carries, under "linker stubs".
///
/// Does nothing unless a dump is open, so a caller needs no guard of its own.
void dump_method (MonoMethod *method, const CompiledMethod &compiled);

} // namespace mono::perf

#endif /* MONO_LLVM_DEBUGGING_PERF_DUMP_METHOD_HPP */

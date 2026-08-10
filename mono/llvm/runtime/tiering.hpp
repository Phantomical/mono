/**
 * \file
 * \brief What may be compiled somewhere the program is not waiting.
 */

#ifndef MONO_LLVM_RUNTIME_TIERING_HPP
#define MONO_LLVM_RUNTIME_TIERING_HPP

typedef struct _MonoMethod MonoMethod;

namespace mono {

/// Whether a method may be compiled on the background worker.
bool compilable_off_thread (MonoMethod *method);

} // namespace mono

#endif

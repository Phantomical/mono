/**
 * \file
 * \brief The jit-info record that resolves a stub's address back to its method.
 */

#ifndef MONO_LLVM_RUNTIME_STUB_JINFO_HPP
#define MONO_LLVM_RUNTIME_STUB_JINFO_HPP

#include <cstddef>
#include <string>

typedef struct _MonoDomain MonoDomain;
typedef struct _MonoJitInfo MonoJitInfo;
typedef struct _MonoMethod MonoMethod;

namespace mono {

/// Register the jit-info record that resolves the given bytes of stub code back
/// to the method they were published for, under the symbol it was published as.
///
/// Without one, nothing recovers a method from a stub address: an escaped
/// function pointer does not become a delegate, and a stack walk that lands in
/// the sixteen bytes in front of a body finds no frame.
///
/// Returns the record when the caller has to take it out again, and null when it
/// dies with its domain. A dynamic method's stub block goes back on the free list
/// when the method is freed, so its record has to be removed before the next
/// method lands there; every other method lives exactly as long as its domain,
/// and so does its record.
MonoJitInfo *register_stub_jinfo (MonoDomain *domain, MonoMethod *method, void *stub,
                                  size_t size, const std::string &name);

} // namespace mono

#endif

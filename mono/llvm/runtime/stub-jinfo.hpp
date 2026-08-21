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

/// Registers the jit-info record that resolves this stub's code back to the
/// method it was published for, under the symbol it was published as.
///
/// Without one, a stack walk cannot cross the stub, and a function pointer
/// into it cannot become a delegate.
///
/// Returns the record for a dynamic method's stub, and null otherwise. A
/// returned record has to reach mono_jit_info_table_remove () before the
/// code is freed. A freed dynamic method's MonoMethod is recycled, and a
/// leftover record can misattribute whatever reuses its address next.
MonoJitInfo *register_stub_jinfo (MonoDomain *domain, MonoMethod *method, void *stub,
                                  size_t size, const std::string &name);

} // namespace mono

#endif

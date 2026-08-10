/**
 * \file
 * \brief What runtime.cpp answers for while it is still one of two engines.
 *
 * Temporary, and deleted with runtime.cpp. Its Backend lives in an anonymous
 * namespace and is staying there: exposing the class so another translation unit
 * could name it would mean publishing every private member it has, all of which
 * are about to be replaced. Free functions cost one line each and die with it.
 */

#ifndef MONO_LLVM_RUNTIME_LEGACY_HPP
#define MONO_LLVM_RUNTIME_LEGACY_HPP

#include <llvm/Support/Error.h>

typedef struct _MonoDomain MonoDomain;
typedef struct _MonoJitInfo MonoJitInfo;
typedef struct _MonoMethod MonoMethod;

namespace mono::legacy {

llvm::Expected<void *> compile (MonoMethod *method, MonoDomain *target_domain);
void stop_compiling ();
void stop_compiling_for (MonoDomain *domain);
void free_domain (MonoDomain *domain);
void free_method (MonoMethod *method);
void *body_of (MonoDomain *domain, MonoMethod *method);
void foreach_body (MonoDomain *domain, MonoMethod *method,
                   void (*visit) (MonoJitInfo *, void *), void *user_data);
void *unbox_entry_of (MonoMethod *method);

} // namespace mono::legacy

#endif

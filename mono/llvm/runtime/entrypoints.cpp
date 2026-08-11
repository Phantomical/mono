/**
 * \file
 * \brief The whole surface mono's C runtime sees of the LLVM backend.
 */

/*
 * Before runtime.hpp, so that MonoError is the internal struct the rest of the
 * runtime passes around rather than the opaque public one.
 */
#include "runtime-error.hpp"

#include "runtime.hpp"

#include "backend.hpp"
#include "jit.hpp"
#include "options.hpp"

#include <llvm/Support/Error.h>

namespace {

/// Hand a compile's result back the way the runtime expects it.
///
/// A refusal the translator raised through a MonoError is handed back as the
/// exception it described; anything else is the engine itself failing, which
/// managed code sees as an ExecutionEngineException all the same.
void *
finish (llvm::Expected<void *> code, MonoError *error)
{
	if (code)
		return *code;

	bool recovered = false;

	llvm::handleAllErrors (
		code.takeError (),
		[&] (mono::RuntimeError &runtime) {
			runtime.move_to (error);
			recovered = true;
		},
		[&] (const llvm::ErrorInfoBase &other) {
			mono_error_set_execution_engine (error, "%s", other.message ().c_str ());
			recovered = true;
		});

	g_assert (recovered);
	return NULL;
}

} // namespace

void *
mono_llvm_jit_compile_method (MonoMethod *method, MonoDomain *target_domain,
                              MonoError *error)
{
	error_init (error);

	llvm::Expected<mono::MonoBackend *> backend = mono::MonoBackend::get ();

	if (!backend)
		return finish (backend.takeError (), error);
	return finish ((*backend)->compile (method, target_domain), error);
}

void
mono_llvm_jit_stop_compiling (void)
{
	mono::MonoBackend::stop_compilation ();
}

void
mono_llvm_jit_stop_compiling_for_domain (MonoDomain *domain)
{
	mono::MonoBackend::stop_compilation (domain);
}

void
mono_llvm_jit_free_domain (MonoDomain *domain)
{
	mono::MonoBackend::release_domain (domain);
}

void
mono_llvm_jit_free_method (MonoMethod *method)
{
	mono::MonoBackend::release_method (method);
}

void *
mono_llvm_jit_find_body (MonoDomain *domain, MonoMethod *method)
{
	return mono::MonoBackend::body_of (domain, method);
}

void
mono_llvm_jit_foreach_body (MonoDomain *domain, MonoMethod *method,
                            void (*visit) (MonoJitInfo *, void *), void *user_data)
{
	mono::MonoBackend::foreach_body (domain, method, visit, user_data);
}

void *
mono_llvm_jit_unbox_entry (MonoMethod *method)
{
	return mono::MonoBackend::unbox_entry_of (method);
}

void
mono_llvm_jit_add_option (const char *opt)
{
	mono::MonoJit::add_option (opt);
}

mono_bool
mono_llvm_jit_tier0_enabled (void)
{
	return mono::tier0_enabled ();
}

int32_t
mono_llvm_jit_tier0_calls (MonoMethod *method)
{
	if (!mono::runs_at_tier0 (method))
		return 0;

	return (int32_t) mono::tier1_threshold ();
}

void
mono_llvm_jit_request_promotion (MonoMethod *method, MonoDomain *domain)
{
	mono::MonoBackend::request_promotion (method, domain);
}

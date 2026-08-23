/**
 * \file
 * \brief The whole surface mono's C runtime sees of the LLVM backend.
 */

/*
 * Before runtime.h, so that MonoError is the internal struct the rest of the
 * runtime passes around rather than the opaque public one.
 */
#include "runtime-error.hpp"

#include "runtime.h"

#include "backend.hpp"
#include "jit.hpp"
#include "options.hpp"
#include "verification.hpp"

#include <llvm/Support/Error.h>
#include <llvm/Support/raw_ostream.h>

namespace {

/// Sets the error from a refusal the way the runtime expects it.
///
/// A refusal raised through a MonoError becomes the exception it described.
/// Anything else is the engine itself failing, which managed code sees as an
/// ExecutionEngineException all the same.
void
report (llvm::Error failure, MonoError *error)
{
	bool recovered = false;

	llvm::handleAllErrors (
		std::move (failure),
		[&] (mono::RuntimeError &runtime) {
			runtime.move_to (error);
			recovered = true;
		},
		[&] (const llvm::ErrorInfoBase &other) {
			mono_error_set_execution_engine (error, "%s", other.message ().c_str ());
			recovered = true;
		});

	g_assert (recovered);
}

void *
finish (llvm::Expected<void *> code, MonoError *error)
{
	if (code)
		return *code;

	report (code.takeError (), error);
	return NULL;
}

} // namespace

void
mono_llvm_jit_init (void)
{
	llvm::Expected<mono::MonoBackend *> backend = mono::MonoBackend::get ();

	/* The first compile asks again and reports this through its MonoError. */
	if (!backend)
		llvm::logAllUnhandledErrors (backend.takeError (), llvm::errs (),
		                             "mono: could not start the LLVM backend: ");
}

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

void *
mono_llvm_jit_stub_for (MonoMethod *method, MonoDomain *target_domain, MonoError *error)
{
	error_init (error);

	llvm::Expected<mono::MonoBackend *> backend = mono::MonoBackend::get ();

	if (!backend)
		return finish (backend.takeError (), error);
	return finish ((*backend)->stub_for (method, target_domain), error);
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
	return mono::tier0_calls (method);
}

mono_bool
mono_llvm_jit_request_promotion (MonoMethod *method, MonoDomain *domain, uint8_t tier)
{
	return mono::MonoBackend::request_promotion (method, domain, (mono::MonoTier) tier);
}

mono_bool
mono_llvm_jit_promote_now (MonoMethod *method, MonoDomain *domain, uint8_t tier)
{
	return mono::MonoBackend::promote_now (method, domain, (mono::MonoTier) tier);
}


mono_bool
mono_llvm_jit_verify_method (MonoMethod *method, MonoError *error)
{
	error_init (error);

	llvm::Error invalid = mono::verify_method (method);

	if (!invalid)
		return TRUE;

	report (std::move (invalid), error);
	return FALSE;
}

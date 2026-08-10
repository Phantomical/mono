/**
 * \file
 * \brief The whole surface mono's C runtime sees of the LLVM backend.
 *
 * Each entry point picks an engine and forwards. That branch is temporary; when
 * runtime.cpp goes, so does every mention of an engine here and these become
 * plain forwarders.
 */

/*
 * Before runtime.hpp, so that MonoError is the internal struct the rest of the
 * runtime passes around rather than the opaque public one.
 */
#include "runtime-error.hpp"

#include "runtime.hpp"

#include "backend.hpp"
#include "engine.hpp"
#include "jit.hpp"
#include "options.hpp"
#include "runtime-legacy.hpp"

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

bool
on_backend ()
{
	return mono::selected_engine () == mono::EngineKind::backend;
}

} // namespace

void *
mono_llvm_jit_compile_method (MonoMethod *method, MonoDomain *target_domain,
                              MonoError *error)
{
	error_init (error);

	if (on_backend ()) {
		llvm::Expected<mono::MonoBackend *> backend = mono::MonoBackend::get ();

		if (!backend)
			return finish (backend.takeError (), error);
		return finish ((*backend)->compile (method, target_domain), error);
	}

	return finish (mono::legacy::compile (method, target_domain), error);
}

void
mono_llvm_jit_stop_compiling (void)
{
	if (on_backend ())
		mono::MonoBackend::stop_compilation ();
	else
		mono::legacy::stop_compiling ();
}

void
mono_llvm_jit_stop_compiling_for_domain (MonoDomain *domain)
{
	if (on_backend ())
		mono::MonoBackend::stop_compilation (domain);
	else
		mono::legacy::stop_compiling_for (domain);
}

void
mono_llvm_jit_free_domain (MonoDomain *domain)
{
	if (on_backend ())
		mono::MonoBackend::release_domain (domain);
	else
		mono::legacy::free_domain (domain);
}

void
mono_llvm_jit_free_method (MonoMethod *method)
{
	if (on_backend ())
		mono::MonoBackend::release_method (method);
	else
		mono::legacy::free_method (method);
}

void *
mono_llvm_jit_find_body (MonoDomain *domain, MonoMethod *method)
{
	if (on_backend ())
		return mono::MonoBackend::body_of (domain, method);
	return mono::legacy::body_of (domain, method);
}

void
mono_llvm_jit_foreach_body (MonoDomain *domain, MonoMethod *method,
                            void (*visit) (MonoJitInfo *, void *), void *user_data)
{
	if (on_backend ())
		mono::MonoBackend::foreach_body (domain, method, visit, user_data);
	else
		mono::legacy::foreach_body (domain, method, visit, user_data);
}

void *
mono_llvm_jit_unbox_entry (MonoMethod *method)
{
	if (on_backend ())
		return mono::MonoBackend::unbox_entry_of (method);
	return mono::legacy::unbox_entry_of (method);
}

void
mono_llvm_jit_add_option (const char *opt)
{
	mono::MonoJit::add_option (opt);
}

void
mono_llvm_jit_interpret_methods (const char *filter)
{
	mono::set_interp_filter (filter);
}

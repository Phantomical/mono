/**
 * \file
 * \brief Putting a method's IL through the verifier before it is compiled.
 *
 * This file's checks run only when `--security=validil`, `--security=verifiable`
 * or `--verify-all` set a verifier mode. `mono_verifier_is_enabled_for_method ()`
 * returns false otherwise. A compile still pays a field read and one
 * image-property lookup, which takes the image lock.
 */

#include "runtime-error.hpp"

#include "verification.hpp"

#include "mono/metadata/debug-helpers.h"
#include "mono/metadata/metadata-internals.h"
#include "mono/metadata/object-internals.h"
#include "mono/metadata/verify.h"
#include "mono/mini/mini.h"
#include "mono/utils/mono-error-internals.h"

extern "C" {
#include "mono/metadata/verify-internals.h"
}

namespace mono {

namespace {

/// The definition whose body the verifier checks, in place of a generic
/// instance.
///
/// A generic instance shares its IL with the definition it was inflated from,
/// so verifying the definition once covers every instance.
MonoMethod *
verification_subject (MonoMethod *method)
{
	while (method->is_inflated)
		method = ((MonoMethodInflated *) method)->declaring;
	return method;
}

void
set_verification_error (MonoError *error, int exception_type, const char *message)
{
	switch (exception_type) {
	case MONO_EXCEPTION_METHOD_ACCESS:
		mono_error_set_generic_error (error, "System", "MethodAccessException",
		                              "%s", message);
		break;
	case MONO_EXCEPTION_FIELD_ACCESS:
		mono_error_set_generic_error (error, "System", "FieldAccessException",
		                              "%s", message);
		break;
	case MONO_EXCEPTION_UNVERIFIABLE_IL:
		mono_error_set_generic_error (error, "System.Security",
		                              "VerificationException", "%s", message);
		break;
	case MONO_EXCEPTION_TYPE_LOAD:
		mono_error_set_generic_error (error, "System", "TypeLoadException",
		                              "%s", message);
		break;
	case MONO_EXCEPTION_BAD_IMAGE:
		mono_error_set_generic_error (error, "System", "BadImageFormatException",
		                              "%s", message);
		break;
	default:
		mono_error_set_generic_error (error, "System", "InvalidProgramException",
		                              "%s", message);
		break;
	}
}

bool
wants_verifying (MonoMethod *method)
{
	if (m_class_get_image (method->klass)->assembly->corlib_internal)
		return false;
	if (mono_method_get_verification_success (method))
		return false;
	return mono_verifier_is_enabled_for_method (method) != FALSE;
}

} // namespace

bool
needs_verification (MonoMethod *method)
{
	return wants_verifying (verification_subject (method));
}

llvm::Error
verify_method (MonoMethod *method)
{
	MonoMethod *subject = verification_subject (method);

	if (!wants_verifying (subject))
		return llvm::Error::success ();

	/*
	 * Full trust decides what an unverifiable-but-valid body means: trusted
	 * code is allowed to be unverifiable, everything else is not. The access
	 * verdicts below are the exception - those bind whatever the trust level.
	 */
	bool full_trust = mono_verifier_is_method_full_trust (subject) != FALSE
	                  || mini_assembly_can_skip_verification (subject) != FALSE;

	/*
	 * skip_visibility on the method itself, which is what a DynamicMethod
	 * constructed with skipVisibility carries and what the translator's own
	 * access checks read.
	 */
	GSList *found = mono_method_verify_with_current_settings (
		subject, subject->skip_visibility, full_trust);

	for (GSList *entry = found; entry != nullptr; entry = entry->next) {
		MonoVerifyInfoExtended *info = (MonoVerifyInfoExtended *) entry->data;
		bool fatal =
			info->info.status == MONO_VERIFY_ERROR
			|| (info->info.status == MONO_VERIFY_NOT_VERIFIABLE
			    && (!full_trust
			        || info->exception_type == MONO_EXCEPTION_METHOD_ACCESS
			        || info->exception_type == MONO_EXCEPTION_FIELD_ACCESS));

		if (!fatal)
			continue;

		ERROR_DECL (failure);
		char *name = mono_method_full_name (subject, TRUE);
		char *message =
			g_strdup_printf ("Error verifying %s: %s", name, info->info.message);

		set_verification_error (failure, info->exception_type, message);
		g_free (message);
		g_free (name);
		mono_free_verify_list (found);
		return runtime_error (failure);
	}

	mono_free_verify_list (found);
	mono_method_set_verification_success (subject);
	return llvm::Error::success ();
}

} // namespace mono

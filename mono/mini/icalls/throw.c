/**
 * \file
 * The exceptions a body throws in place of code the runtime refused to emit.
 *
 * Licensed under the MIT license. See LICENSE file in the project root for full license information.
 */
#include "icalls/icalls.h"

void
mono_throw_method_access (MonoMethod *caller, MonoMethod *callee)
{
	char *caller_name = mono_method_get_reflection_name (caller);
	char *callee_name = mono_method_get_reflection_name (callee);
	ERROR_DECL (error);

	mono_error_set_generic_error (error, "System", "MethodAccessException", "Method `%s' is inaccessible from method `%s'", callee_name, caller_name);
	mono_error_set_pending_exception (error);
	g_free (callee_name);
	g_free (caller_name);
}

void
mono_throw_bad_image ()
{
	ERROR_DECL (error);
	mono_error_set_generic_error (error, "System", "BadImageFormatException", "Bad IL format.");
	mono_error_set_pending_exception (error);
}

void
mono_throw_not_supported ()
{
	ERROR_DECL (error);
	mono_error_set_generic_error (error, "System", "NotSupportedException", "");
	mono_error_set_pending_exception (error);
}

void
mono_throw_invalid_program (const char *msg)
{
	ERROR_DECL (error);
	mono_error_set_invalid_program (error, "Invalid IL due to: %s", msg);
	mono_error_set_pending_exception (error);
}

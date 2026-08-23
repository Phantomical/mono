/**
 * \file
 * A corlib exception object, built from its type-def index and message arguments.
 *
 * Licensed under the MIT license. See LICENSE file in the project root for full license information.
 */
#include "icalls/icalls.h"

MonoException *
mono_create_corlib_exception_0 (guint32 token)
{
	return mono_exception_from_token (mono_defaults.corlib, token);
}

MonoException *
mono_create_corlib_exception_1 (guint32 token, MonoString *arg_raw)
{
	HANDLE_FUNCTION_ENTER ();
	ERROR_DECL (error);
	MONO_HANDLE_DCL (MonoString, arg);
	MonoExceptionHandle ret = mono_exception_from_token_two_strings_checked (
		mono_defaults.corlib, token, arg, NULL_HANDLE_STRING, error);
	mono_error_set_pending_exception (error);
	HANDLE_FUNCTION_RETURN_OBJ (ret);
}

MonoException *
mono_create_corlib_exception_2 (guint32 token, MonoString *arg1_raw, MonoString *arg2_raw)
{
	HANDLE_FUNCTION_ENTER ();
	ERROR_DECL (error);
	MONO_HANDLE_DCL (MonoString, arg1);
	MONO_HANDLE_DCL (MonoString, arg2);
	MonoExceptionHandle ret = mono_exception_from_token_two_strings_checked (
		mono_defaults.corlib, token, arg1, arg2, error);
	mono_error_set_pending_exception (error);
	HANDLE_FUNCTION_RETURN_OBJ (ret);
}

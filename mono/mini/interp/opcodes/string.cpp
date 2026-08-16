#include "mintops.h"
#include "mono/metadata/class-internals.h"
#include "mono/metadata/exception.h"
#include "mono/metadata/icall-decl.h"
#include "mono/metadata/object-internals.h"
#include "mono/mini/interp/interp.hpp"

namespace mono::interp {

MONO_INTERP_OP_IMPL (MINT_LDSTR)
{
	LOCAL_VAR (ip[1], gpointer) = frame->imethod->data_items[ip[2]];

	MONO_INTERP_OP_ADVANCE ();
	MONO_INTERP_DISPATCH ();
}

MONO_INTERP_OP_IMPL (MINT_LDSTR_TOKEN)
{
	auto strtoken = (guint32) (gsize) frame->imethod->data_items[ip[2]];
	MonoMethod *method = frame->imethod->method;

	MonoString *s = NULL;
	if (method->wrapper_type == MONO_WRAPPER_DYNAMIC_METHOD) {
		s = (MonoString *) mono_method_get_wrapper_data (method, strtoken);
	} else if (method->wrapper_type != MONO_WRAPPER_NONE) {
		s = mono_string_new_wrapper_internal (
			(const char *) mono_method_get_wrapper_data (method, strtoken));
	} else {
		g_assert_not_reached ();
	}
	LOCAL_VAR (ip[1], gpointer) = s;

	MONO_INTERP_OP_ADVANCE ();
	MONO_INTERP_DISPATCH ();
}

MONO_INTERP_OP_IMPL (MINT_GETCHR)
{
	auto s = LOCAL_VAR (ip[2], MonoString *);
	NULL_CHECK (s);

	gint32 index = LOCAL_VAR (ip[3], gint32);
	if (G_UNLIKELY ((guint32) index >= (guint32) mono_string_length_internal (s)))
		THROW_EX (mono_get_exception_index_out_of_range (), ip);

	LOCAL_VAR (ip[1], gint32) = mono_string_chars_internal (s)[index];

	MONO_INTERP_OP_ADVANCE ();
	MONO_INTERP_DISPATCH ();
}

MONO_INTERP_OP_IMPL (MINT_STRLEN)
{
	auto s = LOCAL_VAR (ip[2], MonoString *);
	NULL_CHECK (s);
	LOCAL_VAR (ip[1], gint32) = mono_string_length_internal (s);

	MONO_INTERP_OP_ADVANCE ();
	MONO_INTERP_DISPATCH ();
}

} // namespace mono::interp

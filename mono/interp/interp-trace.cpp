#include "config.h"

/**
 * \file
 * \brief Printing a method's frames and opcodes as it executes.
 */

#include "interp-internals.h"
#include "interp-trace.hpp"
#include "interp.hpp"

#include <mono/metadata/class-internals.h>
#include <mono/metadata/debug-helpers.h>
#include <mono/metadata/object-internals.h>
#include <mono/utils/mono-threads.h>

#ifdef ENABLE_INTERP_TRACE

namespace mono::interp {

static void
dump_stackval (GString *str, stackval *s, MonoType *type)
{
	switch (type->type) {
	case MONO_TYPE_I1:
	case MONO_TYPE_U1:
	case MONO_TYPE_I2:
	case MONO_TYPE_U2:
	case MONO_TYPE_I4:
	case MONO_TYPE_U4:
	case MONO_TYPE_CHAR:
	case MONO_TYPE_BOOLEAN:
		g_string_append_printf (str, "[%d] ", s->data.i);
		break;
	case MONO_TYPE_STRING:
	case MONO_TYPE_SZARRAY:
	case MONO_TYPE_CLASS:
	case MONO_TYPE_OBJECT:
	case MONO_TYPE_ARRAY:
	case MONO_TYPE_PTR:
	case MONO_TYPE_FNPTR:
	case MONO_TYPE_I:
	case MONO_TYPE_U:
		g_string_append_printf (str, "[%p] ", s->data.p);
		break;
	case MONO_TYPE_VALUETYPE:
		if (m_class_is_enumtype (type->data.klass))
			g_string_append_printf (str, "[%d] ", s->data.i);
		else
			g_string_append_printf (str, "[vt:%p] ", s->data.p);
		break;
	case MONO_TYPE_R4:
		g_string_append_printf (str, "[%g] ", s->data.f_r4);
		break;
	case MONO_TYPE_R8:
		g_string_append_printf (str, "[%g] ", s->data.f);
		break;
	case MONO_TYPE_I8:
	case MONO_TYPE_U8:
	default: {
		GString *res = g_string_new ("");
		mono_type_get_desc (res, type, TRUE);
		g_string_append_printf (str, "[{%s} %" PRId64 "/0x%0" PRIx64 "] ", res->str, (gint64)s->data.l, (guint64)s->data.l);
		g_string_free (res, TRUE);
		break;
	}
	}
}

static char*
dump_retval (InterpFrame *inv)
{
	GString *str = g_string_new ("");
	MonoType *ret = mono_method_signature_internal (inv->imethod->method)->ret;

	if (ret->type != MONO_TYPE_VOID)
		dump_stackval (str, inv->stack, ret);

	return g_string_free (str, FALSE);
}

static char*
dump_args (InterpFrame *inv)
{
	GString *str = g_string_new ("");
	int i;
	MonoMethodSignature *signature = mono_method_signature_internal (inv->imethod->method);
	
	if (signature->param_count == 0 && !signature->hasthis)
		return g_string_free (str, FALSE);

	if (signature->hasthis) {
		MonoMethod *method = inv->imethod->method;
		dump_stackval (str, inv->stack, m_class_get_byval_arg (method->klass));
	}

	for (i = 0; i < signature->param_count; ++i)
		dump_stackval (str, inv->stack + (!!signature->hasthis) + i, signature->params [i]);

	return g_string_free (str, FALSE);
}

static void
print_indent (ThreadContext *context)
{
	for (int i = 0; i < context->trace_depth; i++)
		g_print ("  ");
}

gboolean
trace_wants_method (MonoMethod *method)
{
	static const char *pattern;
	static gboolean asked;

	if (!asked) {
		pattern = g_getenv ("MONO_INTERP_TRACE");
		asked = TRUE;
	}

	if (!pattern)
		return FALSE;

	char *name = mono_method_full_name (method, TRUE);
	gboolean wanted = strstr (name, pattern) != NULL;
	g_free (name);
	return wanted;
}

void
trace_enter (ThreadContext *context, InterpFrame *frame)
{
	char *name = mono_method_full_name (frame->imethod->method, FALSE);
	char *args = dump_args (frame);

	print_indent (context);
	g_print ("(%p) enter %s (%s)\n", mono_thread_internal_current (), name, args);
	context->trace_depth++;

	g_free (args);
	g_free (name);
}

void
trace_leave (ThreadContext *context, InterpFrame *frame)
{
	char *name = mono_method_full_name (frame->imethod->method, FALSE);
	char *retval = dump_retval (frame);

	if (context->trace_depth > 0)
		context->trace_depth--;
	print_indent (context);
	g_print ("(%p) leave %s => %s\n", mono_thread_internal_current (), name, retval);

	g_free (retval);
	g_free (name);
}

MONO_NEVER_INLINE void
InterpState::trace_op ()
{
	print_indent (context);
	mono_interp_dis_mintop (ip, frame->imethod->code);
}

} // namespace mono::interp

#endif /* ENABLE_INTERP_TRACE */

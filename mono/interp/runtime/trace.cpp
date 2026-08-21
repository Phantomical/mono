#include "config.h"

/**
 * \file
 * \brief Printing a method's frames and opcodes as it executes.
 */

#include "internals.hpp"
#include "trace.hpp"
#include "interp.hpp"

#include <mono/metadata/class-internals.h>
#include <mono/metadata/debug-helpers.h>
#include <mono/metadata/object-internals.h>
#include <mono/utils/mono-threads.h>

#include <cstdarg>
#include <cstdio>
#include <string>

#ifdef ENABLE_INTERP_TRACE

namespace mono::interp {

namespace {

/// Appends a printf-formatted piece. Every caller formats one stack slot, so a
/// fixed 128-byte buffer covers it. Longer output is truncated.
void
append_format (std::string &out, const char *fmt, ...)
{
	char buf[128];
	va_list args;

	va_start (args, fmt);
	vsnprintf (buf, sizeof (buf), fmt, args);
	va_end (args);

	out += buf;
}

/// Copies mono_method_full_name ()'s result into an owned string and frees
/// the buffer it returned.
std::string
full_name (MonoMethod *method, gboolean with_signature)
{
	char *name = mono_method_full_name (method, with_signature);
	std::string owned = name;

	g_free (name);
	return owned;
}

void
dump_stackval (std::string &out, stackval *s, MonoType *type)
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
		append_format (out, "[%d] ", s->data.i);
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
		append_format (out, "[%p] ", s->data.p);
		break;
	case MONO_TYPE_VALUETYPE:
		if (m_class_is_enumtype (type->data.klass))
			append_format (out, "[%d] ", s->data.i);
		else
			append_format (out, "[vt:%p] ", s->data.p);
		break;
	case MONO_TYPE_R4:
		append_format (out, "[%g] ", s->data.f_r4);
		break;
	case MONO_TYPE_R8:
		append_format (out, "[%g] ", s->data.f);
		break;
	case MONO_TYPE_I8:
	case MONO_TYPE_U8:
	default: {
		// mono_type_get_desc () writes into a GString, and what it writes has no
		// length this side can assume, so it is appended rather than formatted.
		GString *desc = g_string_new ("");

		mono_type_get_desc (desc, type, TRUE);
		out += "[{";
		out += desc->str;
		out += "} ";
		g_string_free (desc, TRUE);

		append_format (out, "%" PRId64 "/0x%0" PRIx64 "] ", (gint64) s->data.l,
		               (guint64) s->data.l);
		break;
	}
	}
}

std::string
dump_retval (InterpFrame *inv)
{
	std::string out;
	MonoType *ret = mono_method_signature_internal (inv->imethod->method)->ret;

	if (ret->type != MONO_TYPE_VOID)
		dump_stackval (out, inv->stack, ret);

	return out;
}

std::string
dump_args (InterpFrame *inv)
{
	std::string out;
	MonoMethodSignature *signature = mono_method_signature_internal (inv->imethod->method);

	if (signature->hasthis) {
		MonoMethod *method = inv->imethod->method;
		dump_stackval (out, inv->stack, m_class_get_byval_arg (method->klass));
	}

	for (int i = 0; i < signature->param_count; ++i)
		dump_stackval (out, inv->stack + (!!signature->hasthis) + i, signature->params[i]);

	return out;
}

void
print_indent (ThreadContext *context)
{
	for (int i = 0; i < context->trace_depth; i++)
		g_print ("  ");
}

} // namespace

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

	return full_name (method, TRUE).find (pattern) != std::string::npos;
}

void
trace_enter (ThreadContext *context, InterpFrame *frame)
{
	std::string name = full_name (frame->imethod->method, FALSE);
	std::string args = dump_args (frame);

	print_indent (context);
	g_print ("(%p) enter %s (%s)\n", mono_thread_internal_current (), name.c_str (), args.c_str ());
	context->trace_depth++;
}

void
trace_leave (ThreadContext *context, InterpFrame *frame)
{
	std::string name = full_name (frame->imethod->method, FALSE);
	std::string retval = dump_retval (frame);

	if (context->trace_depth > 0)
		context->trace_depth--;
	print_indent (context);
	g_print ("(%p) leave %s => %s\n", mono_thread_internal_current (), name.c_str (),
	         retval.c_str ());
}

MONO_NEVER_INLINE void
InterpState::trace_op ()
{
	print_indent (context);
	mono_interp_dis_mintop (ip, frame->imethod->code);
}

} // namespace mono::interp

#endif /* ENABLE_INTERP_TRACE */

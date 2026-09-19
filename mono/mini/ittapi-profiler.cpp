/*
 * ittapi-profiler.cpp: reports JIT-compiled methods and code buffers to Intel
 * VTune through the ITT API. Enable it with MONO_PROFILE=ittapi.
 */

#include <config.h>

#include <mono/metadata/class-internals.h>
#include <mono/metadata/debug-helpers.h>
#include <mono/metadata/debug-internals.h>
#include <mono/metadata/loader.h>
#include <mono/metadata/mono-debug.h>
#include <mono/metadata/profiler.h>

#include <glib.h>

#include <cstring>

#include <jitprofiling.h>

namespace {

const char *
code_buffer_desc (MonoProfilerCodeBufferType type)
{
	switch (type) {
	case MONO_PROFILER_CODE_BUFFER_METHOD:
		return "code_buffer_method";
	case MONO_PROFILER_CODE_BUFFER_METHOD_TRAMPOLINE:
		return "code_buffer_method_trampoline";
	case MONO_PROFILER_CODE_BUFFER_UNBOX_TRAMPOLINE:
		return "code_buffer_unbox_trampoline";
	case MONO_PROFILER_CODE_BUFFER_IMT_TRAMPOLINE:
		return "code_buffer_imt_trampoline";
	case MONO_PROFILER_CODE_BUFFER_GENERICS_TRAMPOLINE:
		return "code_buffer_generics_trampoline";
	case MONO_PROFILER_CODE_BUFFER_SPECIFIC_TRAMPOLINE:
		return "code_buffer_specific_trampoline";
	case MONO_PROFILER_CODE_BUFFER_HELPER:
		return "code_buffer_misc_helper";
	case MONO_PROFILER_CODE_BUFFER_MONITOR:
		return "code_buffer_monitor";
	case MONO_PROFILER_CODE_BUFFER_DELEGATE_INVOKE:
		return "code_buffer_delegate_invoke";
	case MONO_PROFILER_CODE_BUFFER_EXCEPTION_HANDLING:
		return "code_buffer_exception_handling";
	default:
		return "unspecified";
	}
}

void
runtime_shutdown_end (MonoProfiler *prof)
{
	iJIT_NotifyEvent (iJVM_EVENT_TYPE_SHUTDOWN, NULL);
}

void
method_jit_done (MonoProfiler *prof, MonoMethod *method, MonoJitInfo *jinfo)
{
	/*
	 * Managed-to-native wrappers raise jit_done for both the wrapped method and
	 * the wrapper. Report only the method described by this JIT info.
	 */
	if (mono_jit_info_get_method (jinfo) != method)
		return;

	MonoClass *klass = mono_method_get_class (method);
	char *signature = mono_signature_get_desc (mono_method_signature_internal (method), TRUE);
	char *name = g_strdup_printf ("%s(%s)", mono_method_get_name (method), signature);
	char *classname = g_strdup_printf ("%s%s%s", m_class_get_name_space (klass),
	                                    m_class_get_name_space (klass) [0] != 0 ? "::" : "",
	                                    m_class_get_name (klass));

	iJIT_Method_Load vtune_method;
	memset (&vtune_method, 0, sizeof (vtune_method));
	vtune_method.method_id = iJIT_GetNewMethodID ();
	vtune_method.method_name = name;
	vtune_method.method_load_address = mono_jit_info_get_code_start (jinfo);
	vtune_method.method_size = (unsigned int) mono_jit_info_get_code_size (jinfo);
	vtune_method.class_file_name = classname;

	MonoDebugMethodJitInfo *dmji = mono_debug_find_method (method, mono_domain_get ());
	if (dmji != NULL) {
		LineNumberInfo *lines = dmji->num_line_numbers != 0
			? (LineNumberInfo *) g_malloc (sizeof (LineNumberInfo) * dmji->num_line_numbers)
			: NULL;
		uint32_t written = 0;

		for (uint32_t i = 0; i < dmji->num_line_numbers; ++i) {
			MonoDebugSourceLocation *loc = mono_debug_lookup_source_location (
				method, dmji->line_numbers [i].native_offset, mono_domain_get ());
			if (loc == NULL)
				continue;

			if (written == 0)
				vtune_method.source_file_name = g_strdup (loc->source_file);
			lines [written].Offset = dmji->line_numbers [i].native_offset;
			lines [written].LineNumber = loc->row;
			++written;

			mono_debug_free_source_location (loc);
		}

		if (written != 0) {
			vtune_method.line_number_size = written;
			vtune_method.line_number_table = lines;
		} else {
			g_free (lines);
		}

		mono_debug_free_method_jit_info (dmji);
	}

	iJIT_NotifyEvent (iJVM_EVENT_TYPE_METHOD_LOAD_FINISHED, &vtune_method);

	g_free (vtune_method.source_file_name);
	g_free (vtune_method.line_number_table);
	g_free (signature);
	g_free (name);
	g_free (classname);
}

void
jit_code_buffer (MonoProfiler *prof, const mono_byte *buffer, uint64_t size,
                 MonoProfilerCodeBufferType type, const void *data)
{
	char *owned_name = NULL;
	const char *name;

	if (type == MONO_PROFILER_CODE_BUFFER_SPECIFIC_TRAMPOLINE) {
		owned_name = g_strdup_printf ("code_buffer_specific_trampoline_%s", (const char *) data);
		name = owned_name;
	} else {
		name = code_buffer_desc (type);
	}

	iJIT_Method_Load vtune_method;
	memset (&vtune_method, 0, sizeof (vtune_method));
	vtune_method.method_id = iJIT_GetNewMethodID ();
	vtune_method.method_name = const_cast<char *> (name);
	vtune_method.method_load_address = const_cast<mono_byte *> (buffer);
	vtune_method.method_size = (unsigned int) size;

	iJIT_NotifyEvent (iJVM_EVENT_TYPE_METHOD_LOAD_FINISHED, &vtune_method);

	g_free (owned_name);
}

} // namespace

MONO_PROFILE_INIT_API void
mono_profiler_init_ittapi (const char *desc)
{
	if (iJIT_IsProfilingActive () != iJIT_SAMPLING_ON)
		return;

	MonoProfilerHandle handle = mono_profiler_create (NULL);
	mono_profiler_set_runtime_shutdown_end_callback (handle, runtime_shutdown_end);
	mono_profiler_set_jit_done_callback (handle, method_jit_done);
	mono_profiler_set_jit_code_buffer_callback (handle, jit_code_buffer);
}

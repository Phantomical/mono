#include "publish-events.hpp"

#include "naming.hpp"

#include <string>

#include "mini-runtime.h"

#include "mono/metadata/class-internals.h"
#include "mono/metadata/marshal.h"
#include "mono/metadata/profiler-private.h"

namespace mono {

void
raise_jit_done (MonoMethod *method, MonoJitInfo *jinfo)
{
	MONO_PROFILER_RAISE (jit_code_buffer,
	                     (static_cast<const mono_byte *> (jinfo->code_start),
	                      jinfo->code_size, MONO_PROFILER_CODE_BUFFER_METHOD, method));

	if (method->wrapper_type == MONO_WRAPPER_MANAGED_TO_NATIVE) {
		MonoMethod *wrapped = mono_marshal_method_from_wrapper (method);

		/* A wrapper around a bare native function wraps no method. */
		if (wrapped != nullptr)
			MONO_PROFILER_RAISE (jit_done, (wrapped, jinfo));
	}

	MONO_PROFILER_RAISE (jit_done, (method, jinfo));

	/* --jitmap: name this code range in /tmp/perf-<pid>.map, so perf resolves
	 * compiled method bodies instead of reporting a bare address. */
	if (mono_jit_map_is_enabled ())
		mono_emit_jit_map (jinfo);
}

void
dump_object_code (MonoMethod *method, const CompiledMethod &compiled)
{
	for (const auto &[name, extent] : compiled.functions) {
		const auto &[code, size] = extent;
		std::string display = display_name (method, name);

		mono_emit_jit_dump_code (display.c_str (),
		                         const_cast<uint8_t *> (code),
		                         (guint32) size, nullptr, 0);
	}
}

} // namespace mono

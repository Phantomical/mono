#include "options.hpp"

#include "method-to-llvm.hpp"

#include <llvm/ADT/StringRef.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "mini-runtime.h"

#include "mono/metadata/class-internals.h"
#include "mono/metadata/debug-helpers.h"

namespace mono {

namespace {

/// Which methods --interp-tier0 selected, or null when it was not given.
const char *interp_tier0_filter = nullptr;

bool
is_truthy_env_var (const char *value)
{
	if (value == nullptr)
		return false;

	llvm::StringRef set (value);

	return !set.empty () && set != "0" && !set.equals_insensitive ("false");
}

} // namespace

bool
is_jit_trace_enabled ()
{
	static bool on = is_truthy_env_var (g_getenv ("MONO_LLVM_JIT_TRACE"));

	return on;
}

bool
dumping (const char *name)
{
	static const char *filter = g_getenv ("MONO_LLVM_JIT_DUMP");

	return filter != nullptr && strstr (name, filter) != nullptr;
}

void
dump_il (MonoMethod *method, MonoMethodHeader *header)
{
	const uint8_t *code = mono_method_header_get_code (header, nullptr, nullptr);
	uint32_t size;

	mono_method_header_get_code (header, &size, nullptr);

	char *il = mono_disasm_code (nullptr, method, code, code + size);
	fprintf (stderr, "%s\n", il);
	g_free (il);
}

bool
recompiling (MonoMethod *method)
{
	static const char *filter = g_getenv ("MONO_LLVM_JIT_RECOMPILE");

	if (filter == nullptr)
		return false;

	char *name = mono_method_full_name (method, TRUE);
	bool selected = strstr (name, filter) != nullptr;

	g_free (name);
	return selected;
}

uint32_t
tier1_threshold ()
{
	static uint32_t calls = [] {
		const char *value = g_getenv ("MONO_LLVM_JIT_TIER1_THRESHOLD");

		if (value == nullptr)
			return 10;

		int set = atoi (value);

		return set > 0 ? set : 0;
	}();

	return calls;
}

void
set_interp_filter (const char *filter)
{
	interp_tier0_filter = filter;
}

/*
 * The refusals below are the ones that would be wrong rather than merely slow:
 *
 *  - a method not implemented in IL has no bytecode of its own, and reaching one
 *    goes back through mono_jit_compile_method;
 *  - the allocator and write-barrier wrappers are handed out as raw entries
 *    rather than as stubs, because SGen identifies a thread suspended in one by
 *    resolving the address through the jit-info table. Nothing stands between
 *    them and their callers that a later tier could redirect, so an interpreted
 *    one would stay interpreted for good;
 *  - a wrapper is generated for the runtime to enter natively, and several kinds
 *    of them the interpreter answers for with something that is not a callable
 *    address at all;
 *  - a method this backend writes the body of has IL that only throws, so any
 *    tier that runs the IL runs the throw.
 *
 * force_use_interpreter is the interpreter as the whole engine, where there is
 * no tier to leave for and nothing should be counting calls towards one.
 *
 * AggressiveInlining goes straight to the compiler because that is what the
 * attribute asks for, even though nothing is inlined across methods yet.
 */
bool
runs_at_tier0 (MonoMethod *method)
{
	if (interp_tier0_filter == nullptr || !mono_use_interpreter
	    || mono_ee_features.force_use_interpreter)
		return false;

	if (implemented_outside_il (method) || is_intrinsic (method)
	    || method->wrapper_type != MONO_WRAPPER_NONE
	    || (method->iflags & METHOD_IMPL_ATTRIBUTE_AGGRESSIVE_INLINING) != 0)
		return false;

	if (*interp_tier0_filter == '\0')
		return true;

	char *name = mono_method_full_name (method, TRUE);
	bool selected = strstr (name, interp_tier0_filter) != nullptr;

	g_free (name);
	return selected;
}

} // namespace mono

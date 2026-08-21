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

bool
is_truthy_env_var (const char *value)
{
	if (value == nullptr)
		return false;

	llvm::StringRef set (value);

	return !set.empty () && set != "0" && !set.equals_insensitive ("false");
}

/// What MONO_LLVM_JIT_TIER0 narrows tier 0 to.
struct Tier0Setting {
	/// Whether any method at all is entered by interpreting it.
	bool enabled = true;
	/// A substring of the printed name a method has to match, or null when
	/// every method that can start at tier 0 does.
	const char *substring = nullptr;
};

const Tier0Setting &
tier0_setting ()
{
	static Tier0Setting setting = [] () -> Tier0Setting {
		const char *value = g_getenv ("MONO_LLVM_JIT_TIER0");

		if (value == nullptr)
			return {};

		if (!is_truthy_env_var (value))
			return { false, nullptr };

		return { true, value };
	} ();

	return setting;
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

uint32_t
promotion_batch_size ()
{
	static uint32_t methods = [] () -> uint32_t {
		const char *value = g_getenv ("MONO_LLVM_JIT_BATCH");

		/*
		 * Eight, measured: against one method per compile it takes 46% off
		 * a corpus of three-instruction methods and 24% off a corpus of
		 * medium ones. Sixteen takes another 12% and 2%. The whole batch has
		 * to compile before any of its methods is published. That is why
		 * the larger batch buys almost nothing on the workload that has the
		 * most to wait for.
		 */
		if (value == nullptr)
			return 8;

		int set = atoi (value);

		return set > 1 ? (uint32_t) set : 1;
	}();

	return methods;
}

bool
tier2_enabled ()
{
	static bool on = [] {
		const char *value = g_getenv ("MONO_LLVM_JIT_TIER2");

		return value == nullptr || is_truthy_env_var (value);
	}();

	return on;
}

/*
 * Five thousand calls of the tier-1 body, on top of the ten at tier 0 that body
 * cost. It is not a tuned number. A tier-2 compile runs the O3 pipeline with an
 * optimizing selector, against a tier-1 body that is O1 and FastISel. The
 * threshold has to be high enough that a method running a few hundred times
 * keeps the body it has. What the trade is worth past that wants an
 * execution-count distribution off a real workload.
 *
 * The counter counts only entries, so a loop that runs for a minute inside one
 * call never reaches it. Lowering the threshold does not reach that method.
 */
uint32_t
tier2_threshold ()
{
	static uint32_t calls = [] () -> uint32_t {
		const char *value = g_getenv ("MONO_LLVM_JIT_TIER2_THRESHOLD");

		if (value == nullptr)
			return 5000;

		int set = atoi (value);

		// Zero is an instrumented body that never promotes on its own, which
		// is what a test driving the tiers through
		// Mono.Tiering.MonoTier::PromoteNow wants.
		return set > 0 ? (uint32_t) set : 0;
	}();

	return calls;
}

uint32_t
trivial_inline_il_limit ()
{
	static uint32_t bytes = [] () -> uint32_t {
		const char *value = g_getenv ("MONO_LLVM_JIT_INLINE_IL_LIMIT");

		/*
		 * Thirty-two bytes covers the shapes with room to spare. A forwarder
		 * with eight arguments is 22 bytes, a field chain four deep is 22, and
		 * a throw helper with three arguments is around 18. The limit is a
		 * backstop on IL the shape test already read as one of these shapes,
		 * not a policy of its own.
		 */
		if (value == nullptr)
			return 32;

		int set = atoi (value);

		return set > 0 ? (uint32_t) set : 0;
	}();

	return bytes;
}

uint32_t
trivial_inline_budget ()
{
	static uint32_t bodies = [] () -> uint32_t {
		const char *value = g_getenv ("MONO_LLVM_JIT_INLINE_BUDGET");

		// A translation each, against a compile that already costs LLVM's own
		// per-method floor several times over.
		if (value == nullptr)
			return 16;

		int set = atoi (value);

		return set > 0 ? (uint32_t) set : 0;
	}();

	return bodies;
}

int32_t
tier0_calls (MonoMethod *method)
{
	if (!runs_at_tier0 (method))
		return 0;

	return (int32_t) tier1_threshold ();
}

bool
tier0_enabled ()
{
	return tier0_setting ().enabled;
}

/*
 * The refusals below matter for correctness, not merely for speed:
 *
 *  - a method not implemented in IL has no bytecode of its own, and reaching
 *    one goes back through mono_jit_compile_method;
 *  - the allocator and write-barrier wrappers are handed out as raw entries
 *    rather than as stubs, because SGen identifies a thread suspended in one
 *    by resolving the address through the jit-info table. No redirectable stub
 *    stands between them and their callers, so tier 0 has no way out for
 *    them;
 *  - a wrapper is generated for the runtime to enter natively. The
 *    interpreter answers for several kinds of them with something that is
 *    not a callable address at all;
 *  - a method this backend writes the body of has IL that only throws, so
 *    any tier that runs the IL runs the throw.
 *
 * force_use_interpreter is the interpreter as the whole engine, where there
 * is no tier to leave for and no counter tracks calls towards one.
 */
bool
runs_at_tier0 (MonoMethod *method)
{
	const Tier0Setting &setting = tier0_setting ();

	if (!setting.enabled || !mono_use_interpreter
	    || mono_ee_features.force_use_interpreter)
		return false;

	if (implemented_outside_il (method) || is_intrinsic (method)
	    || method->wrapper_type != MONO_WRAPPER_NONE)
		return false;

	if (setting.substring == nullptr)
		return true;

	char *name = mono_method_full_name (method, TRUE);
	bool selected = strstr (name, setting.substring) != nullptr;

	g_free (name);
	return selected;
}

} // namespace mono

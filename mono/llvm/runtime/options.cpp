#include "options.hpp"

#include "method-to-llvm.hpp"

#include <llvm/ADT/StringRef.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "mini-runtime.h"

#include "mono/metadata/class-internals.h"
#include "mono/metadata/debug-helpers.h"
#include "mono/utils/mono-proclib.h"

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

struct Tier0Setting {
	bool enabled = true;
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
		 * A batch pays LLVM's per-compile floor once for every method in
		 * it, so a bigger batch spreads that floor further. The queue has
		 * the depth to use this: on Roslyn compiling corlib the average
		 * method arrives in a batch of 27 of a possible 32, and a third of
		 * the batches leave the queue completely full.
		 *
		 * Measure it method-weighted rather than batch-weighted. The
		 * distribution is bimodal - a spike at one and a bigger spike at
		 * the cap - so the average batch holds 14 while the average method
		 * arrives in one of 27, and the second is what the amortising acts
		 * on.
		 *
		 * The whole batch compiles before any of its methods is published,
		 * so a big batch makes each method wait for the slowest in it. That
		 * is what bounds the setting, rather than the amortising running
		 * out.
		 */
		if (value == nullptr)
			return 32;

		int set = atoi (value);

		return set > 1 ? (uint32_t) set : 1;
	}();

	return methods;
}

uint32_t
compile_worker_count ()
{
	static uint32_t threads = [] () -> uint32_t {
		const char *value = g_getenv ("MONO_LLVM_JIT_WORKERS");

		if (value != nullptr) {
			int set = atoi (value);

			return set > 1 ? (uint32_t) set : 1;
		}

		/*
		 * Two processors are left to the program, and eight threads is the
		 * cap however many it has.
		 *
		 * Compiles scale badly - ORC takes its session lock once per compile
		 * to make a JITDylib and again to look the entry up, which measured
		 * 2.86x out of 18 threads. The cap is not set from that number,
		 * because it describes throughput and what a method waits for is
		 * latency: on a queue thousands deep, sublinear speedup is still
		 * speedup. Roslyn compiling corlib waits 2.5 s for a worker at four
		 * threads and 0.25 s at eight.
		 *
		 * What the process gets back is not cheaper compiling. Eight threads
		 * spend 75% more CPU compiling than four do, for 17% more bodies.
		 * They win because a method waiting for a body runs interpreted, and
		 * the interpretation the shorter wait displaces is worth more than
		 * the extra compiling costs - on that workload 34 s more compile CPU
		 * against 39 s less of everything else.
		 *
		 * mono_cpu_count () reads the affinity mask and the cgroup quota, so a
		 * container gets what it can use rather than what the machine has.
		 */
		int cpus = mono_cpu_count ();

		return (uint32_t) std::max (std::min (cpus - 2, 8), 1);
	}();

	return threads;
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
 * Twenty thousand entries of the tier-1 body, on top of the ten calls at tier 0
 * that body cost. A tier-2 compile runs the O3 pipeline with an optimizing
 * selector against a tier-1 body that is O1 and FastISel, so the threshold buys
 * a better body with a compile, and the method has to run enough afterwards to
 * pay for it.
 *
 * Five thousand was too eager on both workloads it has been measured against.
 * Roslyn compiling this tree's corlib reads -13% of process CPU at twenty
 * thousand over 8 paired reps, almost all of it compile time it no longer
 * spends, because that workload is full of methods entered a few thousand times
 * and then never again. SharpChess reads -2.5% over 8 reps, and it is the arm
 * that says how far this can go: at two hundred thousand it turns and costs
 * +8.5%, because its hot methods stay at tier 1 for the whole search.
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
			return 20000;

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
		 * A forwarder with eight arguments is 22 bytes, a field chain four
		 * deep is 22, and a throw helper with three arguments is around 18.
		 * The limit is a backstop on IL the shape test already read as one of
		 * these shapes, not a policy of its own.
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

uint32_t
costed_inline_il_limit ()
{
	static uint32_t bytes = [] () -> uint32_t {
		const char *value = g_getenv ("MONO_LLVM_JIT_INLINE_COST_IL_LIMIT");

		/*
		 * A number with no sweep behind it.
		 */
		if (value == nullptr)
			return 128;

		int set = atoi (value);

		return set > 0 ? (uint32_t) set : 0;
	}();

	return bytes;
}

uint32_t
inline_depth_limit ()
{
	static uint32_t levels = [] () -> uint32_t {
		const char *value = g_getenv ("MONO_LLVM_JIT_INLINE_DEPTH");

		if (value == nullptr)
			return 4;

		int set = atoi (value);

		return set > 0 ? (uint32_t) set : 0;
	}();

	return levels;
}

uint32_t
trivial_inline_depth_limit ()
{
	static uint32_t levels = [] () -> uint32_t {
		const char *value = g_getenv ("MONO_LLVM_JIT_INLINE_PREPASS_DEPTH");

		if (value == nullptr)
			return 2;

		int set = atoi (value);

		return set > 0 ? (uint32_t) set : 0;
	}();

	return levels;
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

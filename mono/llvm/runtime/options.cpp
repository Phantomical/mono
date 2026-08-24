#include "options.hpp"

#include "method-to-llvm.hpp"

#include <llvm/ADT/StringRef.h>

#include <algorithm>
#include <cstdint>
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

std::mutex &
jit_trace_mutex ()
{
	/* Leaked on purpose. A compile worker can still print while the process
	 * exits, and a mutex that ran its destructor first is undefined to lock. */
	static std::mutex *lock = new std::mutex ();

	return *lock;
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

std::chrono::milliseconds
compile_worker_idle_timeout ()
{
	static std::chrono::milliseconds timeout = [] {
		const char *value = g_getenv ("MONO_LLVM_JIT_WORKER_IDLE_MS");

		/*
		 * A second. That is long against the burst a promotion arrives in, and
		 * short against the quiet a program settles into.
		 *
		 * An idle thread costs a signal and a wait at every collection. The
		 * default suspend policy is preemptive, and it suspends an attached
		 * thread wherever that thread parked. bh (BH.exe -b 700 -s 1000) takes
		 * 51010 voluntary context switches and 1.64 s of system time while it
		 * holds its workers, against 17367 and 0.75 s while it retires them.
		 *
		 * A restart costs one attach. It also costs the pipelines and the
		 * TargetMachine on the first compile that follows, because those are
		 * thread_local (jit.cpp). A 1 ms timeout retires a thread as soon as it
		 * runs dry, and turns that same run into 114 more thread starts. Compile
		 * CPU goes from 4.09 ms a method to 4.51, so a restart is around 0.7 ms.
		 * Most of it is in cgsetup and pbsetup.
		 *
		 * The floor is therefore soft. A shorter timeout cuts more of the
		 * transient that a short run is mostly made of, and 250 ms takes bh to
		 * 7066 switches. A second is the conservative end of that range. It
		 * keeps a program that promotes a method every few seconds from
		 * restarting a thread for each one.
		 */
		if (value == nullptr)
			return std::chrono::milliseconds (1000);

		int set = atoi (value);

		return std::chrono::milliseconds (set > 0 ? set : 0);
	}();

	return timeout;
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

bool
inline_write_barrier ()
{
	static bool on = [] {
		const char *value = g_getenv ("MONO_LLVM_JIT_INLINE_WBARRIER");

		return value == nullptr || is_truthy_env_var (value);
	}();

	return on;
}

/*
 * A tier-1 body promotes on whichever of two counters runs out first, and these
 * are the two thresholds. A tier-2 compile runs the O3 pipeline with an
 * optimizing selector against a tier-1 body that is O1 and FastISel, so a
 * threshold buys a better body with a compile, and the method has to run enough
 * afterwards to pay for it.
 *
 * Neither count alone covers the methods that pay. Both are needed because the
 * two populations do not overlap:
 *
 * Calls. Twenty thousand entries of the tier-1 body, on top of the ten calls at
 * tier 0 that body cost. Five thousand was too eager on both workloads it has
 * been measured against. Roslyn compiling this tree's corlib reads -13% of
 * process CPU at twenty thousand over 8 paired reps, almost all of it compile
 * time it no longer spends, because that workload is full of methods entered a
 * few thousand times and then never again. SharpChess reads -2.5% over 8 reps,
 * and it is the arm that says how far this can go: at two hundred thousand it
 * turns and costs +8.5%, because its hot methods stay at tier 1 for the whole
 * search. Those hot methods are property getters of three instructions, which no
 * count of work reaches however often they run, and tier 2 pays there by folding
 * them into their callers.
 *
 * Work. A count of calls says nothing about how long a method runs. euler spends
 * 40% of its run inside Euler.Tunnel:calculateR (), which is entered far too few
 * times to reach any call threshold either arm above would accept, and its
 * tier-2 body measures 22% faster than the one stock mono emits. Counting the
 * work a body does reaches it on the turns of its loop instead. A hundred million
 * costs euler -24% of process CPU over 5 paired pairs and takes calculateR () and
 * the rest of the Euler.Tunnel kernel to tier 2, where the call count took none
 * of them.
 *
 * The number is large because the work count must not fire where the call count
 * would have got there. By the time a method has taken twenty thousand calls it
 * has done twenty thousand times its per-call work, so a hundred million
 * pre-empts the call count only for a method that does more than about five
 * thousand instructions in a call. That is the loop-bound population this second
 * counter is for.
 *
 * One million puts that figure at fifty, which is most methods. It cost
 * SharpChess +7.3% of CPU and took its promotions from 241 down to 222, because
 * methods reached tier 2 on a profile of one or two calls. Ten million cost
 * pystone under IronPython +4.9%, where a hundred million costs it +2.6%; euler
 * reads the same at both. Each of those arms is paired, alternating, and read on
 * CPU rather than wall.
 */
uint64_t
tier2_threshold ()
{
	static uint64_t calls = [] () -> uint64_t {
		const char *value = g_getenv ("MONO_LLVM_JIT_TIER2_THRESHOLD");

		if (value == nullptr)
			return 20000;

		int set = atoi (value);

		// Zero is an instrumented body that never promotes on its own, which
		// is what a test driving the tiers through
		// Mono.Tiering.MonoTier::PromoteNow wants.
		return set > 0 ? (uint64_t) set : 0;
	}();

	return calls;
}

uint64_t
tier2_cost_threshold ()
{
	static uint64_t work = [] () -> uint64_t {
		// The call threshold at zero is the switch that turns automatic
		// promotion off for good, so it turns this half off with it.
		if (tier2_threshold () == 0)
			return 0;

		const char *value = g_getenv ("MONO_LLVM_JIT_TIER2_COST_THRESHOLD");

		if (value == nullptr)
			return 100000000;

		char *end = nullptr;
		unsigned long long set = strtoull (value, &end, 10);

		if (end == value || set == 0)
			return 0;

		// The counter is signed, so that a cost can take it past zero and the
		// thread that crossed can see that it did.
		return std::min<unsigned long long> (set, INT64_MAX);
	}();

	return work;
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
costed_inline_budget ()
{
	static uint32_t bodies = [] () -> uint32_t {
		const char *value = g_getenv ("MONO_LLVM_JIT_INLINE_COST_BUDGET");

		// The same count the pre-pass gets. What the cost model translates it
		// also weighs, and a candidate it refuses is stripped again, so this
		// bounds the questions rather than the code a body ends up with.
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

		/*
		 * Generous rather than tuned. Reach stops paying well before this on
		 * both corpora we have, and the room past that is deliberate: we
		 * would rather the pre-pass folds a little too much than too little,
		 * and this is not tuned to the benchmarks in the tree. What bounds
		 * the translation is the budget, in materialize_trivial_callees ().
		 */
		if (value == nullptr)
			return 8;

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

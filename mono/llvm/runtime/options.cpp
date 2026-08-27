#include "options.hpp"

#include "jit.hpp"
#include "method-to-llvm.hpp"
#include "naming.hpp"

#include <llvm/ADT/StringRef.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "mini-runtime.h"

#include "mono/metadata/class-internals.h"
#include "mono/metadata/marshal.h"
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
		 * One thread while LLVM is printing. Both tiers print to stderr, and
		 * two compiles printing at once interleave into text that names no
		 * method. It narrows the overlap rather than removing it: a compile
		 * the runtime asks for by name runs on the thread that asked, and can
		 * still print over the worker.
		 */
		if (ir_printing_enabled ())
			return 1;

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
fold_casts ()
{
	static bool on = [] {
		const char *value = g_getenv ("MONO_LLVM_JIT_FOLD_CASTS");

		return value == nullptr || is_truthy_env_var (value);
	}();

	return on;
}

llvm::FastMathFlags
relaxed_float_flags ()
{
	static llvm::FastMathFlags flags = [] {
		llvm::FastMathFlags relaxed;

		if (!mono_use_fast_math)
			return relaxed;

		// Each of these gives back a value the operation did not compute, and
		// each is what one of the transforms --ffast-math is asked for needs.
		// nnan and ninf are left out: ECMA-335 I.12.1.3 makes a NaN or an
		// infinity the answer an ordinary operation gives, and ckfinite is how
		// a program tests for one.
		relaxed.setAllowReassoc ();
		relaxed.setNoSignedZeros ();
		relaxed.setAllowReciprocal ();
		relaxed.setAllowContract ();
		relaxed.setApproxFunc ();

		return relaxed;
	}();

	return flags;
}

/*
 * A tier-1 body spends one counter and asks for tier 2 when it runs out. The
 * counter is charged for the work the body does, one unit for each instruction
 * that emits code, and for each call, at the entry weight below. A tier-2 compile
 * runs the O3 pipeline with an optimizing selector against a tier-1 body that is
 * O1 and FastISel, so the threshold buys a better body with a compile, and the
 * method has to run enough afterwards to pay for it.
 *
 * Two populations pay for that compile, and the entry weight is what puts both of
 * them on one counter:
 *
 * Work. A count of calls says nothing about how long a method runs. euler spends
 * 40% of its run inside Euler.Tunnel:calculateR (), which is entered eleven
 * times, and its tier-2 body measures 22% faster than the one stock mono emits.
 * The turns of its loop reach a threshold on work that no count of calls reaches.
 *
 * Calls. A count of work says nothing about how often a method is called, and a
 * body of three instructions never reaches a threshold on work however hot it is.
 * SharpChess is full of those, property getters called millions of times, and
 * tier 2 pays there by folding them into their callers rather than by emitting
 * them better. A weight for each call is what reaches them.
 *
 * The defaults put a body that does no work at twenty thousand calls, which is
 * a hundred million over five thousand. That call figure is the calibrated one:
 * five thousand was too eager on every workload it was measured against, and at
 * two hundred thousand SharpChess turns and costs +8.5% of CPU, because its hot
 * getters then stay at tier 1 for the whole search.
 *
 * The threshold is large for the same reason the call figure is. A method that
 * has taken twenty thousand calls has done twenty thousand times its per-call
 * work, so a hundred million reaches a loop-bound body first only where that body
 * does more than about five thousand instructions in a call. A million puts that
 * figure at fifty, which is most methods: it cost SharpChess +7.3% of CPU and
 * took its promotions from 241 down to 222, because methods reached tier 2 on a
 * profile of one or two calls. Ten million cost pystone under IronPython +4.9%,
 * where a hundred million costs it +2.6%, and euler reads the same at both.
 *
 * One counter that adds the two is more eager than two counters that promote on
 * whichever runs out first, because a body half way through each of them has
 * spent this one. The two are furthest apart for a body whose per-call work is
 * the entry weight, and there the merged count promotes in half the calls. At
 * both ends, a body that does nothing in a call and a body that does very much,
 * the two agree.
 *
 * Every arm quoted here is paired, with the two arms adjacent and the pair order
 * alternated, and read on CPU rather than wall.
 */
uint64_t
tier2_threshold ()
{
	static uint64_t cost = [] () -> uint64_t {
		const char *value = g_getenv ("MONO_LLVM_JIT_TIER2_THRESHOLD");

		if (value == nullptr)
			return 100000000;

		char *end = nullptr;
		unsigned long long set = strtoull (value, &end, 10);

		// Zero is an instrumented body that never promotes on its own, which
		// is what a test driving the tiers through
		// Mono.Tiering.MonoTier::PromoteNow wants. A value nothing parses
		// answers the same way.
		if (end == value || set == 0)
			return 0;

		// The counter is signed, so that a cost can take it past zero and the
		// thread that crossed can see that it did.
		return std::min<unsigned long long> (set, INT64_MAX);
	}();

	return cost;
}

uint64_t
tier2_entry_weight ()
{
	static uint64_t weight = [] () -> uint64_t {
		// The threshold at zero turns automatic promotion off for good, and a
		// weight with no counter to charge is worth nothing.
		if (tier2_threshold () == 0)
			return 0;

		const char *value = g_getenv ("MONO_LLVM_JIT_TIER2_ENTRY_WEIGHT");

		if (value == nullptr)
			return 5000;

		char *end = nullptr;
		unsigned long long set = strtoull (value, &end, 10);

		if (end == value)
			return 0;

		// A weight past the threshold would promote every body on its first
		// exit, which the threshold itself already expresses.
		return std::min<unsigned long long> (set, tier2_threshold ());
	}();

	return weight;
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
inline_round_limit ()
{
	static uint32_t rounds = [] () -> uint32_t {
		const char *value = g_getenv ("MONO_LLVM_JIT_INLINE_ROUNDS");

		if (value == nullptr)
			return 4;

		int set = atoi (value);

		return set > 0 ? (uint32_t) set : 1;
	}();

	return rounds;
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
 * Whether a wrapper of this kind can run at tier 0.
 *
 * A wrapper carries IL of its own, so most kinds interpret like an ordinary
 * method, and generate_code () (`mono/interp/transform/transform.cpp`) refuses
 * the opcodes the interpreter does not implement. The refusals below are each
 * about how the body is entered rather than what it holds.
 *
 * Native code enters through a C-convention entry, which
 * publishes_interop_entry () names. The interpreter's entry is not that shape.
 *
 * The allocator and the write barrier are handed out as raw entries rather than
 * through a thunk. SGen identifies a thread suspended in one by resolving the
 * address through the jit-info table. Nothing redirectable stands between them
 * and their callers, so tier 0 has no way out for them.
 *
 * The marshalling that crosses between the engines is its own entry. An
 * interp_in wrapper is how compiled code reaches the interpreter, and a
 * gsharedvt_out_sig wrapper is how an interpreted caller reaches compiled code.
 * interp_lmf and gsharedvt_in_sig cross the same two ways. To interpret any of
 * them skips the crossing it exists to make.
 *
 * The gsharedvt_in and gsharedvt_out wrappers are not in the list.
 * compile_special () (`mono/mini/mini-runtime.c`) answers both with an arch
 * trampoline before any of this runs, and their IL is one ret.
 *
 * A managed-to-native wrapper is refused because the interpreter needs some of
 * them to run at all. Interpreting the wrapper for the array-allocation icall
 * makes the interpreter allocate an array, which calls that same wrapper, and
 * the thread runs out of stack. Boehm is where this shows, because SGen's
 * managed allocator keeps the hot path off the icall.
 */
static bool
wrapper_runs_at_tier0 (MonoMethod *method)
{
	/*
	 * A dynamic method carries IL of its own, and the interpreter's transform
	 * reads the wrapper data its tokens name. What makes the exception worth
	 * having is what writes one. Reflection.Emit does, and a program that
	 * generates code generates many: IronJS writes a dynamic method for each
	 * JavaScript function. create_delegate_method_ptr () then compiles each of
	 * them where the delegate over it is made, on that thread, before the first
	 * call. Tier 0 is what keeps the ones that never get hot out of the
	 * compiler.
	 */
	if (method->wrapper_type == MONO_WRAPPER_DYNAMIC_METHOD)
		return true;

	if (publishes_interop_entry (method))
		return false;

	if (method->wrapper_type == MONO_WRAPPER_ALLOC
	    || method->wrapper_type == MONO_WRAPPER_WRITE_BARRIER
	    || method->wrapper_type == MONO_WRAPPER_MANAGED_TO_NATIVE)
		return false;

	WrapperInfo *info = mono_marshal_get_wrapper_info (method);

	if (info == nullptr)
		return true;

	switch (info->subtype) {
	case WRAPPER_SUBTYPE_GSHAREDVT_IN_SIG:
	case WRAPPER_SUBTYPE_GSHAREDVT_OUT_SIG:
	case WRAPPER_SUBTYPE_INTERP_IN:
	case WRAPPER_SUBTYPE_INTERP_LMF:
		return false;
	default:
		return true;
	}
}

/*
 * The refusals below matter for correctness, not merely for speed:
 *
 *  - a method not implemented in IL has no bytecode of its own, and reaching
 *    one goes back through mono_jit_compile_method;
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

	if (implemented_outside_il (method) || is_intrinsic (method))
		return false;

	if (method->wrapper_type != MONO_WRAPPER_NONE && !wrapper_runs_at_tier0 (method))
		return false;

	if (setting.substring == nullptr)
		return true;

	char *name = mono_method_full_name (method, TRUE);
	bool selected = strstr (name, setting.substring) != nullptr;

	g_free (name);
	return selected;
}

} // namespace mono

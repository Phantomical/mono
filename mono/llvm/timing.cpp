/**
 * \file
 * \brief Per-phase accounting of where a compile's time goes.
 *
 * Off unless MONO_LLVM_JIT_TIMING is set, and then a line per phase goes to
 * stderr when the process exits. What this answers is which part of a compile
 * is expensive - the front end, the pass pipeline, codegen or the linker -
 * which otherwise wants a sampling profiler, and there is not always one to
 * hand.
 *
 * The value is a comma-separated set of words. `cpu` charges each phase the
 * thread CPU time it used rather than wall clock, which is what makes a run on
 * a loaded machine comparable to a quiet one; it costs about ten times as much
 * per reading, so it is not the default. `fine` turns on the sub-phases, which
 * split the four expensive phases into the pieces a per-compile floor is
 * actually made of.
 */

#include "timing.hpp"

#include <atomic>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string_view>

namespace mono {
namespace timing {

namespace {

struct Bucket {
	std::atomic<uint64_t> total { 0 };
	std::atomic<uint64_t> self { 0 };
	std::atomic<uint64_t> count { 0 };
};

Bucket g_buckets[(size_t) Phase::count];
thread_local Scope *g_current = nullptr;

/// The first sub-phase; everything from here on is gated on `fine`.
constexpr Phase first_fine = Phase::ctxnew;

bool g_cpu_clock = false;
clockid_t g_clock = CLOCK_MONOTONIC;

bool
names (std::string_view setting, std::string_view word)
{
	size_t at = 0;

	while (at < setting.size ()) {
		size_t end = setting.find (',', at);

		if (end == std::string_view::npos)
			end = setting.size ();
		if (setting.substr (at, end - at) == word)
			return true;
		at = end + 1;
	}
	return false;
}

uint64_t
now_ns ()
{
	struct timespec ts;

	clock_gettime (g_clock, &ts);
	return (uint64_t) ts.tv_sec * 1000000000ull + (uint64_t) ts.tv_nsec;
}

const char *
name_of (Phase phase)
{
	switch (phase) {
	case Phase::compile:
		return "compile";
	case Phase::metadata:
		return "metadata";
	case Phase::translate:
		return "translate";
	case Phase::resolve:
		return "resolve";
	case Phase::orc:
		return "orc";
	case Phase::pipeline:
		return "pipeline";
	case Phase::codegen:
		return "codegen";
	case Phase::jinfo:
		return "jinfo";
	case Phase::dylib:
		return "dylib";
	case Phase::vslots:
		return "vslots";
	case Phase::cgsetup:
		return "cgsetup";
	case Phase::cgrun:
		return "cgrun";
	case Phase::ctxnew:
		return "  ctxnew";
	case Phase::addir:
		return "  addir";
	case Phase::jlink:
		return "  jlink";
	case Phase::pbsetup:
		return "  pbsetup";
	case Phase::prun:
		return "  prun";
	case Phase::mmi:
		return "  mmi";
	case Phase::cgpass:
		return "  cgpass";
	case Phase::strm:
		return "  strm";
	case Phase::aprint:
		return "  aprint";
	case Phase::isel:
		return "  isel";
	case Phase::mpass:
		return "  mpass";
	case Phase::emit:
		return "  emit";
	case Phase::sidetbl:
		return "  sidetbl";
	case Phase::objout:
		return "  objout";
	case Phase::pmfree:
		return "  pmfree";
	case Phase::tsmfree:
		return "  tsmfree";
	case Phase::lgraph:
		return "  lgraph";
	case Phase::memfin:
		return "  memfin";
	default:
		return "?";
	}
}

/*
 * Self time as a percentage of the whole rather than of the parent phase, so
 * the column sums to a hundred and any two phases can be read off against each
 * other. A sub-phase's self is inside its parent's total but not its parent's
 * self, so the column still sums to a hundred with them on.
 */
void
report ()
{
	uint64_t whole = g_buckets[(size_t) Phase::compile].total.load ();

	if (whole == 0)
		return;

	fprintf (stderr, "[llvm-jit] clock %s\n",
	         g_cpu_clock ? "thread-cpu" : "monotonic");
	fprintf (stderr, "[llvm-jit] %-10s %8s %13s %12s %7s %10s\n", "phase",
	         "calls", "total ms", "self ms", "self %", "self us/c");
	for (size_t i = 0; i < (size_t) Phase::count; i++) {
		const Bucket &b = g_buckets[i];
		uint64_t count = b.count.load ();

		if (count == 0)
			continue;
		fprintf (stderr,
		         "[llvm-jit] %-10s %8" PRIu64 " %13.1f %12.1f %7.1f %10.1f\n",
		         name_of ((Phase) i), count, b.total.load () / 1e6,
		         b.self.load () / 1e6, 100.0 * b.self.load () / whole,
		         b.self.load () / 1e3
		                 / g_buckets[(size_t) Phase::compile].count.load ());
	}
}

void
account (Phase phase, uint64_t elapsed, uint64_t children)
{
	Bucket &b = g_buckets[(size_t) phase];

	b.total.fetch_add (elapsed, std::memory_order_relaxed);
	b.self.fetch_add (elapsed - children, std::memory_order_relaxed);
	b.count.fetch_add (1, std::memory_order_relaxed);
}

} // namespace

bool
enabled ()
{
	static const bool on = [] {
		const char *setting = std::getenv ("MONO_LLVM_JIT_TIMING");

		if (setting == nullptr)
			return false;
		if (names (setting, "cpu")) {
			g_cpu_clock = true;
			g_clock = CLOCK_THREAD_CPUTIME_ID;
		}
		std::atexit (report);
		return true;
	}();
	return on;
}

bool
fine ()
{
	static const bool on = [] {
		const char *setting = std::getenv ("MONO_LLVM_JIT_TIMING");

		return enabled () && setting != nullptr && names (setting, "fine");
	}();
	return on;
}

Scope::Scope (Phase phase) : phase_ (phase)
{
	if (!(phase < first_fine ? enabled () : fine ())) {
		parent_ = nullptr;
		start_ = 0;
		return;
	}

	parent_ = g_current;
	g_current = this;
	start_ = now_ns ();
}

Scope::~Scope ()
{
	if (start_ == 0)
		return;

	uint64_t elapsed = now_ns () - start_;

	account (phase_, elapsed, children_);

	if (parent_ != nullptr)
		parent_->children_ += elapsed;
	g_current = parent_;
}

uint64_t
span_begin (Phase phase)
{
	if (!(phase < first_fine ? enabled () : fine ()))
		return 0;

	/*
	 * A reading of exactly zero would read as "nothing recorded"; the clocks
	 * here are nanoseconds since boot or since the thread started, so this is
	 * only reachable in the first nanosecond of a thread's life.
	 */
	uint64_t start = now_ns ();

	return start == 0 ? 1 : start;
}

void
span_end (Phase phase, uint64_t start)
{
	if (start == 0)
		return;

	uint64_t elapsed = now_ns () - start;

	account (phase, elapsed, 0);

	if (g_current != nullptr)
		g_current->children_ += elapsed;
}

} // namespace timing
} // namespace mono

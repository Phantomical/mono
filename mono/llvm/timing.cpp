/**
 * \file
 * \brief Per-phase accounting of where a compile's time goes.
 *
 * Off unless MONO_LLVM_JIT_TIMING is set, and then a line per phase goes to
 * stderr when the process exits. What this answers is which part of a compile
 * is expensive - the front end, the pass pipeline, codegen or the linker -
 * which otherwise wants a sampling profiler, and there is not always one to
 * hand.
 */

#include "timing.hpp"

#include <atomic>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <ctime>

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

uint64_t
now_ns ()
{
	struct timespec ts;

	clock_gettime (CLOCK_MONOTONIC, &ts);
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
	case Phase::dwarf:
		return "dwarf";
	case Phase::cgsetup:
		return "cgsetup";
	case Phase::cgrun:
		return "cgrun";
	default:
		return "?";
	}
}

/*
 * Self time as a percentage of the whole rather than of the parent phase, so
 * the column sums to a hundred and any two phases can be read off against each
 * other.
 */
void
report ()
{
	uint64_t whole = g_buckets[(size_t) Phase::compile].total.load ();

	if (whole == 0)
		return;

	fprintf (stderr, "[llvm-jit] %-10s %8s %13s %12s %7s\n", "phase", "calls",
	         "total ms", "self ms", "self %");
	for (size_t i = 0; i < (size_t) Phase::count; i++) {
		const Bucket &b = g_buckets[i];
		uint64_t count = b.count.load ();

		if (count == 0)
			continue;
		fprintf (stderr, "[llvm-jit] %-10s %8" PRIu64 " %13.1f %12.1f %7.1f\n",
		         name_of ((Phase) i), count, b.total.load () / 1e6,
		         b.self.load () / 1e6, 100.0 * b.self.load () / whole);
	}
}

} // namespace

bool
enabled ()
{
	static const bool on = [] {
		if (std::getenv ("MONO_LLVM_JIT_TIMING") == nullptr)
			return false;
		std::atexit (report);
		return true;
	}();
	return on;
}

Scope::Scope (Phase phase) : phase_ (phase)
{
	if (!enabled ()) {
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
	Bucket &b = g_buckets[(size_t) phase_];

	b.total.fetch_add (elapsed, std::memory_order_relaxed);
	b.self.fetch_add (elapsed - children_, std::memory_order_relaxed);
	b.count.fetch_add (1, std::memory_order_relaxed);

	if (parent_ != nullptr)
		parent_->children_ += elapsed;
	g_current = parent_;
}

} // namespace timing
} // namespace mono

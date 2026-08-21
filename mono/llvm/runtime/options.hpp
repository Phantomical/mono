/**
 * \file
 * \brief The environment knobs the engine reads, and the tier-0 filter the
 * command line sets.
 *
 * Every value here is read once and cached, so setting one of these variables
 * after the first method has compiled does nothing. That is deliberate:
 * caching keeps a variable changed partway through a run from splitting a
 * method's state across two policies. This means a test that wants one has to
 * set it before starting the runtime, not around the call it is interested in.
 */

#ifndef MONO_LLVM_RUNTIME_OPTIONS_HPP
#define MONO_LLVM_RUNTIME_OPTIONS_HPP

#include <cstdint>

typedef struct _MonoMethod MonoMethod;
typedef struct _MonoMethodHeader MonoMethodHeader;

namespace mono {

/// Whether MONO_LLVM_JIT_TRACE asked to see every method the engine translates.
///
/// This matters because a method reached as a callee is compiled without the
/// runtime asking for it. Only this flag reports that it happened.
bool is_jit_trace_enabled ();

/// Whether MONO_LLVM_JIT_DUMP names this method: a substring of its full name
/// selects it for having its IL and translated IR printed to stderr.
bool dumping (const char *name);

/// Print a method's IL to stderr.
void dump_il (MonoMethod *method, MonoMethodHeader *header);

/// Whether MONO_LLVM_JIT_RECOMPILE names this method: a substring of its full
/// name selects it. A selected method is translated afresh on every compile
/// request rather than answered from the cache, which gives it more than one
/// live body. It is a way to exercise the paths that have to cope with that,
/// rather than a tiering policy. One such path is the debugger, which arms a
/// breakpoint everywhere a method is executing.
bool recompiling (MonoMethod *method);

/// How many calls an interpreted method is given before it is compiled.
///
/// Ten, and it is not a tuned number. All a threshold has to do is keep
/// methods called once or twice out of the compiler. What the trade is worth
/// past that needs an execution-count distribution measured off a real
/// workload, not an argument. MONO_LLVM_JIT_TIER1_THRESHOLD moves it, which is
/// the point of it being a variable at all. Zero there leaves the selected
/// methods interpreted for good. That is what separates the tier-0 entry path
/// from promotion when one of them misbehaves.
uint32_t tier1_threshold ();

/// How many methods a tier-1 promotion compile can take at once.
///
/// Methods promoted close together are translated into one module and compiled
/// together, which pays LLVM's per-compile cost once instead of once each.
/// MONO_LLVM_JIT_BATCH moves it, and one there compiles every method on its own.
/// Tier 2 is never batched: its code is laid out by its own method's counts.
uint32_t promotion_batch_size ();

/// Whether tier 2 exists at all, which is what decides whether a tier-1 body
/// carries profiling instrumentation.
///
/// On unless MONO_LLVM_JIT_TIER2 turns it off.
bool tier2_enabled ();

/// How many calls a tier-1 body takes before it asks to be compiled again.
///
/// Zero for a body that never asks, which leaves it instrumented and counting
/// while something else decides when it promotes.
/// MONO_LLVM_JIT_TIER2_THRESHOLD moves it, and the default is five thousand.
uint32_t tier2_threshold ();

/// The largest callee, in IL bytes, tier 2 folds into its caller before any
/// cost model has looked at it.
///
/// Thirty-two, which leaves room to spare on the shapes the pre-pass
/// recognizes. MONO_LLVM_JIT_INLINE_IL_LIMIT moves it. Zero there turns the
/// pre-pass off, which is what separates a bug in a folded body from one in
/// the method that folded it.
uint32_t trivial_inline_il_limit ();

/// How many bodies one tier-2 compile can fold in.
///
/// MONO_LLVM_JIT_INLINE_BUDGET moves it. It bounds the translation the pre-pass
/// adds to a compile. A chain of forwarders is what spends it.
uint32_t trivial_inline_budget ();

/// How many calls a method takes at tier 0 before it is asked for as tier 1.
///
/// Zero for a method that does not run at tier 0 at all. That also tells a
/// caller that counting its calls settles nothing.
int32_t tier0_calls (MonoMethod *method);

/// Whether any method at all is entered by interpreting it.
///
/// Answers before there is a method to ask about, which is what the decision to
/// start the interpreter needs.
bool tier0_enabled ();

/// Whether a method is entered by interpreting its bytecode rather than by
/// compiling it.
///
/// Every method the interpreter can run starts there. MONO_LLVM_JIT_TIER0
/// narrows that for debugging. A false value keeps every method out of tier 0.
/// Anything else is matched as a substring of the printed name.
bool runs_at_tier0 (MonoMethod *method);

} // namespace mono

#endif

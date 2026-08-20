/**
 * \file
 * \brief The environment knobs the engine reads, and the tier-0 filter the
 * command line sets.
 *
 * Every value here is read once and cached, so setting one of these variables
 * after the first method has compiled does nothing. That is deliberate - a knob
 * that changed halfway through a run would split a method's state across two
 * policies - but it means a test that wants one has to set it before starting
 * the runtime rather than around the call it is interested in.
 */

#ifndef MONO_LLVM_RUNTIME_OPTIONS_HPP
#define MONO_LLVM_RUNTIME_OPTIONS_HPP

#include <cstdint>

typedef struct _MonoMethod MonoMethod;
typedef struct _MonoMethodHeader MonoMethodHeader;

namespace mono {

/// Whether MONO_LLVM_JIT_TRACE asked to see every method the engine translates.
///
/// Worth having because a method reached as a callee is compiled without the
/// runtime ever being asked for it, so nothing else says it happened.
bool is_jit_trace_enabled ();

/// Whether MONO_LLVM_JIT_DUMP names this method: a substring of its full name
/// selects it for having its IL and translated IR printed to stderr.
bool dumping (const char *name);

/// Print a method's IL to stderr.
void dump_il (MonoMethod *method, MonoMethodHeader *header);

/// Whether MONO_LLVM_JIT_RECOMPILE names this method: a substring of its full
/// name selects it for being translated afresh on every compile request rather
/// than answered from the cache, which is what gives a method more than one live
/// body. It is a way to exercise the paths that have to cope with that - the
/// debugger arming a breakpoint everywhere a method is executing - rather than a
/// tiering policy.
bool recompiling (MonoMethod *method);

/// How many calls an interpreted method is given before it is compiled.
///
/// Ten, and it is not a tuned number: all a threshold really has to do is keep
/// methods that are called once or twice out of the compiler, and what the trade
/// is worth past that needs an execution-count distribution measured off a real
/// workload rather than an argument. MONO_LLVM_JIT_TIER1_THRESHOLD moves it,
/// which is the point of it being a variable at all; zero there leaves the
/// selected methods interpreted for good, which is what separates the tier-0
/// entry path from promotion when one of them misbehaves.
uint32_t tier1_threshold ();

/// How many methods a promotion compile may take at once.
///
/// Methods promoted close together are translated into one module and compiled
/// together, which pays LLVM's per-compile cost once instead of once each.
/// MONO_LLVM_JIT_BATCH moves it, and one there compiles every method on its own.
uint32_t promotion_batch_size ();

/// Whether tier 2 exists at all, which is what decides whether a tier-1 body
/// carries profiling instrumentation.
///
/// Setting MONO_LLVM_JIT_TIER2_THRESHOLD to anything turns it on.
bool tier2_enabled ();

/// How many calls a tier-1 body takes before it asks to be compiled again.
///
/// Zero for a body that never asks, which leaves it instrumented and counting
/// while something else decides when it promotes.
/// MONO_LLVM_JIT_TIER2_THRESHOLD moves it.
uint32_t tier2_threshold ();

/// How many calls a method takes at tier 0 before it is asked for as tier 1.
///
/// Zero for a method that does not run at tier 0 at all, which is also how a
/// caller is told that counting its calls would settle nothing.
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
/// narrows that for debugging: a false value keeps every method out of tier 0,
/// and anything else is matched as a substring of the printed name.
bool runs_at_tier0 (MonoMethod *method);

} // namespace mono

#endif

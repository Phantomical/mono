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

/// Record which methods --interp-tier0 selected. Null means the option was not
/// given; an empty filter takes every method that can be interpreted at all, and
/// anything else is matched as a substring of the printed name.
///
/// The string is borrowed from the command line and is never copied.
void set_interp_filter (const char *filter);

/// Whether a method is entered by interpreting its bytecode rather than by
/// compiling it.
bool runs_at_tier0 (MonoMethod *method);

} // namespace mono

#endif

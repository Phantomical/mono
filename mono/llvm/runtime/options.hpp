/**
 * \file
 * \brief The environment knobs the engine reads, the tier-0 filter the command
 * line sets, and the lock the trace prints under.
 *
 * Every value here is read once and cached, so setting one of these variables
 * after the first method has compiled does nothing. That is deliberate:
 * caching keeps a variable changed partway through a run from splitting a
 * method's state across two policies. This means a test that wants one has to
 * set it before starting the runtime, not around the call it is interested in.
 */

#ifndef MONO_LLVM_RUNTIME_OPTIONS_HPP
#define MONO_LLVM_RUNTIME_OPTIONS_HPP

#include "../util/lock.hpp"

#include <llvm/IR/FMF.h>

#include <chrono>
#include <cstdint>
#include <mutex>

typedef struct _MonoMethod MonoMethod;
typedef struct _MonoMethodHeader MonoMethodHeader;

namespace mono {

/// Whether MONO_LLVM_JIT_TRACE asked to see every method the engine translates.
///
/// This matters because a method reached as a callee is compiled without the
/// runtime asking for it. Only this flag reports that it happened.
bool is_jit_trace_enabled ();

/// The mutex that keeps one thread's trace line whole.
///
/// Both engines trace, and compiles run on several worker threads at once.
/// Two writers reach file descriptor 2: stdio and llvm::errs (). They share no
/// buffer, and each one writes a line in more than one call. So two lines
/// splice together, and the cut lands inside a method name.
///
/// Take this around the print itself:
///
///     MONO_LOCK (jit_trace_mutex ())
///     {
///             fprintf (stderr, "[llvm-jit] ...");
///     }
///
/// Build the line's arguments before you take this lock.
/// mono_method_full_name () takes metadata locks of its own. Format a name
/// under this one, and the trace lock goes above those in the lock order.
/// A thread that traces while it holds a metadata lock then deadlocks.
std::mutex &jit_trace_mutex ();

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
/// the point of it being a variable at all. Zero there leaves every tier-0
/// method interpreted for good. That is what separates the tier-0 entry path
/// from promotion when one of them misbehaves.
uint32_t tier1_threshold ();

/// How many methods a tier-1 promotion compile can take at once.
///
/// Methods promoted close together are translated into one module and compiled
/// together, which pays LLVM's per-compile cost once instead of once each.
/// MONO_LLVM_JIT_BATCH moves it, and one there compiles every method on its own.
/// Tier 2 is never batched: its code is laid out by its own method's counts.
uint32_t promotion_batch_size ();

/// The most threads the compile queue can run promotions on at once.
///
/// MONO_LLVM_JIT_WORKERS moves it, and one there puts every background compile
/// back on a single thread, which is what separates a bug in a compile from a
/// bug in two compiles overlapping. The queue starts threads only as work
/// outruns the ones it has, so this is a ceiling and not a count of threads a
/// process will have.
uint32_t compile_worker_count ();

/// How long a compile worker waits for work before the queue retires it.
///
/// MONO_LLVM_JIT_WORKER_IDLE_MS moves it. Zero there keeps every thread the
/// queue started for as long as the runtime lives, which is what separates the
/// cost of retiring threads from the cost of holding them.
std::chrono::milliseconds compile_worker_idle_timeout ();

/// Whether tier 2 exists at all, which is what decides whether a tier-1 body
/// carries profiling instrumentation.
///
/// On unless MONO_LLVM_JIT_TIER2 turns it off.
bool tier2_enabled ();

/// Whether MONO_LLVM_JIT_FOLD_CASTS left the type-test fold on.
///
/// A false value leaves every cast for the lowering to write as the probe and
/// the wrapper, which is what separates a wrong answer from a wrong probe. The
/// translator writes the same IR either way, so the two arms differ in one pass.
bool fold_casts ();

/// Whether MONO_LLVM_JIT_VTABLE_SNAPSHOT left the vtable constant on.
///
/// A false value leaves every class's vtable an external symbol, so a read of
/// the class word or the rank stays a load whatever the IR says the receiver
/// is. The translator writes the same reads either way, so the two arms differ
/// in what one global carries.
bool vtable_snapshots ();

/// The fast-math flags the float operations a method asks for carry.
///
/// Empty unless --ffast-math is on the command line, which is the only way to
/// leave IEC 60559 here. It never sets nnan or ninf, whatever the rest of the
/// set is, so a NaN or an infinity a computation produces is still the value
/// the program reads.
///
/// A method under these flags answers differently before and after it promotes,
/// because the interpreter relaxes nothing.
llvm::FastMathFlags relaxed_float_flags ();

/// How much a tier-1 body spends before it asks to be compiled again.
///
/// One unit is one instruction that emits code, counted as the body runs it. A
/// call costs tier2_entry_weight () on top, so the turns of a loop and the number
/// of calls both reach this threshold. The counter is signed and this answer
/// never goes past INT64_MAX.
///
/// Zero for a body that never asks on a count of any kind, which leaves it
/// instrumented and counting while something else decides when it promotes.
/// MONO_LLVM_JIT_TIER2_THRESHOLD moves it, and the default is a hundred million.
uint64_t tier2_threshold ();

/// What one call adds to the count tier2_threshold () bounds.
///
/// It is the exchange rate between how hot a method is and how much it does, in
/// the same units: a body that does nothing promotes after
/// tier2_threshold () / tier2_entry_weight () calls. Zero counts work alone,
/// which separates a promotion the work asked for from one the calls asked for.
///
/// MONO_LLVM_JIT_TIER2_ENTRY_WEIGHT moves it, and the default is five thousand.
uint64_t tier2_entry_weight ();

/// The largest callee, in IL bytes, a compile folds into its caller before any
/// cost model has looked at it.
///
/// Thirty-two, which leaves room to spare on the shapes the pre-pass
/// recognizes. MONO_LLVM_JIT_INLINE_IL_LIMIT moves it. Zero there turns the
/// pre-pass off, which is what separates a bug in a folded body from one in
/// the method that folded it.
uint32_t trivial_inline_il_limit ();

/// How many bodies the shape-test pre-pass can fold into one method.
///
/// MONO_LLVM_JIT_INLINE_BUDGET moves it. A chain of forwarders is what spends
/// it. A batch gives each member its own count, so a method folds in the same
/// bodies however many others promoted beside it.
uint32_t trivial_inline_budget ();

/// How many bodies the tier-2 cost model can fold into one method.
///
/// MONO_LLVM_JIT_INLINE_COST_BUDGET moves it. A count of its own rather than
/// the pre-pass's, so what one inliner takes in does not decide what the other
/// one is left to fold.
uint32_t costed_inline_budget ();

/// The largest callee, in IL bytes, the tier-2 cost model will translate in
/// order to weigh it.
///
/// MONO_LLVM_JIT_INLINE_COST_IL_LIMIT moves it, and zero there leaves tier 2
/// with the shape-test pre-pass alone - which is what separates a bug in the
/// cost model from one in what it folded.
uint32_t costed_inline_il_limit ();

/// How many folds deep past a method the tier-2 inliner can go.
///
/// MONO_LLVM_JIT_INLINE_DEPTH moves it. A call graph with a cycle in it never
/// runs out of sites, so the loop needs a floor whatever the budget says.
uint32_t inline_depth_limit ();

/// How many folds deep past root the shape-test pre-pass can go.
///
/// MONO_LLVM_JIT_INLINE_PREPASS_DEPTH moves it. A bound of its own rather than
/// the cost model's, because the two take different candidates: this one takes
/// a forwarder, and a chain of them reaches much further for the same budget.
///
/// The pre-pass drains its worklist least deep first, so this decides where the
/// count left over goes rather than what the first folds are.
uint32_t trivial_inline_depth_limit ();

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

/**
 * \file
 * \brief What this backend tells the inline cost model about a site.
 *
 * `inline-cost.cpp` is a copy of LLVM's model, and it stays one. It calls into
 * here for what mono knows and the model cannot see, so the copy holds no
 * policy of its own.
 */

#ifndef MONO_LLVM_PASSES_INLINE_POLICY_HPP
#define MONO_LLVM_PASSES_INLINE_POLICY_HPP

#include <llvm/ADT/STLFunctionalExtras.h>
#include <llvm/ADT/StringRef.h>

#include <optional>

namespace llvm {
class BasicBlock;
class BlockFrequencyInfo;
class BranchInst;
class CallBase;
class Function;
class LoadInst;
class Value;
} // namespace llvm

namespace mono {

/// How often a call site runs, against the body it sits in.
enum class SiteHeat { cold, ordinary, hot };

/// How hot \p call runs in the promoted body around it, or nothing.
///
/// A tier-2 compile holds one promoted body, so the module's profile summary is
/// built from that body's own counters. Its percentiles then land on that
/// body's own count levels, and it cannot rank a site against the rest of the
/// program. That is the question LLVM asks it.
///
/// The answer here is against the caller's entry count instead. A block the
/// body hardly ever takes is cold. A block that runs far more often than the
/// body is entered is hot. Where no block of the body runs much more often,
/// every block the body runs each time it is entered is hot instead. The rest
/// is ordinary.
///
/// Nothing comes back for a caller that carries no tier-2 counter, for one
/// whose profile counts were dropped, and where \p caller_bfi is null. LLVM's
/// own ranking decides those.
std::optional<SiteHeat> tier2_site_heat (const llvm::CallBase &call,
                                         llvm::BlockFrequencyInfo *caller_bfi);

/// Reports what a walk settled a value to, or null. The answer may belong to
/// the function the call site is in rather than to the one being walked.
using SettledValue = llvm::function_ref<llvm::Value *(llvm::Value *)>;

/// Whether a call to \p f is one load rather than a call.
///
/// A caller that prices such a site as a call charges it a call penalty and an
/// argument setup for work that is one instruction.
bool lowers_to_a_load (const llvm::Function &f);

/// Marks a function whose front end links its frame onto the thread's LMF
/// chain (`method->save_lmf`). No value. Presence is the fact.
constexpr llvm::StringRef save_lmf_attribute = "mono-save-lmf";

/// The vtable \p load reads, where the call site settles which object it reads
/// off, or null.
///
/// So a caller gets the receiver's vtable, which an IR pointer does not carry.
/// Null covers a load that is not a read of an object's vtable word.
llvm::Value *folded_object_vtable (llvm::LoadInst &load, SettledValue settled);

/// What a read off a vtable gives, where the call site settles which vtable is
/// read, or null.
///
/// So a caller gets the receiver's class and its `System.Type` object, neither
/// of which an IR pointer carries.
llvm::Value *folded_vtable_read (llvm::CallBase &call, SettledValue settled);

/// What a type test answers, where the class the call site settled its operand
/// to decides it, or null.
///
/// A test the operand's own function could settle is already answered, so what
/// is left is the one a caller decides. Null covers a test that fails for
/// certain and raises rather than answering, which is a throw and not a value.
llvm::Value *folded_type_test (llvm::CallBase &call, SettledValue settled);

/// The successor of \p branch that a run reaches, or null if \p branch is not
/// a null check that codegen folds.
///
/// A caller can walk that successor alone and leave the raising arm uncounted.
llvm::BasicBlock *implicit_null_check_successor (const llvm::BranchInst &branch);

/// What to add to the threshold a call site is weighed against, in the units
/// the model costs an instruction in.
///
/// Each bonus estimates the work a fold removes from \p callee once its body
/// can see the caller's own values. LLVM's model cannot estimate it: resolving
/// a dispatch needs the operand's class, and an IR pointer carries none.
///
/// Zero for a site no bonus recognizes.
int call_site_bonus (const llvm::CallBase &call, const llvm::Function &callee);

/// What to add to \p callee's cost for the frame `emit_push_lmf ()` pushes, or
/// zero.
///
/// The push and the pop are ordinary instructions once translated.
/// `getInlineCost ()` prices each one at the going per-instruction rate.
/// Nothing prices the callee-saved registers the clobber forces the caller to
/// spill.
int save_lmf_cost (const llvm::Function &callee);

} // namespace mono

#endif

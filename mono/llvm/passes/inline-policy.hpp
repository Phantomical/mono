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

namespace llvm {
class BasicBlock;
class BranchInst;
class CallBase;
class Function;
class LoadInst;
class Value;
} // namespace llvm

namespace mono {

/// What a walk settled a value to, or null. The answer may belong to the
/// function the call site is in rather than to the one being walked.
using SettledValue = llvm::function_ref<llvm::Value *(llvm::Value *)>;

/// Whether a call to \p f is one load rather than a call.
///
/// A caller that prices such a site as a call charges it a call penalty and an
/// argument setup for work that is one instruction.
bool lowers_to_a_load (const llvm::Function &f);

/// What \p load reads, where the call site settles it, or null.
///
/// Answers a read of a managed object's vtable word and a read of the fields
/// and dispatch slots a vtable snapshot states. So a caller gets the class, the
/// `System.Type` object and the dispatch target the receiver's own class
/// settles, none of which an IR pointer carries.
///
/// A shape this cannot place is left alone rather than reported. The base is
/// reached by inference, so an offset outside what a snapshot states says the
/// inference was wrong rather than that generated code is.
llvm::Value *folded_vtable_read (llvm::LoadInst &load, SettledValue settled);

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

} // namespace mono

#endif

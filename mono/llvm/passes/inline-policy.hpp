/**
 * \file
 * \brief What this backend tells the inline cost model about a site.
 *
 * `inline-cost.cpp` is a copy of LLVM's model, and it stays one. What mono
 * knows that the model does not is asked for from here, so the copy carries the
 * calls and none of the policy.
 */

#ifndef MONO_LLVM_PASSES_INLINE_POLICY_HPP
#define MONO_LLVM_PASSES_INLINE_POLICY_HPP

namespace llvm {
class BasicBlock;
class BranchInst;
class CallBase;
class Function;
} // namespace llvm

namespace mono {

/// The successor of \p branch that a run reaches, or null where \p branch is
/// not a null check codegen folds.
///
/// A caller can walk the answer alone and leave the other arm uncounted.
llvm::BasicBlock *implicit_null_check_successor (const llvm::BranchInst &branch);

/// What to add to the threshold a call site is weighed against, in the units
/// the model costs an instruction in.
///
/// Each part answers the same question: how much of \p callee stops being work
/// once its body sits beside the caller's own values. The model cannot see any
/// of it, because what settles a dispatch here is a class, and an IR pointer
/// carries none.
///
/// Zero for a site none of the parts recognize, which is most of them.
int call_site_bonus (const llvm::CallBase &call, const llvm::Function &callee);

} // namespace mono

#endif

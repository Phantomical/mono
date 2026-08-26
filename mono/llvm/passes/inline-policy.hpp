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

namespace llvm {
class BasicBlock;
class BranchInst;
class CallBase;
class Function;
} // namespace llvm

namespace mono {

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

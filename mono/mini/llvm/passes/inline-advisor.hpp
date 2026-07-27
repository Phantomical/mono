/**
 * \file
 * \brief mono's inline advisor - the tier-1 cost policy layered over LLVM's.
 *
 * LLVM's inline cost model is tuned for IR a C compiler produces, where a call
 * in the callee is a call the program is expected to make. Managed IR is not
 * shaped like that: every dereference carries a null check and every array
 * access a bounds check, each branching to a block whose sole content is a call
 * to the throw trampoline. The cost model charges ~35 for each of those calls -
 * the same as any other call - so a method with half a dozen checks has spent
 * the whole -O2 budget of 225 before a single instruction of its actual work has
 * been costed. Those blocks never run.
 *
 * The advisor credits them back, per callee, and leaves every other part of the
 * decision to LLVM.
 */

#ifndef MONO_MINI_LLVM_INLINE_ADVISOR_HPP
#define MONO_MINI_LLVM_INLINE_ADVISOR_HPP

#include <llvm/IR/PassManager.h>

namespace mono {

/*
 * Install mono's advisor as the one MAM's inliner will ask. Call once per
 * compile, before the pipeline runs.
 *
 * Registration flips a process-wide flag inside LLVM, so every inliner run in
 * the process ends up on this path - which is what we want, and is safe only
 * because every compile registers it. The advisor object itself is built per
 * module, so concurrent compiles share nothing.
 */
void register_mono_inline_advisor (llvm::ModuleAnalysisManager &mam);

} // namespace mono

#endif /* MONO_MINI_LLVM_INLINE_ADVISOR_HPP */

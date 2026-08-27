/**
 * \file
 * \brief Answering a delegate's Invoke with the method the delegate calls.
 *
 * Invoke has no body, so the site reads its entry off the object instead
 * (`delegate_invoke_callee ()`). Where the translator could say which method a
 * delegate was built over, the site can enter that method and the read goes
 * away.
 *
 * Two producers say it, and they differ in whether the object is known. A read
 * of an initonly static names the object, so every delegate reaching the site
 * is that one and the call becomes a direct call. A merge with a construction
 * on one arm names a candidate only, so the call becomes a compare against the
 * delegate's own entry with the direct call on the arm that matches.
 */

#ifndef MONO_LLVM_PASSES_FOLD_DELEGATE_HPP
#define MONO_LLVM_PASSES_FOLD_DELEGATE_HPP

#include <llvm/IR/PassManager.h>

namespace llvm {
class Function;
class Value;
} // namespace llvm

typedef struct _MonoMethod MonoMethod;

namespace mono {

/// What the IR says about the delegate arriving at a site.
struct DelegateTarget {
	/// The method named, or null where no arm named one or two arms disagreed.
	MonoMethod *method = nullptr;

	/// Whether every delegate that can arrive calls that method. False with a
	/// method means it is a candidate to compare against, not the answer.
	bool settled = false;
};

/// What \p receiver says about the delegate it holds.
///
/// Walks merges, so it reads a value the translator marked however many phis
/// and selects stand between. The walk is bounded by a node budget and answers
/// nothing at all once it runs out, rather than answering from the part it
/// reached.
DelegateTarget delegate_target_at (const llvm::Value *receiver);

/// Enters the delegate's target at each Invoke in \p f the IR names one for: a
/// settled target directly, a candidate behind a compare against the delegate's
/// own entry, with the original dispatch on the arm that does not match.
///
/// Tier 2 only, and behind the pass that reads the profile. Two things put it
/// there. A guard is blocks the CFG tier 1 hashed does not have. And the fold
/// needs current_compile () to name a method at all, which a tier-1 compile has
/// only when its batch holds one method - so at tier 1 whether a site folds
/// turns on how many methods promoted together rather than on the IR, and a
/// tier 2 that folded where tier 1 could not loses the counts.
class FoldDelegateInvokesPass : public llvm::PassInfoMixin<FoldDelegateInvokesPass> {
public:
	llvm::PreservedAnalyses run (llvm::Function &f, llvm::FunctionAnalysisManager &fam);
};

} // namespace mono

#endif

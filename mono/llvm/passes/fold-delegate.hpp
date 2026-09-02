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
class CallBase;
class Function;
class LoadInst;
class Value;
} // namespace llvm

typedef struct _MonoMethod MonoMethod;

namespace mono {

/// Marks \p load as a read of `MonoDelegate::method_ptr` that does not change.
///
/// `mono_delegate_ctor ()` sets the field once, before anything else can see
/// the delegate: to the bound method's entry, or to null for one that combines
/// others. Nothing writes it again - an `ldvirtftn` delegate never writes back
/// the override it later resolves, and `Combine` builds a new object rather
/// than mutating one. `invoke_impl` sits next to it in the same struct
/// and does not take this mark - a trampoline patches that field in place on
/// the delegate's first dispatch.
///
/// The write happens inside `mono_delegate_ctor ()`, opaque to this compile, so
/// there is no store to carry the matching tag the way
/// `mark_object_vtable_read ()` needs one. Two tagged reads of the same
/// pointer still equate to each other across whatever runs between them.
void mark_delegate_method_ptr_read (llvm::LoadInst *load);

class ConstantValues;

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
/// Reads a value the translator marked through whatever phis, selects and field
/// copies stand between, which \p values is what settles.
DelegateTarget delegate_target_at (llvm::Value *receiver,
                                  const ConstantValues &values);

/// Whether \p site reads its callee out of the delegate it passes.
///
/// `delegate_invoke_callee ()` (`method-to-llvm/call.cpp`) writes every Invoke as
/// one shape, and this is that shape read back:
///
///     %impl   = load ptr, ptr (getelementptr i8, %d, invoke_impl)
///     %isnull = icmp eq ptr %impl, null
///     %callee = select i1 %isnull, %dispatch, %impl
///     call %callee (%d, ...)
///
/// The delegate the site passes has to be the object the load reads, which is
/// what separates this from any other call through a selected pointer.
///
/// Reading the shape rather than a mark on the call is what still recognizes a
/// site an inliner moved. Metadata does not survive a transform that builds a
/// new instruction, and inlining a body into a try does exactly that: it writes
/// each call again as an invoke of the caller's pad.
bool reads_callee_off_delegate (const llvm::CallBase &site);

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

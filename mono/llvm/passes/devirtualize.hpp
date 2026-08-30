/**
 * \file
 * \brief Answering a dispatch site from the class of the receiver that reaches
 * it.
 *
 * The two operands of a dispatch site name the receiver's vtable and the slot.
 * Where the optimizer has made both constant - which the store at an allocation
 * is what produces - the entry the site reads is a method this compile can name,
 * and the call becomes a direct one.
 *
 * An array receiver never gets there, because the class its slot is declared
 * with is a bound rather than an identity. The guard below takes that site on
 * the same class behind a compare of the receiver's vtable, so the direct call
 * runs on the arm that proves the bound was the class.
 */

#ifndef MONO_LLVM_PASSES_DEVIRTUALIZE_HPP
#define MONO_LLVM_PASSES_DEVIRTUALIZE_HPP

#include <llvm/IR/PassManager.h>

#include <cstdint>

namespace llvm {
class Function;
}

typedef struct _MonoClass MonoClass;
typedef struct _MonoMethod MonoMethod;

namespace mono {

class ConstantValues;

/// The method in slot \p index of \p klass's vtable that a caller can name
/// directly, or null where it cannot name what stands there.
///
/// Null covers a slot with no method, one whose method is abstract, generic or
/// implemented outside IL, and one whose entry needs a context. A synchronized
/// method comes back as its wrapper, which is what the runtime puts in the slot.
MonoMethod *slot_target (MonoClass *klass, int32_t index);

/// Replaces each dispatch site in \p f whose class and slot are settled with
/// the entry it stands for. Says whether it changed anything.
///
/// What it needs of the running compile it reads from current_compile ()
/// (compile-state.hpp), and it asks mono for the rest. Outside a compile it
/// leaves every site alone.
bool fold_dispatch_sites (llvm::Function &f, llvm::FunctionAnalysisManager &fam);

/// Sends each dispatch on an array receiver through a compare of that
/// receiver's vtable against the array class its slot is declared with. The
/// arm that matches calls that class's implementation directly and the arm
/// that does not keeps the dispatch the site already had.
///
/// The compare is what makes the speculation safe. An array slot admits every
/// array of its rank with the same cast class, and each of those carries a
/// vtable of its own, so the arm the compare picks proves which one arrived.
/// Which slots are taken is `guardable_array ()`'s rule.
///
/// Tier 2 only, and behind the pass that reads the profile. A guard adds
/// blocks the CFG tier 1 hashed does not have. It runs again between the
/// inliner's rounds, because a folded body brings dispatches on the caller's
/// own array with it, and it marks each dispatch it has taken so that a later
/// run leaves the arm the compare did not pick alone.
class GuardDispatchPass : public llvm::PassInfoMixin<GuardDispatchPass> {
public:
	llvm::PreservedAnalyses run (llvm::Function &f, llvm::FunctionAnalysisManager &fam);
};

} // namespace mono

#endif

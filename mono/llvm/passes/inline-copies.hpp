/**
 * \file
 * \brief The bodies an inliner translates in beside a caller, and the sweep
 * that takes back the ones it did not fold in.
 */

#ifndef MONO_LLVM_PASSES_INLINE_COPIES_HPP
#define MONO_LLVM_PASSES_INLINE_COPIES_HPP

#include <llvm/ADT/StringRef.h>
#include <llvm/IR/PassManager.h>
#include <llvm/Support/Error.h>

namespace llvm {
class Function;
} // namespace llvm

namespace mono {

/// Marks a body an inliner translated in beside its caller, and carries the
/// symbol the engine publishes that method's entry under.
///
/// The name goes in the attribute rather than in the function's own name for
/// two reasons. The translator declares a method under a placeholder of its own
/// and finds it again by that name, so a copy renamed early is declared a second
/// time by the next translation that calls it. And the mangling belongs to the
/// engine, which a pass here cannot ask.
constexpr llvm::StringRef inline_copy_attribute = "mono-inline-copy";

/// Names a freshly materialized body as the copy of the method published at
/// \p published_name, and gives it the linkage and the marks a copy carries.
///
/// The caller adds `alwaysinline` when the copy is one the pipeline has to fold
/// in rather than one a cost model still has to weigh.
void mark_inline_copy (llvm::Function &copy, llvm::StringRef published_name);

/// Turns every inline copy the module still defines back into a declaration of
/// the method's published entry.
///
/// A copy sits under no thunk and has no jit info of its own. So a call left
/// standing to one enters code a stack walk cannot name and a detour cannot
/// reach. Deleting the body puts that call back on the callee's thunk, which is
/// where it was before an inliner asked for the body. That is what lets a cost
/// model translate a candidate, weigh it and refuse it without owing a cleanup.
///
/// Run it behind every inliner. What it leaves is what the rest of the pipeline
/// and codegen see.
class StripInlineCopiesPass : public llvm::PassInfoMixin<StripInlineCopiesPass> {
public:
	llvm::PreservedAnalyses run (llvm::Module &m, llvm::ModuleAnalysisManager &mam);
};

/// Answers an error naming an inline copy the module still defines.
llvm::Error inline_copies_stripped (const llvm::Module &m);

} // namespace mono

#endif

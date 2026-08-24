/**
 * \file
 * \brief Keeping the null checks of a method with EH clauses explicit.
 *
 * The translator tags every null check with !make.implicit, which asks LLVM's
 * ImplicitNullChecks pass to fold the test into the dereference behind it. Such
 * a fold raises the exception at the dereference, and `.mono_lsda` carries one
 * protected range for each invoke (passes/eh-gather.cpp), so the ranges cover
 * the calls and not the code between them. A fault at the dereference is
 * outside all of them, and the exception unwinds past the clause that protects
 * the site.
 *
 * This pass takes the tag off a method that has clauses. It runs behind both
 * inliners, because a folded copy carries the tags of its own body into
 * whichever method took it in.
 */

#ifndef MONO_LLVM_PASSES_PROTECTED_NULL_CHECKS_HPP
#define MONO_LLVM_PASSES_PROTECTED_NULL_CHECKS_HPP

#include <llvm/ADT/StringRef.h>
#include <llvm/IR/PassManager.h>

namespace mono {

/// The tag LLVM's ImplicitNullChecks pass looks for on a branch. The translator
/// puts it on every null check it emits.
constexpr llvm::StringRef make_implicit_metadata = "make.implicit";

class ProtectedNullChecksPass : public llvm::PassInfoMixin<ProtectedNullChecksPass> {
public:
	llvm::PreservedAnalyses run (llvm::Function &f,
	                             llvm::FunctionAnalysisManager &fam);
};

} // namespace mono

#endif

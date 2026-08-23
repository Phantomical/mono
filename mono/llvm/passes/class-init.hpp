/**
 * \file
 * \brief Dropping class-init checks a dominating check already made.
 *
 * The translator cannot see far enough to know which class-init call comes
 * first, so it emits one at every point the CIL requires:
 *
 * - a method's own entry
 * - every static field access
 * - every newobj of a class with a cctor
 *
 * This pass, which can, deletes the rest.
 */

#ifndef MONO_LLVM_PASSES_CLASS_INIT_HPP
#define MONO_LLVM_PASSES_CLASS_INIT_HPP

#include <llvm/ADT/StringRef.h>
#include <llvm/IR/PassManager.h>

namespace mono {

/// Marks a declaration whose calls run a class's static constructor and
/// return once it has run. The call's own argument names the class. The
/// translator puts this on the one icall wrapper it calls for that, and
/// no other code sets it.
constexpr llvm::StringRef class_init_attribute = "mono-class-init";

class ClassInitPass : public llvm::PassInfoMixin<ClassInitPass> {
public:
	llvm::PreservedAnalyses run (llvm::Function &f,
	                             llvm::FunctionAnalysisManager &fam);
};

} // namespace mono

#endif

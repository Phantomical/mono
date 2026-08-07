/**
 * \file
 * \brief Dropping class-init checks a dominating check already made.
 *
 * The translator emits a class-initialization call everywhere the CIL asks for
 * one - a method's own entry, every static field access, every newobj of a
 * class with a cctor - because it cannot see far enough to know which of them
 * are the first. This pass, which can, deletes the rest.
 */

#ifndef MONO_LLVM_PASSES_CLASS_INIT_HPP
#define MONO_LLVM_PASSES_CLASS_INIT_HPP

#include <llvm/ADT/StringRef.h>
#include <llvm/IR/PassManager.h>

namespace mono {

/// Marks a declaration whose calls run a class's static constructor and return
/// once it has run, the class named by the call's own argument. The translator
/// puts it on the one icall wrapper it calls for that; nothing else does.
constexpr llvm::StringRef class_init_attribute = "mono-class-init";

/// Deletes every call to a `mono-class-init` declaration that another call for
/// the same class dominates.
class ClassInitPass : public llvm::PassInfoMixin<ClassInitPass> {
public:
	llvm::PreservedAnalyses run (llvm::Function &f,
	                             llvm::FunctionAnalysisManager &fam);
};

} // namespace mono

#endif

/**
 * \file
 * \brief Pass to remove unnecessary class-init calls.
 *
 * The translator unconditionally emits class init checks all over the place.
 * Once profiling data has been gathered this pass is used to drop any class
 * init call that is either dominated or known to be unnecessary.
 */

#ifndef MONO_LLVM_PASSES_CLASS_INIT_ELISION_HPP
#define MONO_LLVM_PASSES_CLASS_INIT_ELISION_HPP

#include <llvm/ADT/StringRef.h>
#include <llvm/IR/PassManager.h>

typedef struct _MonoClass MonoClass;
typedef struct _MonoDomain MonoDomain;

namespace mono {

constexpr llvm::StringRef class_init_attribute = "mono-class-init";

/// Has \p klass been fully initialized in \p domain yet?
bool is_class_initialized (MonoDomain *domain, MonoClass *klass);

/// Remove class init calls for classes which are already initialized.
class ClassInitCompleteElisionPass : public llvm::PassInfoMixin<ClassInitCompleteElisionPass> {
public:
	llvm::PreservedAnalyses run (llvm::Function &f, llvm::FunctionAnalysisManager &fam);
};

/// Remove class init calls for classes which are dominated by other class init
/// calls.
class ClassInitDominatedElisionPass : public llvm::PassInfoMixin<ClassInitDominatedElisionPass> {
public:
	llvm::PreservedAnalyses run (llvm::Function &f, llvm::FunctionAnalysisManager &fam);
};

} // namespace mono

#endif

/**
 * \file
 * \brief Dropping a class-init check the running domain has already answered.
 *
 * `method-to-llvm/fields.cpp` no longer asks whether a class has already run
 * its cctor: it lowers every static-field access the same way whether it has
 * or not, so that two translations of the same method - tier 1's and tier
 * 2's own, made at whatever points in the program each happens to run at -
 * hash the same CFG. This pass is where the question the front end no longer
 * asks gets asked instead, on the far side of both tiers' hash.
 */

#ifndef MONO_LLVM_PASSES_CLASS_INIT_WARM_HPP
#define MONO_LLVM_PASSES_CLASS_INIT_WARM_HPP

#include <llvm/IR/PassManager.h>

typedef struct _MonoClass MonoClass;
typedef struct _MonoDomain MonoDomain;

namespace mono {

/// Whether klass's static constructor has run in domain, so a check on it -
/// wherever the front end left one standing - answers the same as no check at
/// all. False also covers a klass this compile cannot yet name a vtable for.
bool class_is_initialized (MonoDomain *domain, MonoClass *klass);

class ClassInitWarmPass : public llvm::PassInfoMixin<ClassInitWarmPass> {
public:
	llvm::PreservedAnalyses run (llvm::Function &f, llvm::FunctionAnalysisManager &fam);
};

} // namespace mono

#endif

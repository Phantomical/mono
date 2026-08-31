/**
 * \file
 * \brief Reading a warm initonly static's own value, once nothing further
 * downstream can ask what CFG either tier hashed it under.
 *
 * `push_guarded_static_read ()` never bakes a value in: the class may still be
 * behind its own initializer when the front end runs, so every eligible read
 * goes through the runtime, twice - once through `ClassInitWarmPass`'s
 * concern, the flag, and once here, through the value itself. Both ask the
 * same question this late for the same reason: a translation cannot answer it
 * once and have that answer count for every later translation of the same
 * method, without the two tiers hashing different CFGs for it.
 */

#ifndef MONO_LLVM_PASSES_STATIC_CONST_FOLD_HPP
#define MONO_LLVM_PASSES_STATIC_CONST_FOLD_HPP

#include <llvm/IR/PassManager.h>

namespace mono {

class StaticConstFoldPass : public llvm::PassInfoMixin<StaticConstFoldPass> {
public:
	llvm::PreservedAnalyses run (llvm::Function &f, llvm::FunctionAnalysisManager &fam);
};

} // namespace mono

#endif

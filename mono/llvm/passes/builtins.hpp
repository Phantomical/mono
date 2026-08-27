/**
 * \file
 * \brief The two passes that act on the declarations the front end leaves
 * standing, and the helpers a new one is written with.
 *
 * A site the front end cannot expand is written as a call to a declaration
 * whose name says what the site means. `MonoBuiltinConstProp` folds such a site
 * where the IR settles it, and `MonoBuiltinLower` writes back the IR every site
 * that is left stands for.
 */

#ifndef MONO_LLVM_PASSES_BUILTINS_HPP
#define MONO_LLVM_PASSES_BUILTINS_HPP

#include <llvm/ADT/SmallVector.h>
#include <llvm/ADT/StringRef.h>
#include <llvm/IR/PassManager.h>

namespace llvm {
class CallBase;
class Function;
class FunctionType;
class Module;
} // namespace llvm

namespace mono {

/// The declaration \p name has in \p m, created on first use with \p shape.
///
/// The caller puts on what comes back the attributes its own sites carry.
llvm::Function *builtin_decl (llvm::Module &m, llvm::StringRef name,
                              llvm::FunctionType *shape);

/// Every call of the declaration \p name has in \p m, or none where the module
/// has no such declaration.
///
/// The result is a snapshot, so a caller can erase what it rewrites.
llvm::SmallVector<llvm::CallBase *, 8> builtin_sites (llvm::Module &m, llvm::StringRef name);

/// Every call of it inside \p f alone, which is what a function pass folds.
llvm::SmallVector<llvm::CallBase *, 8> builtin_sites (llvm::Function &f, llvm::StringRef name);

/// Erases the declaration \p name has in \p m, and says whether it was there.
///
/// Fails the process on a use left standing, which is a use no lowering
/// understands.
bool erase_builtin (llvm::Module &m, llvm::StringRef name);

/// Where in a pipeline a family's lowering runs.
enum class LowerStage {
	/// In front of the simplification. A site nothing reads back only hides
	/// its own arithmetic from the optimizer.
	pre_simplification,

	/// Behind the simplification and in front of the PGO instrumentation. Both
	/// tiers lower here, so a body carries the same CFG into the hash whichever
	/// tier compiled it.
	pre_profile,

	/// Behind the inliners. A cost model then weighs a callee with its sites
	/// still one call each, and a fold reads what a caller brought in.
	post_inline,
};

/// Folds every builtin site in a function that the IR settles.
///
/// Runs at the peephole point, behind each round of the simplification that
/// settles an operand. That is in front of the round which drops the branches a
/// fold makes dead.
class MonoBuiltinConstProp : public llvm::PassInfoMixin<MonoBuiltinConstProp> {
public:
	llvm::PreservedAnalyses run (llvm::Function &f, llvm::FunctionAnalysisManager &fam);
};

/// Writes back the IR the sites lowered at one stage stand for, and erases
/// their declarations.
///
/// Codegen has no lowering for any of them, so every tier runs this at each
/// stage.
class MonoBuiltinLower : public llvm::PassInfoMixin<MonoBuiltinLower> {
public:
	explicit MonoBuiltinLower (LowerStage stage) : stage (stage) { }

	llvm::PreservedAnalyses run (llvm::Module &m, llvm::ModuleAnalysisManager &mam);

private:
	LowerStage stage;
};

} // namespace mono

#endif

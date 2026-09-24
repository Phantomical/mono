/**
 * \file
 * \brief `MonoBuiltinConstProp` and `MonoBuiltinLower`, the eliminations
 * behind them, and the helpers a new one is written with.
 *
 * A site the front end cannot expand is written as a call to a declaration
 * whose name says what the site means. `MonoBuiltinConstProp` eliminates such a
 * site where the IR settles it, and `MonoBuiltinLower` writes back the IR every
 * site that is left stands for. Each elimination below is a plain function
 * rather than a pass of its own, because more than one pass can want to call
 * it: `eliminate_delegate_invokes ()` is `EliminateDelegateAndGuardDispatchPass`'s,
 * not `MonoBuiltinConstProp`'s.
 */

#ifndef MONO_LLVM_PASSES_BUILTINS_HPP
#define MONO_LLVM_PASSES_BUILTINS_HPP

#include <llvm/ADT/SmallVector.h>
#include <llvm/ADT/StringRef.h>
#include <llvm/IR/PassManager.h>

namespace llvm {
class BlockFrequencyInfo;
class CallBase;
class Function;
class FunctionType;
class Module;
} // namespace llvm

namespace mono {

class ConstantValues;

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

/// Every call of it inside \p f alone, which is what a function pass eliminates.
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

	/// Behind the inliners, `GuardDispatchPass` and `eliminate_type_tests ()`,
	/// which read a vtable, a slot or a cast's answer straight off the call.
	/// A site none of them settled turns into a probe here.
	post_inline,

	/// Behind the optimization pipeline. A site that stands until here keeps the
	/// attributes only the front end can state, so the passes that decide
	/// whether an object is dead read them rather than an opaque call.
	///
	/// What lowers here has to answer for the code it writes, because no
	/// simplification runs behind it. An allocation becomes one call, and a
	/// barrier a compare and a byte store, so neither wants one.
	post_optimization,
};

/// Replaces each type test in \p f that the operand's own class decides with
/// the value it stands for. Says whether it changed anything.
bool eliminate_type_tests (llvm::Function &f, llvm::FunctionAnalysisManager &fam);

/// Replaces each System.Enum builtin site in \p f whose receiver is a
/// provably boxed enum with the arithmetic on its value that the site stands
/// for. Says whether it changed anything.
bool eliminate_enum_builtins (llvm::Function &f, llvm::FunctionAnalysisManager &fam);

/// Replaces each object vtable read in \p f whose class the IR settles with
/// that class's own vtable symbol. Says whether it changed anything.
///
/// A read stands under the null check on its object, and the declaration is not
/// speculatable, so nothing moves one above that check. That is what lets a
/// sealed slot's declared class stand for the class the object is. The null
/// such a slot also admits cannot reach the read.
bool eliminate_object_vtables (llvm::Function &f, llvm::FunctionAnalysisManager &fam);

/// Replaces each vtable field read in \p f whose vtable is a marked symbol with
/// the value that symbol carries. Says whether it changed anything.
bool eliminate_vtable_fields (llvm::Function &f, llvm::FunctionAnalysisManager &fam);

/// Replaces `MonoClass::element_class` loads from known class symbols with the
/// corresponding class symbol. Says whether it changed anything.
bool eliminate_element_class_reads (llvm::Function &f, llvm::FunctionAnalysisManager &fam);

/// Erases each write barrier in \p f whose destination the IR settles to an
/// alloca. Says whether it changed anything.
///
/// Both collectors scan a thread's frames conservatively, so a reference a frame
/// holds is found without a remembered-set entry.
bool eliminate_stack_barriers (llvm::Function &f);

/// Rewrites each value copy in \p f that the IR settles as safe in the open into
/// a memcpy or a memmove with the cards behind it. Says whether it changed
/// anything.
///
/// A copy the optimizer can read is what lets SROA scalarize a value type and
/// what lets the dead-allocation walk erase the object behind it. The site the
/// translator wrote hides all of that inside one call, because an open copy is
/// wrong where a copied reference lands somewhere no conservative scan reaches.
/// `gc_value_copy_name` (`passes/gc-barrier.hpp`) states that rule, and this is
/// the elimination that reads the IR against it.
bool open_value_copies (llvm::Function &f);

/// Enters the delegate's target at each Invoke in \p f the IR names one for: a
/// settled target directly, a candidate behind a compare against the delegate's
/// own entry, with the original dispatch on the arm that does not match.
///
/// Tier 2 only, and behind the pass that reads the profile. Two things put it
/// there. A guard is blocks the CFG tier 1 hashed does not have. And the
/// elimination needs current_compile () to name a method at all, which a tier-1
/// compile has only when its batch holds one method - so at tier 1 whether a
/// site eliminates turns on how many methods promoted together rather than on
/// the IR, and a tier 2 that eliminated where tier 1 could not loses the counts.
///
/// \p counts and \p values are read rather than fetched, so a caller running
/// this beside another pass that reads the same two can solve them once.
/// Says whether it changed anything.
bool eliminate_delegate_invokes (llvm::Function &f, llvm::BlockFrequencyInfo &counts,
                                 const ConstantValues &values);

/// Eliminates every builtin site in a function that the IR settles.
///
/// Runs at the peephole point, behind each round of the simplification that
/// settles an operand. That is in front of the round which drops the branches
/// an elimination makes dead.
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

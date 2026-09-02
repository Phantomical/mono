/**
 * \file
 * \brief Answering a type test whose operand's class the IR already gives.
 *
 * A cast site names the class it tests against. Where the IR also says what the
 * operand is - an allocation states its class, and a parameter states the class
 * its slot is declared with - the two together decide the test, and the site
 * becomes the operand or null.
 */

#ifndef MONO_LLVM_PASSES_FOLD_CAST_HPP
#define MONO_LLVM_PASSES_FOLD_CAST_HPP

#include <llvm/ADT/STLFunctionalExtras.h>
#include <llvm/IR/PassManager.h>

namespace llvm {
class Function;
class PHINode;
class Value;
} // namespace llvm

typedef struct _MonoClass MonoClass;

namespace mono {

class ConstantValues;

/// What a test of an operand against a class answers for every class the
/// operand can hold.
enum class CastAnswer { Unknown, Yes, No };

/// How a test against \p target comes out for an operand of class \p held.
///
/// \p exact says that held is the class the operand is. False makes it a bound:
/// the operand holds some class assignable to held, and an answer needs every
/// one of them to agree.
///
/// Unknown is always available and always right, so a shape this does not model
/// leaves the site alone.
CastAnswer cast_answer (MonoClass *target, MonoClass *held, bool exact);

/// Whether \p answer settles every one of \p phi's own incoming edges. Does
/// not require them to settle the same way.
bool isinst_settles_over_incoming (
	llvm::PHINode &phi, llvm::function_ref<CastAnswer (llvm::Value *)> answer);

/// Rebuilds an isinst answer one incoming edge of \p phi at a time, where
/// \p answer decides each edge on its own. Null where one edge is undecided.
llvm::Value *rebuild_isinst_over_incoming (
	llvm::PHINode &phi, llvm::function_ref<CastAnswer (llvm::Value *)> answer);

/// Replaces each type test in \p f that the operand's own class decides with
/// the value it stands for. Says whether it changed anything.
bool fold_type_tests (llvm::Function &f, llvm::FunctionAnalysisManager &fam);

} // namespace mono

#endif

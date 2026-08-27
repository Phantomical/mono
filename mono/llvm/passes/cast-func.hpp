/**
 * \file
 * \brief The declaration a type test is written as, and the probe it lowers to.
 *
 * A cast expanded into a cache probe and an icall says nothing a later pass can
 * act on. Written as a call it keeps the class the test names as an operand,
 * which is the form fold_type_tests () decides once the operand's own class is
 * settled.
 */

#ifndef MONO_LLVM_PASSES_CAST_FUNC_HPP
#define MONO_LLVM_PASSES_CAST_FUNC_HPP

#include <llvm/ADT/StringRef.h>

namespace llvm {
class Function;
class Module;
} // namespace llvm

namespace mono {

/*
 * Both declarations take the same four operands:
 *
 *   ptr @mono.cast.isinst (ptr obj, ptr class, ptr cache, ptr icall)
 *
 * class is the class the test names. It is a marked global for a class the
 * compile can name, and the value an rgctx fetch answered for one it cannot.
 * Only the first form gives a pass a class to read back.
 *
 * cache is the word the site's own answer is kept in, and icall is the wrapper
 * a miss falls back to. Both are carried rather than built by the lowering,
 * because a slot for an open class comes from the context and only the
 * translator can ask for one.
 *
 * Neither declaration is nounwind. The wrapper raises the class's own load
 * failure, and castclass raises InvalidCastException, so a site inside a clause
 * is an invoke and the lowering keeps that edge.
 */

/// Answers obj where obj is an instance of the class, and null where it is not.
constexpr llvm::StringRef cast_isinst_name = "mono.cast.isinst";

/// Answers obj where obj is an instance of the class, and raises
/// InvalidCastException where it is not.
constexpr llvm::StringRef cast_castclass_name = "mono.cast.castclass";

/// The declaration in \p m for one of the two forms, created on first use.
llvm::Function *cast_func_decl (llvm::Module &m, bool throw_on_fail);

/// Rewrites every cast call into the null check and the cache probe it stands
/// for, with a one-sided inline test in front of the probe wherever the target
/// class admits one, erases the declarations, and says whether it changed
/// anything.
bool lower_type_tests (llvm::Module &m);

} // namespace mono

#endif

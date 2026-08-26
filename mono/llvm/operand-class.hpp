/**
 * \file
 * \brief How a reference in the IR says which class stands behind it.
 *
 * An IR pointer carries no class. What the translator knew about a value is
 * therefore written beside it, and a pass reads it back after inlining has
 * brought the value and the use into one function.
 *
 * Two facts travel, and they are not the same strength. An allocation states
 * the class the object *is*. A parameter states only a bound: the class its
 * slot is declared with.
 *
 * Each is a MonoClass pointer written into metadata, the way a marked
 * declaration carries one in an attribute (`method-symbols.hpp`). So it names
 * no symbol and costs the link nothing. It is also why a reader has to be
 * inside the compile that wrote it: the pointer means nothing to a later
 * process reading a dumped module.
 */

#ifndef MONO_LLVM_OPERAND_CLASS_HPP
#define MONO_LLVM_OPERAND_CLASS_HPP

#include <llvm/ADT/ArrayRef.h>
#include <llvm/ADT/StringRef.h>

#include <utility>

namespace llvm {
class Function;
class Instruction;
class Value;
} // namespace llvm

typedef struct _MonoClass MonoClass;

namespace mono {

/// Names the class an allocation makes an instance of. It sits on the call that
/// makes the object, so a fold carries it along with the call.
constexpr llvm::StringRef alloc_class_md = "mono.alloc.class";

/// Names the class each of a function's reference parameters is declared with,
/// as pairs of the argument index and the class. Only the parameters this can
/// answer for are listed.
constexpr llvm::StringRef param_classes_md = "mono.param.classes";

/// Says that \p site makes an instance of \p klass.
void mark_allocated_class (llvm::Instruction &site, MonoClass *klass);

/// Says that \p f declares the listed arguments with the listed classes, each
/// pair an argument index and that argument's class.
void mark_parameter_classes (llvm::Function &f,
                             llvm::ArrayRef<std::pair<unsigned, MonoClass *>> classes);

/// What \p v's class is, and whether that is the class it is rather than a
/// bound on it. Answers a null class where the IR says nothing.
///
/// \p f must be the function v belongs to, because a parameter's class is
/// recorded there.
std::pair<MonoClass *, bool> operand_class (const llvm::Value *v, const llvm::Function &f);

} // namespace mono

#endif

/**
 * \file
 * \brief How the IR carries what the translator knew about a reference.
 *
 * An IR pointer carries no class. What the translator knew about a value is
 * therefore written beside it, and a pass reads it back after inlining has
 * brought the value and the use into one function.
 *
 * Three facts travel, and they are not the same strength. An allocation and a
 * read of an initonly static both state the class the object *is*. A parameter
 * states only a bound: the class its slot is declared with. A delegate either
 * of the first two produced also states the method it calls.
 *
 * Each is a host pointer written into metadata, the way a marked declaration
 * carries one in an attribute (`method-symbols.hpp`). So it names no symbol and
 * costs the link nothing. It is also why a reader has to be inside the compile
 * that wrote it: the pointer means nothing to a later process reading a dumped
 * module.
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
typedef struct _MonoMethod MonoMethod;

namespace mono {

/// Names the class a value is an instance of. It sits on the instruction that
/// produces the value, so a fold carries it along with that instruction.
constexpr llvm::StringRef exact_class_md = "mono.exact.class";

/// Names the class each of a function's reference parameters is declared with,
/// as pairs of the argument index and the class. Only the parameters this can
/// answer for are listed.
constexpr llvm::StringRef param_classes_md = "mono.param.classes";

/// Names the method a delegate value calls. It sits on the instruction that
/// produces the delegate, so a fold carries it along with that instruction.
constexpr llvm::StringRef delegate_target_md = "mono.delegate.target";


/// Says that \p site produces an instance of \p klass.
void mark_exact_class (llvm::Instruction &site, MonoClass *klass);

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

/// The class \p v is, or null where the IR gives only a bound this cannot
/// sharpen. \p f carries the same rule as above.
///
/// A sealed class admits itself alone, so a bound on one is the class the value
/// is. Two shapes are marked sealed and are not exact all the same. An array
/// is, because covariance puts `Derived[]` under a `Base[]` slot. So is a
/// marshal-by-ref class, because such a slot can hold a transparent proxy,
/// whose vtable is not the class's.
///
/// A bound holds for a null reference as well, which no class answers for. A
/// caller that reads memory off the value therefore needs the null check that
/// dominates it to rule that out.
MonoClass *exact_class (const llvm::Value *v, const llvm::Function &f);

/// Says that \p site produces a delegate whose target method is \p target.
///
/// Only where \p target is the method the delegate will really enter. An
/// `ldvirtftn` delegate resolves its target when it is called, so the method
/// named at its construction is not the one it runs.
void mark_delegate_target (llvm::Instruction &site, MonoMethod *target);

/// The method the delegate \p v calls, or null where the IR says nothing.
///
/// A null answer means "not stated here" rather than "not a delegate": only the
/// producers the translator could answer for carry the mark.
MonoMethod *delegate_target (const llvm::Value *v);


} // namespace mono

#endif

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
 * Three channels carry them, and a reader here takes any of them. What the
 * translator writes down is a host pointer in metadata, the way a marked
 * declaration carries one in an attribute (`method-symbols.hpp`). So it names
 * no symbol and costs the link nothing.
 *
 * A read of an initonly static instead says what it is by its shape, a load off
 * a marked statics block at a constant offset, and the reader asks mono what
 * the field holds. Metadata does not last on such a load: InstCombine folds the
 * address into the load's pointer operand and builds a new one, which drops it.
 * A global outlives every pass, so the shape is what survives.
 *
 * An allocation site names its class a third way: through the vtable operand
 * its declaration takes, itself a marked global. Metadata on the site is the
 * ordinary way to read it, but that metadata can go missing when the site
 * moves into a protected region and LLVM rewrites the call into an invoke
 * (`operand-class.cpp` says where). The operand is not metadata, so it
 * survives that rewrite and every other one.
 *
 * Either way a reader has to be inside the compile: a metadata pointer means
 * nothing to a later process reading a dumped module, and the field is read
 * against this compile's domain.
 */

#ifndef MONO_LLVM_ANALYSIS_OPERAND_CLASS_HPP
#define MONO_LLVM_ANALYSIS_OPERAND_CLASS_HPP

#include <llvm/ADT/ArrayRef.h>
#include <llvm/ADT/SmallVector.h>
#include <llvm/ADT/StringRef.h>

#include <utility>

namespace llvm {
class Function;
class Instruction;
class LoadInst;
class Value;
} // namespace llvm

typedef struct _MonoClass MonoClass;
typedef struct _MonoMethod MonoMethod;

namespace mono {

class ConstantValues;

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

/// Returns the class \p v states on its own, and whether that is the class it
/// is rather than a bound on it. Null where the IR says nothing.
///
/// Nothing in front of \p v is looked through. Use this where the merges are
/// already resolved for the value in hand, and `operand_class ()` below where
/// they are not.
///
/// \p f must be the function v belongs to, because a parameter's class is
/// recorded there.
std::pair<MonoClass *, bool> stated_class (const llvm::Value *v, const llvm::Function &f);

/// Returns \p v's class, and whether that is the class it is rather than a
/// bound on it. Null where the IR says nothing.
///
/// \p f must be the function v belongs to, because a parameter's class is
/// recorded there. \p values is what looks through the merges in front of \p v.
///
/// Every value reaching \p v has to name one class, and \p values has to have
/// found all of them. A null among them gives no class, so a field with exactly
/// one store gets none: the allocation's own zero still reaches the load.
/// `exact_class ()` below relaxes that null rule.
std::pair<MonoClass *, bool> operand_class (llvm::Value *v, const llvm::Function &f,
                                            const ConstantValues &values);

/// Returns the class \p v is, or null where the IR gives only a bound this
/// cannot sharpen. \p f and \p values carry the same rule as above.
///
/// A sealed class admits itself alone, so a bound on one is the class the value
/// is. Two shapes are marked sealed and are not exact all the same. An array
/// is, because covariance puts `Derived[]` under a `Base[]` slot. So is a
/// marshal-by-ref class, because such a slot can hold a transparent proxy,
/// whose vtable is not the class's.
///
/// Unlike `operand_class ()`, a null reaching \p v does not make the answer no
/// class. This assumes the caller dereferences \p v right after, so the path
/// carrying null faults before the use and the answer never has to cover it. A
/// caller that does not dereference \p v must call `operand_class ()` instead.
MonoClass *exact_class (llvm::Value *v, const llvm::Function &f, const ConstantValues &values);

/// A class \p v plausibly has, for a caller that compares against it before it
/// acts on the answer. Null where this walk names none.
///
/// The two answers above hold on every path. This one does not: the caller
/// writes a compare that sends the path this walk was wrong about to the code
/// that stood there before. So this reads the same channels with the proofs
/// that cover an unseen path dropped. A field whose object escapes still
/// answers from the stores this walk can see, and a merge still answers where
/// only some of its arms name a class.
///
/// What it does not answer from is a parameter's declared class. That is a
/// bound, and a compare against a bound misses every subclass, so a guard
/// written on one pays for itself nowhere. Every class this answers is one an
/// allocation or an initonly static states.
MonoClass *guessed_class (llvm::Value *v, const llvm::Function &f, const ConstantValues &values);

/// Says that \p site produces a delegate whose target method is \p target.
///
/// Only where \p target is the method the delegate will really enter. An
/// `ldvirtftn` delegate resolves its target when it is called, so the method
/// named at its construction is not the one it runs.
void mark_delegate_target (llvm::Instruction &site, MonoMethod *target);

/// The method the delegate \p v calls, or null where the IR says nothing.
///
/// A null answer means "not stated here" rather than "not a delegate": only the
/// producers the translator could answer for say a target at all.
MonoMethod *delegate_target (const llvm::Value *v);


} // namespace mono

#endif

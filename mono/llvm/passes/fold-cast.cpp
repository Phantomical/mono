/**
 * \file
 * \brief Deciding a type test from the two classes, and rewriting the site.
 *
 * The rule below is stated over the set of classes an operand can hold rather
 * than over one class, because what the IR gives for a parameter is the class
 * its slot is declared with. An answer needs every class the slot admits to
 * agree, and the arguments that a set agrees are what each arm carries.
 */

#include "fold-cast.hpp"

#include "analysis/operand-class.hpp"
#include "analysis/strip-casts.hpp"
#include "builtins.hpp"
#include "cast-func.hpp"
#include "compile-state.hpp"
#include "method-symbols.hpp"
#include "runtime/options.hpp"

#include "mono/metadata/class-init.h"
#include "mono/metadata/class-inlines.h"
#include "mono/metadata/class-internals.h"
#include "mono/metadata/metadata.h"

#include <llvm/ADT/SmallPtrSet.h>
#include <llvm/ADT/SmallVector.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/GlobalValue.h>
#include <llvm/IR/InstrTypes.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>

using namespace llvm;

namespace mono {
namespace {

/// Whether a class is one this rule reads at all.
///
/// A generic argument names no class while the body is shared, and a class the
/// runtime failed to load answers nothing - the site raises that failure, which
/// is the program's to see.
bool
readable (MonoClass *klass)
{
	MonoType *self = m_class_get_byval_arg (klass);

	if (self->type == MONO_TYPE_VAR || self->type == MONO_TYPE_MVAR)
		return false;

	mono_class_init_internal (klass);

	return !mono_class_has_failure (klass);
}

/**
 * Whether every array a slot of the array class \p held admits shares its rank
 * and its cast class.
 *
 * That is what makes held answer for the whole set: the array rule reads both
 * sides through those two, so two arrays that share them are assignable to and
 * from the same classes and implement the same interfaces.
 *
 * A value type element gives it, because `class_composite_fixup_cast_class ()`
 * (`mono/metadata/class-init.c`) folds the element onto one cast class and
 * assignability then compares that. A sealed reference element gives it because
 * the element admits itself alone.
 *
 * An array element does not, and is refused. `int[][]` admits `uint[][]`,
 * whose cast class is `uint[]` rather than `int[]`, so the set spreads over
 * more than one.
 */
bool
arrays_agree (MonoClass *held)
{
	MonoClass *element = m_class_get_cast_class (held);

	if (element == nullptr || m_class_get_rank (element) != 0)
		return false;

	return m_class_is_valuetype (element) || m_class_is_sealed (element);
}

} // namespace

CastAnswer
cast_answer (MonoClass *target, MonoClass *held, bool exact)
{
	if (target == nullptr || held == nullptr || !readable (target) || !readable (held))
		return CastAnswer::Unknown;

	// A transparent proxy answers for the class it stands in for rather than for
	// its own. mono_object_handle_isinst () (mono/metadata/object.c) sends a test
	// to that answer only where the target is marshal-by-ref or an interface. So
	// a slot declared with neither holds no proxy, and refusing both here is what
	// the rest of this rule stands on.
	if (mono_class_is_marshalbyref (target) || mono_class_is_marshalbyref (held))
		return CastAnswer::Unknown;

	// Assignability carries down: a class assignable to held is assignable to
	// anything held is assignable to. So this arm needs no set argument.
	if (mono_class_is_assignable_from_internal (target, held))
		return CastAnswer::Yes;

	if (exact)
		return CastAnswer::No;

	// An interface-typed slot admits any class that implements it, and a
	// delegate carries variance of its own, so neither says what its set holds.
	if (mono_class_is_interface (held) || m_class_is_delegate (held)
	    || m_class_is_valuetype (held))
		return CastAnswer::Unknown;

	// An array slot admits arrays alone, and covariance is what decides which. So
	// held answers for the set only where every array in it reads the same.
	// Nothing below applies: an array reaches a class it does not descend from,
	// which is what the single-inheritance argument rules out.
	if (m_class_get_rank (held) != 0)
		return arrays_agree (held) ? CastAnswer::No : CastAnswer::Unknown;

	// A subclass may implement any interface, and a delegate has variance of
	// its own, so a bound answers neither.
	if (mono_class_is_interface (target) || m_class_is_delegate (target))
		return CastAnswer::Unknown;

	// Both are ordinary classes now, and a class has one base. So a class
	// assignable to held and to target puts the two on one chain, and the arm
	// above ruled out held reaching target. An array target is covered as well:
	// an array descends from System.Array, so a held that admits one is a held
	// target reaches.
	return mono_class_is_assignable_from_internal (held, target) ? CastAnswer::Unknown
	                                                             : CastAnswer::No;
}

namespace {

/// Replaces \p site with \p value, keeping the block structure the site's own
/// shape needs.
void
answer_with (CallBase *site, Value *value)
{
	site->replaceAllUsesWith (value);

	// An invoke is a terminator, and the answer raises nothing, so the unwind
	// edge goes with it. A pad left with no predecessor is dead code the
	// simplification behind this pass removes.
	if (auto *invoke = dyn_cast<InvokeInst> (site)) {
		BasicBlock *head = invoke->getParent ();

		BranchInst::Create (invoke->getNormalDest (), invoke->getIterator ());
		invoke->getUnwindDest ()->removePredecessor (head);
	}

	site->eraseFromParent ();
}

/**
 * How a test against \p target comes out for every value \p v can be.
 *
 * A phi and a select each name more than one value, and the answer is theirs
 * only where all of them agree. A null arm agrees with either answer, because
 * both rewrites leave null where the operand is null.
 *
 * \p seen holds the values this walk is already inside. A phi in a loop names
 * itself, and the values a self-reference stands for are the ones its other
 * arms already put in, so leaving it out changes no answer.
 */
CastAnswer
answer_for (const Value *v, MonoClass *target, const Function &f,
            SmallPtrSetImpl<const Value *> &seen)
{
	v = strip_casts (v);

	SmallVector<const Value *, 4> arms;

	if (const auto *phi = dyn_cast<PHINode> (v))
		arms.append (phi->incoming_values ().begin (), phi->incoming_values ().end ());
	else if (const auto *select = dyn_cast<SelectInst> (v))
		arms = { select->getTrueValue (), select->getFalseValue () };

	if (arms.empty ()) {
		std::pair<MonoClass *, bool> held = operand_class (v, f);

		return cast_answer (target, held.first, held.second);
	}

	if (!seen.insert (v).second)
		return CastAnswer::Unknown;

	CastAnswer agreed = CastAnswer::Unknown;
	bool constrained = false;

	for (const Value *arm : arms) {
		if (isa<ConstantPointerNull> (strip_casts (arm)))
			continue;

		CastAnswer answer = answer_for (arm, target, f, seen);

		if (answer == CastAnswer::Unknown)
			return CastAnswer::Unknown;
		if (constrained && answer != agreed)
			return CastAnswer::Unknown;

		agreed = answer;
		constrained = true;
	}

	// Every arm null, which the walk above reaches only through a cycle it
	// dropped. Nothing is known about what the phi holds.
	return constrained ? agreed : CastAnswer::Unknown;
}

/// The class the site tests against, or null where an rgctx fetch answered for
/// it and the IR holds no class.
MonoClass *
tested_class (const CallBase *site)
{
	auto *global = dyn_cast<GlobalValue> (site->getArgOperand (1));

	return global != nullptr ? marked_class (*global) : nullptr;
}

/// Folds what it can of the sites in \p f that call the declaration \p name.
bool
fold_sites (Function &f, StringRef name, bool throw_on_fail)
{
	bool changed = false;

	for (CallBase *site : builtin_sites (f, name)) {
		Value *obj = site->getArgOperand (0);
		SmallPtrSet<const Value *, 8> seen;
		CastAnswer answer = answer_for (obj, tested_class (site), f, seen);

		// Both forms answer the operand where the test passes, since both
		// answer null for null and neither changes what it is handed.
		if (answer == CastAnswer::Yes) {
			answer_with (site, obj);
			changed = true;
			continue;
		}

		// Only isinst has a value for a test that fails. castclass raises
		// InvalidCastException for every operand but null, which is a site this
		// leaves for the lowering to write as it stands.
		if (answer == CastAnswer::No && !throw_on_fail) {
			answer_with (site, ConstantPointerNull::get (
						   PointerType::get (f.getContext (), 0)));
			changed = true;
		}
	}

	return changed;
}

} // namespace

bool
fold_type_tests (Function &f)
{
	// The classes ride as pointers into this process. An offline run over a
	// dumped module would read them as addresses of its own.
	if (current_compile ().domain == nullptr || !fold_casts ())
		return false;

	bool changed = fold_sites (f, cast_isinst_name, false);

	return fold_sites (f, cast_castclass_name, true) || changed;
}

} // namespace mono

/**
 * \file
 * \brief Deciding a type test from two classes, and reading what a delegate
 * site's own operand says about the method it calls.
 */

#include "builtins.hpp"

#include "constant-values.hpp"
#include "operand-class.hpp"
#include "strip-casts.hpp"

#include "mono/metadata/abi-details.h"
#include "mono/metadata/class-init.h"
#include "mono/metadata/class-inlines.h"
#include "mono/metadata/class-internals.h"
#include "mono/metadata/metadata.h"
#include "mono/metadata/object-internals.h"

#include <llvm/ADT/APInt.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DataLayout.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/InstrTypes.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Metadata.h>
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

bool
isinst_settles_over_incoming (PHINode &phi, function_ref<CastAnswer (Value *)> answer)
{
	for (unsigned i = 0, n = phi.getNumIncomingValues (); i < n; i++)
		if (answer (phi.getIncomingValue (i)) == CastAnswer::Unknown)
			return false;

	return true;
}

Value *
rebuild_isinst_over_incoming (PHINode &phi, function_ref<CastAnswer (Value *)> answer)
{
	if (!isinst_settles_over_incoming (phi, answer))
		return nullptr;

	unsigned n = phi.getNumIncomingValues ();
	auto *rebuilt = PHINode::Create (phi.getType (), n, "isinst_merge", phi.getIterator ());

	// A "no" edge takes null rather than being dropped, which is what lets a
	// later jump-threading pass split the merge and eliminate each cascade's
	// test against its own class.
	for (unsigned i = 0; i < n; i++) {
		Value *edge = answer (phi.getIncomingValue (i)) == CastAnswer::Yes
			? phi.getIncomingValue (i)
			: ConstantPointerNull::get (PointerType::get (phi.getContext (), 0));

		rebuilt->addIncoming (edge, phi.getIncomingBlock (i));
	}

	return rebuilt;
}

void
mark_delegate_method_ptr_read (LoadInst *load)
{
	load->setMetadata (LLVMContext::MD_invariant_group,
	                   MDNode::get (load->getContext (), {}));
}

bool
reads_callee_off_delegate (const CallBase &site)
{
	if (site.arg_size () < 1 || site.getCalledFunction () != nullptr)
		return false;

	const auto *pick = dyn_cast<SelectInst> (strip_casts (site.getCalledOperand ()));

	if (pick == nullptr)
		return false;

	const auto *test = dyn_cast<ICmpInst> (pick->getCondition ());
	const auto *impl = dyn_cast<LoadInst> (strip_casts (pick->getFalseValue ()));

	// The arms sit the way CreateSelect (CreateIsNull (impl), dispatch, impl)
	// wrote them, so the load answers on the arm where it is not null.
	if (test == nullptr || impl == nullptr
	    || test->getPredicate () != ICmpInst::ICMP_EQ
	    || !isa<ConstantPointerNull> (test->getOperand (1))
	    || strip_casts (test->getOperand (0)) != impl)
		return false;

	const DataLayout &layout = site.getModule ()->getDataLayout ();
	const Value *address = impl->getPointerOperand ();
	APInt offset (layout.getIndexTypeSizeInBits (address->getType ()), 0);
	const Value *object = address->stripAndAccumulateConstantOffsets (
		layout, offset, /*AllowNonInbounds=*/true);

	// stripAndAccumulateConstantOffsets () peels offsets and stops, so it leaves
	// a freeze that the other side's peel takes off.
	return strip_casts (object) == strip_casts (site.getArgOperand (0))
	       && offset == MONO_STRUCT_OFFSET (MonoDelegate, invoke_impl);
}

DelegateTarget
delegate_target_at (Value *receiver, const ConstantValues &values)
{
	const ValueSources &from = values.sources (receiver);
	DelegateTarget answer;

	/*
	 * A delegate copied into a field and read back is answered by the store
	 * walk behind `sources ()`. That answer leaves out the field's own
	 * zero-filled initial value, which is safe here for a reason no class
	 * caller has: the Invoke this receiver feeds dereferences it, so a null
	 * delegate faults at the site instead of reaching a wrong target.
	 *
	 * A field whose object escapes still names the delegates the stores this
	 * walk can see put there. Such an answer is a candidate rather than the
	 * target, which is what leaves `settled` false and sends the site to the
	 * guarded form.
	 */
	bool opaque = false;

	for (const Value *at : from.sources) {
		if (isa<ConstantPointerNull> (at))
			continue;

		MonoMethod *named = delegate_target (at);

		// A source naming no method is one more path the compare covers, so
		// it leaves a candidate rather than emptying the answer.
		if (named == nullptr) {
			opaque = true;
			continue;
		}

		// Two sources naming different methods leave nothing to compare
		// against: picking one is a guess a profile would have to settle,
		// and there is no profile here.
		if (answer.method != nullptr && answer.method != named)
			return {};

		answer.method = named;
	}

	answer.settled = answer.method != nullptr && !opaque;

	return answer;
}

} // namespace mono

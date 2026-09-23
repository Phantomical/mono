/**
 * \file
 * \brief Resolving a dispatch site against the class its receiver turned out to
 * have.
 *
 * The slot the site reads is the one the runtime would have read, so what
 * stands there is the method a caller can name directly. The declaration this
 * writes is the one a direct call would have named all along: it carries the
 * MonoMethod, so the engine renames it to the published entry and the inliner
 * reads the method back off it like any other callee.
 */

#include "devirtualize.hpp"

#include "analysis/operand-class.hpp"
#include "analysis/constant-values.hpp"
#include "builtins.hpp"
#include "compile-state.hpp"
#include "direct-call.hpp"
#include "method-symbols.hpp"
#include "runtime/naming.hpp"
#include "runtime/options.hpp"
#include "vtable-func.hpp"

#include "mini.h"

#include "mono/metadata/abi-details.h"
#include "mono/metadata/class-init.h"
#include "mono/metadata/class-inlines.h"
#include "mono/metadata/class-internals.h"
#include "mono/metadata/debug-helpers.h"
#include "mono/metadata/marshal.h"
#include "mono/metadata/metadata.h"
#include "mono/metadata/object-internals.h"

#include <llvm/ADT/SmallVector.h>
#include <llvm/Analysis/BlockFrequencyInfo.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/GlobalValue.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/InstrTypes.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/MDBuilder.h>
#include <llvm/IR/Module.h>
#include <llvm/ProfileData/InstrProf.h>

#include <algorithm>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <optional>

using namespace llvm;

namespace mono {
namespace {

/// Whether klass has a vtable for a slot to be read out of.
///
/// The layout is what fills the slots in, and a class that cannot have one
/// reaches vtable[index] with vtable null. Leaving a site alone puts the type
/// load back where the runtime raises it.
bool
laid_out (MonoClass *klass)
{
	mono_class_setup_vtable (klass);

	return !mono_class_has_failure (klass) && m_class_get_vtable (klass) != nullptr;
}

} // namespace

MonoMethod *
slot_target (MonoClass *klass, int32_t index)
{
	if (!laid_out (klass))
		return nullptr;

	/*
	 * The word at vtable_size is the static field block rather than a method,
	 * so the bound is strict. A negative index is what a method with no slot of
	 * its own carries.
	 */
	if (index < 0 || index >= m_class_get_vtable_size (klass))
		return nullptr;

	return nameable (m_class_get_vtable (klass)[index]);
}

namespace {

/// The method \p klass implements \p asked with, or null where a caller cannot
/// name it.
///
/// The IMT slot a site reads is a hash bucket, so the answer comes from the
/// method the site asked for rather than from the slot. That is the same
/// question the runtime's thunk answers with the key in its register.
MonoMethod *
imt_target (MonoClass *klass, MonoMethod *asked)
{
	if (!mono_class_is_interface (asked->klass) || !laid_out (klass))
		return nullptr;

	/*
	 * The table is indexed from the receiver's offset for that interface, and
	 * the resolver asserts that the offset is there. Verifiable IL gives one,
	 * so a site that does not keeps its lookup rather than reading past the
	 * array.
	 */
	gboolean variance_used = FALSE;

	if (mono_class_interface_offset_with_variance (klass, asked->klass, &variance_used) <= 0)
		return nullptr;

	ERROR_DECL (error);
	MonoMethod *target = mono_class_get_virtual_method (klass, asked, FALSE, error);

	if (!is_ok (error)) {
		mono_error_cleanup (error);
		return nullptr;
	}

	/*
	 * A default implementation lives on an interface rather than on the class,
	 * and which one a class gets is not always a question with one answer: where
	 * two interfaces override a method and neither is more specific, the runtime
	 * raises AmbiguousImplementationException at the dispatch.
	 * mono_class_get_virtual_method () answers with a candidate instead of
	 * raising, so that verdict stays the dispatch's to give.
	 * mono/tests/dim-diamondshape.il is what asks for it.
	 */
	if (target != nullptr && mono_class_is_interface (target->klass))
		return nullptr;

	return nameable (target);
}

/// The method \p klass implements the virtual generic \p asked with, or null
/// where a caller cannot name it.
///
/// The slot holds a trampoline, because one slot serves every instantiation.
/// asked is the instantiation the call site named, so the two together settle
/// what that trampoline would have picked.
MonoMethod *
generic_virtual_target (MonoClass *klass, MonoMethod *asked)
{
	if (!laid_out (klass))
		return nullptr;

	ERROR_DECL (error);
	MonoMethod *target = mono_class_get_virtual_method (klass, asked, FALSE, error);

	if (!is_ok (error)) {
		mono_error_cleanup (error);
		return nullptr;
	}

	return nameable (target);
}

/// The shape every use of \p site calls the entry with, or null where the uses
/// do not agree or one of them is not a call.
///
/// Each use names the prototype the IL settled, so they agree in the ordinary
/// case. A use of another kind - the select a delegate's Invoke reads its entry
/// through is one - leaves the site alone, because what the entry is for there
/// is not this pass's to say.
///
/// A call with no arguments is refused as well. Only a method with a receiver
/// reaches a vtable slot, so one is a shape this pass does not understand.
FunctionType *
called_shape (CallBase *site)
{
	FunctionType *shape = nullptr;

	for (User *user : site->users ()) {
		auto *call = dyn_cast<CallBase> (user);

		if (call == nullptr || call->getCalledOperand () != site
		    || call->arg_size () < 1)
			return nullptr;
		if (shape != nullptr && shape != call->getFunctionType ())
			return nullptr;
		shape = call->getFunctionType ();
	}

	return shape;
}

/// Writes at the builder's position the call \p at makes, entering \p entry
/// rather than the callee the site read. \p normal is where an invoke's
/// ordinary edge goes.
///
/// A value type's slot holds the unbox entry, which steps the receiver past the
/// object header before it runs into the body. \p steps_receiver names an entry
/// that expects the step to have happened, so the call does here what that
/// prologue did - mono/mini/thunk.cpp asserts the same size the arch stub is
/// baked with.
///
/// \p drops_key names a site that carried the method it asked for in the IMT
/// register. The thunk that read it is gone, and the target never did
/// (emit_call () says so where it puts the key on), so the call is built again
/// without it. That is also what leaves the call holding the same prototype a
/// direct call to the same method holds anywhere else.
CallBase *
direct_call (IRBuilderBase &b, CallBase &at, Function *entry, bool steps_receiver,
             bool drops_key, BasicBlock *normal)
{
	SmallVector<Value *, 8> args (at.args ().begin (), at.args ().end ());

	if (steps_receiver)
		args[0] = b.CreateGEP (b.getInt8Ty (), args[0],
		                       b.getInt64 (MONO_ABI_SIZEOF (MonoObject)));

	if (drops_key)
		args.pop_back ();

	CallBase *direct;

	if (auto *unwinds = dyn_cast<InvokeInst> (&at))
		direct = b.CreateInvoke (entry, normal, unwinds->getUnwindDest (), args);
	else
		direct = b.CreateCall (entry, args);

	// The key's own slot goes with it. Everything else the site said about its
	// arguments - which are extended, which are by value - still holds.
	AttributeList was = at.getAttributes ();

	direct->setAttributes (
		drops_key ? was.removeParamAttributes (at.getContext (), at.arg_size () - 1)
	                  : was);
	direct->setCallingConv (at.getCallingConv ());
	direct->setDebugLoc (at.getDebugLoc ());

	if (auto *from = dyn_cast<CallInst> (&at))
		if (auto *now = dyn_cast<CallInst> (direct))
			now->setTailCallKind (from->getTailCallKind ());

	return direct;
}

/// Rewrites \p call so it enters \p entry. The two flags carry the meaning
/// direct_call () gives them.
void
enter_at (CallBase *call, Function *entry, bool steps_receiver, bool drops_key)
{
	IRBuilder<> builder (call);

	if (!drops_key) {
		if (steps_receiver)
			call->setArgOperand (
				0, builder.CreateGEP (
					   builder.getInt8Ty (), call->getArgOperand (0),
					   builder.getInt64 (MONO_ABI_SIZEOF (MonoObject))));

		call->setCalledFunction (entry);
		return;
	}

	CallBase *direct = direct_call (
		builder, *call, entry, steps_receiver, drops_key,
		isa<InvokeInst> (call) ? cast<InvokeInst> (call)->getNormalDest () : nullptr);

	call->replaceAllUsesWith (direct);
	call->eraseFromParent ();
}

/// Which declaration a set of sites calls, which decides where the method comes
/// from and whether the calls carry a key to drop.
enum class Lookup { vtable, imt, generic_virtual };

/// What a dispatch site resolves to: the method a receiver enters, and the
/// prototype a caller calls it with.
struct Dispatched {
	MonoMethod *target;
	FunctionType *shape;
};

/// What a receiver of \p klass reaching \p site enters, or nothing where the
/// class does not settle it.
std::optional<Dispatched>
dispatched_at (CallBase *site, MonoClass *klass, Lookup lookup,
               const ConstantValues &values)
{
	const auto *index =
		dyn_cast_or_null<ConstantInt> (values.value (site->getArgOperand (1)));
	FunctionType *shape = called_shape (site);

	if (index == nullptr || shape == nullptr)
		return std::nullopt;

	if (lookup == Lookup::vtable) {
		MonoMethod *target =
			slot_target (klass, static_cast<int32_t> (index->getSExtValue ()));

		if (target == nullptr)
			return std::nullopt;

		return Dispatched{ target, shape };
	}

	const GlobalValue *key = values.global (site->getArgOperand (2));
	MonoMethod *asked = key != nullptr ? get_method_pointer (*key) : nullptr;

	// The key is the last argument, and what the call enters is the method's own
	// prototype, which does not have it.
	if (asked == nullptr || shape->getNumParams () < 1)
		return std::nullopt;

	MonoMethod *target = lookup == Lookup::imt ? imt_target (klass, asked)
	                                           : generic_virtual_target (klass, asked);

	if (target == nullptr)
		return std::nullopt;

	return Dispatched{ target,
		           FunctionType::get (shape->getReturnType (),
		                              shape->params ().drop_back (),
		                              shape->isVarArg ()) };
}

/// Which of the three declarations \p v is a call to, or nothing where it is
/// not a call to one of them.
std::optional<Lookup>
lookup_at (const Value *v)
{
	const auto *site = dyn_cast<CallBase> (v);
	const Function *decl = site != nullptr ? site->getCalledFunction () : nullptr;

	if (decl == nullptr)
		return std::nullopt;

	StringRef name = decl->getName ();

	if (name == vtable_func_name)
		return Lookup::vtable;
	if (name == imt_func_name)
		return Lookup::imt;
	if (name == vtable_gfunc_name)
		return Lookup::generic_virtual;

	return std::nullopt;
}

/// The method \p site reads, or null where its operands do not settle one.
MonoMethod *
site_target (const CallBase *site, Lookup lookup, const ConstantValues &values)
{
	const GlobalValue *vtable = values.global (site->getArgOperand (0));
	MonoClass *klass = vtable != nullptr ? get_class (*vtable) : nullptr;
	const auto *index =
		dyn_cast_or_null<ConstantInt> (values.value (site->getArgOperand (1)));

	if (klass == nullptr || index == nullptr)
		return nullptr;

	if (lookup == Lookup::vtable)
		return slot_target (klass, static_cast<int32_t> (index->getSExtValue ()));

	const GlobalValue *key = values.global (site->getArgOperand (2));
	MonoMethod *asked = key != nullptr ? get_method_pointer (*key) : nullptr;

	if (asked == nullptr)
		return nullptr;

	return lookup == Lookup::imt ? imt_target (klass, asked)
	                             : generic_virtual_target (klass, asked);
}

/// The method and lookup a dispatch site reads.
struct Reached {
	MonoMethod *target;
	Lookup lookup;
};

/// What the dispatch sites reaching \p callee agree the call enters, or nothing
/// where they do not agree on one method.
///
/// A dispatch site is one of the values reaching the callee rather than the
/// callee itself. GVN's partial-redundancy elimination puts a second copy of
/// the site in the predecessor that lacked one and merges the two with a phi,
/// so a call reads its callee off that phi. Two copies of one site name one
/// method while being two values, which is why this combines over the sources.
std::optional<Reached>
reached_target (Value *callee, const ConstantValues &values)
{
	const ValueSources &from = values.sources (callee);

	if (from.sources.empty ())
		return std::nullopt;

	std::optional<Reached> agreed;

	for (const Value *v : from.sources) {
		std::optional<Lookup> lookup = lookup_at (v);

		if (!lookup)
			return std::nullopt;

		MonoMethod *target = site_target (cast<CallBase> (v), *lookup, values);

		if (target == nullptr)
			return std::nullopt;

		// A call enters one method, so a source another cannot cover is one
		// the whole answer has to give up on.
		if (agreed && (agreed->target != target || agreed->lookup != *lookup))
			return std::nullopt;

		agreed = Reached{ target, *lookup };
	}

	return agreed;
}

/// The calls in \p f that read their callee rather than naming it.
SmallVector<CallBase *, 8>
indirect_calls (Function &f)
{
	SmallVector<CallBase *, 8> found;

	for (BasicBlock &block : f)
		for (Instruction &i : block)
			if (auto *call = dyn_cast<CallBase> (&i))
				// Only a method with a receiver reaches a vtable slot, so a
				// call with no arguments is a shape this pass does not read.
				if (call->arg_size () >= 1
				    && !isa<Function> (call->getCalledOperand ()))
					found.push_back (call);

	return found;
}

} // namespace

bool
eliminate_dispatch_sites (Function &f, FunctionAnalysisManager &fam)
{
	const CompileState &compile = current_compile ();

	if (compile.domain == nullptr || !compile.publish)
		return false;

	SmallVector<CallBase *, 8> calls = indirect_calls (f);
	bool changed = false;

	if (calls.empty ())
		return false;

	const ConstantValues &values = fam.getResult<MonoConstantValues> (f);
	SmallVector<std::tuple<CallBase *, Function *, bool, bool>, 8> entries;

	for (CallBase *call : calls) {
		std::optional<Reached> found =
			reached_target (call->getCalledOperand (), values);

		if (!found)
			continue;

		// The key is the last argument, and what the call enters is the
		// method's own prototype, which does not have it.
		bool drops_key = found->lookup != Lookup::vtable;
		FunctionType *shape = call->getFunctionType ();

		if (drops_key) {
			if (shape->getNumParams () < 1)
				continue;

			shape = FunctionType::get (shape->getReturnType (),
			                           shape->params ().drop_back (),
			                           shape->isVarArg ());
		}

		Function *entry = entry_for (*f.getParent (), found->target, shape, compile);

		if (entry == nullptr)
			continue;

		entries.emplace_back (call, entry, publishes_unbox_entry (found->target),
		                      drops_key);
	}

	for (auto [call, entry, unboxes, drops_key] : entries) {
		enter_at (call, entry, unboxes, drops_key);
		changed = true;
	}

	// Erase a site no call reads any more, so `lower_vtable_reads ()` does not
	// write a load of the slot for it. A site a phi still holds stays here. That
	// phi is dead as well, and DCE takes the two together, because the
	// declarations are `memory(none)` and nothing else holds them live.
	for (StringRef name : { vtable_func_name, imt_func_name, vtable_gfunc_name })
		for (CallBase *site : builtin_sites (f, name))
			if (site->use_empty ())
				site->eraseFromParent ();

	return changed;
}

namespace {

/// What a guard's true edge weighs where the function carries no profile.
///
/// Only the ratio against the zero beside it reaches BranchProbabilityInfo, so
/// this stands in for a count rather than claiming one.
constexpr uint64_t unprofiled_guard_weight = 1000;

/// Marks the dispatch a guard has already been written around, which is the one
/// left standing on the arm the compare did not pick. Reading the same class off
/// it again would nest a second guard inside the first.
constexpr StringRef guarded_md = "mono.dispatch.guarded";

/// Whether a conformant image can put a class other than \p klass in a slot
/// declared with it.
///
/// The six classes below are the reduced types ECMA-335 I.8.7 names, which is
/// what I.8.7.1 compares to decide that two array types hold each other's
/// values, and mono's own cast class is that reduced type -
/// class_composite_fixup_cast_class () (mono/metadata/class-init.c) folds the
/// signed and the unsigned form of each width, and an array of an enum takes
/// the cast class of the enum's underlying type. II.14.3 admits `bool` and
/// `char` as underlying types, so `char[]` shares its class with an array of an
/// enum over char the same way `int[]` shares one with `uint[]`.
///
/// What is left out is left out for two reasons. A reference element is
/// covariant, so a `Base[]` slot holds a `Derived[]` whenever the program uses
/// one and the compare would miss. `float[]`, `double[]` and an array of an
/// ordinary struct hold themselves alone, because reaching one needs an enum
/// over a type II.14.3 does not admit. This loader takes such an enum without
/// complaint, which is why exact_class () still refuses every array, but a
/// compare written for an image no compiler emits pays for itself nowhere.
bool
guardable_array (MonoClass *klass)
{
	if (m_class_get_rank (klass) == 0)
		return false;

	MonoClass *element = m_class_get_element_class (klass);

	if (element == nullptr || !m_class_is_valuetype (element))
		return false;

	MonoClass *shared = m_class_get_cast_class (klass);

	return shared == mono_defaults.byte_class || shared == mono_defaults.int16_class
	       || shared == mono_defaults.int32_class || shared == mono_defaults.int64_class
	       || shared == mono_defaults.char_class || shared == mono_defaults.boolean_class;
}

/// A class a guard compares a receiver against, and how often tier 1 saw it.
struct Candidate {
	MonoClass *klass;
	uint64_t count;
};

/// Returns the classes tier 1 recorded at \p site and the total receiver count,
/// or empty if the site has no record with enough samples.
std::optional<std::pair<SmallVector<Candidate, 4>, uint64_t>>
recorded_classes (const CallBase *site, const CompileState &compile)
{
	if (!guard_profile_dispatch () || !compile.vtable_class)
		return std::nullopt;

	uint64_t total = 0;
	SmallVector<InstrProfValueData, 4> seen =
		getValueProfDataFromInst (*site, IPVK_VTableTarget, UINT32_MAX, total);

	if (seen.empty () || total < guard_profile_min_samples ())
		return std::nullopt;

	SmallVector<Candidate, 4> classes;

	for (const InstrProfValueData &value : seen)
		classes.push_back ({ compile.vtable_class (value.Value), value.Count });

	return std::make_pair (classes, total);
}

/**
 * Whether the blocks around \p call are a shape `guard_dispatch ()` below can
 * write two arms into.
 *
 * The answer the two arms merge into needs one phi in the block the call
 * returns to, with an incoming for each arm. Where that block has a
 * predecessor that is neither arm, such a phi would have to name a value for
 * that edge as well, and the call's own answer is not defined along it.
 *
 * A plain call always fits, because the split below makes the block it returns
 * to and that block then has the guard as its only predecessor. An invoke
 * returns to a block that was already there, and tier 2 gives most of them one
 * of their own - `changeToInvokeAndSplitBasicBlock ()` is what makes it - so
 * refusing the rest costs little.
 *
 * Leaving such a site alone also keeps the block's phis agreeing on their
 * predecessors, which is a rule LLVM enforces nowhere: `InstCombine`'s
 * `PredOrder` reads the list off the first phi in a block and indexes the rest
 * of them with it.
 */
bool
guard_fits (CallBase *call)
{
	auto *unwinds = dyn_cast<InvokeInst> (call);

	if (unwinds == nullptr)
		return true;

	return unwinds->getNormalDest ()->getUniquePredecessor () == call->getParent ();
}

/// One direct call a guard writes: the classes whose receivers take it, the
/// method they all enter, and the weight of its edge.
struct Arm {
	SmallVector<MonoClass *, 2> classes;
	Dispatched entered;
	uint64_t count;
};

/// A dispatch a guard can take, with the arms that guard will write.
struct Guardable {
	CallBase *site;
	CallBase *call;
	SmallVector<Arm, 2> arms;
	/// The weight of the edge to the dispatch the guard keeps.
	uint64_t missed;
	bool drops_key;
	/// Whether the arms come from what tier 1 recorded.
	bool recorded = false;
};

/// Adds an arm for \p klass to \p at, or adds \p klass to the arm that enters
/// the same method. Returns false where \p klass does not settle what the site
/// enters.
bool
add_arm (Guardable &at, MonoClass *klass, uint64_t count, Lookup lookup,
         const ConstantValues &values)
{
	std::optional<Dispatched> entered = dispatched_at (at.site, klass, lookup, values);

	if (!entered)
		return false;

	for (Arm &arm : at.arms) {
		if (arm.entered.target == entered->target && arm.entered.shape == entered->shape) {
			arm.classes.push_back (klass);
			arm.count += count;
			return true;
		}
	}

	at.arms.push_back ({ { klass }, *entered, count });
	return true;
}

/**
 * Collects the sites in \p f that call \p name and that a guard can take.
 *
 * Three rules name classes, asked in this order. The array rule reads the class
 * the slot is declared with, which bounds a set every member of which
 * `guardable_array ()` says reaches the same implementation. The record rule
 * reads the classes tier 1 saw at the site. The guess reads a class an
 * allocation states. Neither of the last two bounds anything, so the compare is
 * what covers the rest.
 */
void
collect_guardable (Function &f, StringRef name, Lookup lookup, BlockFrequencyInfo &counts,
                   const ConstantValues &values, const CompileState &compile,
                   SmallVectorImpl<Guardable> &into)
{
	for (CallBase *site : builtin_sites (f, name)) {
		// The guard writes the call again on an arm of its own, so a site
		// several calls read is one it leaves alone.
		CallBase *call = dispatch_call (site);

		if (call == nullptr || !guard_fits (call))
			continue;

		Value *object = object_vtable_read (site->getArgOperand (0));

		if (object == nullptr || site->getMetadata (guarded_md) != nullptr)
			continue;

		// A class exact_class () answers needs no guard: eliminate_object_vtables ()
		// replaces the read with that class's vtable, and the elimination above
		// takes the site from there. Asking it rather than reading `exact` below is
		// what keeps the two in step, since it has a rule of its own for a bound on
		// a sealed class.
		if (exact_class (object, f, values) != nullptr)
			continue;

		/*
		 * The zero on the other edge of a guard without a record is the point:
		 * the site's whole count goes to the direct call, which is what a cost
		 * model reading block counts weighs the target at. The dispatching arm
		 * is a call nothing can inline, so splitting the count with it buys
		 * nothing.
		 */
		uint64_t hot = std::max<uint64_t> (
			counts.getBlockProfileCount (call->getParent ()).value_or (unprofiled_guard_weight),
			1);
		Guardable at { site, call, {}, 0, lookup != Lookup::vtable };
		MonoClass *declared = operand_class (object, f, values).first;

		if (declared != nullptr && guard_array_dispatch () && guardable_array (declared)) {
			add_arm (at, declared, hot, lookup, values);
		} else if (auto recorded = recorded_classes (site, compile)) {
			auto [classes, total] = *recorded;

			for (const Candidate &seen : classes)
				add_arm (at, seen.klass, seen.count, lookup, values);

			// The share is the method's rather than the class's, so two
			// classes that enter one method count together.
			std::sort (at.arms.begin (), at.arms.end (),
			           [] (const Arm &a, const Arm &b) { return a.count > b.count; });

			while (!at.arms.empty ()
			       && (at.arms.size () > guard_profile_classes ()
			           || at.arms.back ().count * 100 < total * guard_profile_min_share ()))
				at.arms.pop_back ();

			uint64_t covered = 0;

			for (const Arm &arm : at.arms)
				covered += arm.count;

			// A record no method dominates is a site a guess would miss as
			// well, so the guess is not asked.
			at.missed = total - covered;
			at.recorded = true;
		} else if (guard_class_dispatch ()) {
			if (MonoClass *guess = guessed_class (object, f, values))
				add_arm (at, guess, hot, lookup, values);
		}

		if (!at.arms.empty ())
			into.push_back (std::move (at));
	}
}

/// Branch weights for an edge taken \p taken times against one taken
/// \p other times, scaled down together until both fit.
MDNode *
guard_weights (LLVMContext &c, uint64_t taken, uint64_t other)
{
	while (taken > UINT32_MAX || other > UINT32_MAX) {
		taken >>= 1;
		other >>= 1;
	}

	return MDBuilder (c).createBranchWeights ((uint32_t) std::max<uint64_t> (taken, 1),
	                                          (uint32_t) other);
}

/// Adds an incoming for \p arm to every phi in \p block that has one for
/// \p had, carrying the same value.
///
/// A block both arms reach needs two incomings where it had one. Renaming the
/// single incoming would leave the other arm's edge unnamed.
void
share_phis_with (BasicBlock *block, BasicBlock *had, BasicBlock *arm)
{
	for (PHINode &phi : block->phis ()) {
		int at = phi.getBasicBlockIndex (had);

		if (at >= 0)
			phi.addIncoming (phi.getIncomingValue (at), arm);
	}
}

/// An arm with its compares and its callee named in the module.
struct NamedArm {
	SmallVector<Constant *, 2> vtables;
	Function *entry;
	MonoMethod *target;
	uint64_t count;
};

/// Sends \p at's call through a compare of the receiver's vtable against each
/// arm's vtables in turn, with a call of that arm's entry where one matches.
void
guard_dispatch (const Guardable &at, ArrayRef<NamedArm> arms, uint64_t missed)
{
	CallBase &call = *at.call;
	LLVMContext &c = call.getContext ();
	Function *f = call.getFunction ();
	BasicBlock *head = call.getParent ();
	Value *read = at.site->getArgOperand (0);
	auto *unwinds = dyn_cast<InvokeInst> (&call);

	BasicBlock *tail = unwinds != nullptr
	                           ? unwinds->getNormalDest ()
	                           : head->splitBasicBlock (call.getIterator (), "guard_done");
	BasicBlock *pad = unwinds != nullptr ? unwinds->getUnwindDest () : nullptr;
	BasicBlock *slow = BasicBlock::Create (c, "guard_dispatch", f, tail);

	// The call and the site it reads its callee from move into the dispatching
	// arm rather than being cloned. That is what keeps the matching arm's
	// direct call off the vtable read.
	call.removeFromParent ();
	call.insertInto (slow, slow->end ());
	at.site->removeFromParent ();
	at.site->insertInto (slow, slow->begin ());
	at.site->setMetadata (guarded_md, MDNode::get (c, {}));

	SmallVector<std::pair<BasicBlock *, CallBase *>, 2> directs;

	for (const NamedArm &arm : arms) {
		BasicBlock *fast = BasicBlock::Create (c, "guard_direct", f, slow);
		IRBuilder<> b (fast);

		b.SetCurrentDebugLocation (call.getDebugLoc ());

		CallBase *direct = direct_call (b, call, arm.entry,
		                                publishes_unbox_entry (arm.target), at.drops_key,
		                                tail);

		if (unwinds == nullptr)
			b.CreateBr (tail);
		directs.emplace_back (fast, direct);
	}

	if (unwinds == nullptr) {
		IRBuilder<> (slow).CreateBr (tail);
	} else {
		// Every arm reaches the pad and the continuation now, where the one
		// invoke reached each of them from the block above.
		tail->replacePhiUsesWith (head, slow);
		pad->replacePhiUsesWith (head, slow);

		for (auto [fast, direct] : directs) {
			share_phis_with (tail, slow, fast);
			share_phis_with (pad, slow, fast);
		}
	}

	if (!call.getType ()->isVoidTy ()) {
		PHINode *merged = PHINode::Create (call.getType (), directs.size () + 1,
		                                   "guard_result", tail->getFirstNonPHIIt ());

		// Before the incoming values name it, so that replacing the call's uses
		// does not reach into the phi's own operand for the dispatched answer.
		call.replaceAllUsesWith (merged);

		for (auto [fast, direct] : directs)
			merged->addIncoming (direct, fast);
		merged->addIncoming (&call, slow);
	}

	// head is left without a terminator either way: an invoke was the
	// terminator, and a call left the branch the split wrote.
	if (Instruction *stale = head->getTerminatorOrNull ())
		stale->eraseFromParent ();

	uint64_t rest = missed;

	for (const NamedArm &arm : arms)
		rest += arm.count;

	BasicBlock *test = head;

	for (size_t i = 0; i < arms.size (); i++) {
		rest -= arms[i].count;

		BasicBlock *next = i + 1 < arms.size ()
		                           ? BasicBlock::Create (c, "guard_next", f, directs[i + 1].first)
		                           : slow;
		IRBuilder<> guard (test);
		Value *hit = nullptr;

		guard.SetCurrentDebugLocation (call.getDebugLoc ());

		for (Constant *vtable : arms[i].vtables) {
			Value *same = guard.CreateICmpEQ (read, vtable, "guard_hit");

			hit = hit != nullptr ? guard.CreateOr (hit, same) : same;
		}

		guard.CreateCondBr (hit, directs[i].first, next)
			->setMetadata (LLVMContext::MD_prof, guard_weights (c, arms[i].count, rest));
		test = next;
	}
}

void
trace_guard (const Function &f, ArrayRef<NamedArm> arms, uint64_t missed)
{
	uint64_t total = missed;

	for (const NamedArm &arm : arms)
		total += arm.count;

	fprintf (stderr, "[llvm-jit] guarding a recorded dispatch in %s:", f.getName ().str ().c_str ());

	for (const NamedArm &arm : arms) {
		char *name = mono_method_full_name (arm.target, TRUE);

		fprintf (stderr, " %s (%" PRIu64 "/%" PRIu64 ")", name, arm.count, total);
		g_free (name);
	}

	fprintf (stderr, "\n");
}

} // namespace

bool
guard_dispatch_sites (Function &f, BlockFrequencyInfo &counts, const ConstantValues &values)
{
	const CompileState &compile = current_compile ();

	// The classes ride as pointers into this process, and the vtable the compare
	// reads is a symbol resolved against this compile's domain.
	if (compile.domain == nullptr || !compile.publish || !compile.vtable_of)
		return false;

	if (builtin_sites (f, vtable_func_name).empty ()
	    && builtin_sites (f, imt_func_name).empty ()
	    && builtin_sites (f, vtable_gfunc_name).empty ())
		return false;

	SmallVector<Guardable, 4> pending;

	// Every weight is read before the first split, because a block this pass
	// makes has no count of its own and the analysis is stale the moment one
	// appears.
	collect_guardable (f, vtable_func_name, Lookup::vtable, counts, values, compile, pending);
	collect_guardable (f, imt_func_name, Lookup::imt, counts, values, compile, pending);
	collect_guardable (f, vtable_gfunc_name, Lookup::generic_virtual, counts, values,
	                   compile, pending);

	bool changed = false;

	for (const Guardable &at : pending) {
		SmallVector<NamedArm, 2> arms;
		uint64_t missed = at.missed;

		for (const Arm &arm : at.arms) {
			NamedArm named { {}, nullptr, arm.entered.target, arm.count };

			for (MonoClass *klass : arm.classes)
				if (Constant *vtable = compile.vtable_of (*f.getParent (), klass))
					named.vtables.push_back (vtable);

			if (!named.vtables.empty ())
				named.entry = entry_for (*f.getParent (), arm.entered.target,
				                         arm.entered.shape, compile);

			if (named.entry != nullptr)
				arms.push_back (std::move (named));
			else
				missed += arm.count;
		}

		if (arms.empty ())
			continue;

		if (at.recorded && is_jit_trace_enabled ())
			trace_guard (f, arms, missed);

		guard_dispatch (at, arms, missed);
		changed = true;
	}

	return changed;
}

PreservedAnalyses
GuardDispatchPass::run (Function &f, FunctionAnalysisManager &fam)
{
	BlockFrequencyInfo &counts = fam.getResult<BlockFrequencyAnalysis> (f);
	const ConstantValues &values = fam.getResult<MonoMemoryValues> (f);

	return guard_dispatch_sites (f, counts, values) ? PreservedAnalyses::none ()
	                                                : PreservedAnalyses::all ();
}

} // namespace mono

/**
 * \file
 * \brief Writing a value's class beside it, and reading it back.
 */

#include "operand-class.hpp"

#include "compile-state.hpp"
#include "method-symbols.hpp"
#include "passes/alloc-func.hpp"
#include "strip-casts.hpp"
#include "value-walk.hpp"

#include "mono/metadata/class.h"
#include "mono/metadata/class-internals.h"
#include "mono/metadata/loader.h"
#include "mono/metadata/metadata.h"
#include "mono/metadata/object-internals.h"

#include <llvm/ADT/APInt.h>
#include <llvm/ADT/ArrayRef.h>
#include <llvm/ADT/SmallPtrSet.h>
#include <llvm/ADT/SmallVector.h>
#include <llvm/IR/Argument.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DataLayout.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/GlobalObject.h>
#include <llvm/IR/GlobalValue.h>
#include <llvm/IR/InstrTypes.h>
#include <llvm/IR/Instruction.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Metadata.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Operator.h>
#include <llvm/Support/ModRef.h>

#include <cstdint>
#include <optional>

using namespace llvm;

namespace mono {
namespace {

Metadata *
as_metadata (LLVMContext &c, const void *pointer)
{
	return ConstantAsMetadata::get (ConstantInt::get (
		Type::getInt64Ty (c), reinterpret_cast<uintptr_t> (pointer)));
}

/// The pointer at \p at in \p node, or null where the node holds none there.
void *
pointer_in (const MDNode *node, unsigned at)
{
	if (node == nullptr || at >= node->getNumOperands ())
		return nullptr;

	auto *held = mdconst::dyn_extract<ConstantInt> (node->getOperand (at));

	if (held == nullptr)
		return nullptr;

	return reinterpret_cast<void *> (static_cast<uintptr_t> (held->getZExtValue ()));
}

MonoClass *
class_in (const MDNode *node, unsigned at)
{
	return static_cast<MonoClass *> (pointer_in (node, at));
}

/// The method \p site was marked with under \p kind, or null where it carries
/// no such mark.
MonoMethod *
marked_with (const Instruction &site, StringRef kind)
{
	return static_cast<MonoMethod *> (pointer_in (site.getMetadata (kind), 0));
}

/*
 * Below is the second channel: a read of an initonly static, recognized by its
 * shape rather than by a mark.
 *
 * The translator marks the load it writes, and that mark does not survive.
 * InstCombine folds the address GEP into the load's pointer operand, which
 * builds a new load, and only a field at offset 0 has no GEP to fold. What the
 * shape carries instead is a marked global and a constant offset, and both
 * outlive every pass because no instruction-level transform rebuilds a global.
 *
 * Reading the field here rather than at translation is also what lets a load
 * answer that the translator could not: one a pass rematerialized, and one
 * inlined from a body compiled before the class initializer had run.
 */

/// The class \p held is an instance of, or null where a compile cannot state one.
///
/// A transparent proxy stands in for another class and carries a vtable that is
/// not that class's.
MonoClass *
settled_class_of (MonoObject *held)
{
	MonoClass *klass = mono_object_class (held);

	return mono_class_is_marshalbyref (klass) ? nullptr : klass;
}

/// The method the delegate \p held calls, or null where \p held is not a
/// delegate whose target this compile can name.
///
/// MonoDelegate::method is the field to read. mini_init_delegate () writes it
/// once, at construction, and no other path writes it at all. method_ptr is not
/// an alternative: the delegate trampoline replaces it on the first call and can
/// put an unbox entry there, so a value read now is not the one a later call
/// uses.
///
/// Two delegates name a method they do not enter. One built from `ldvirtftn`
/// resolves an override when it is called, and a multicast one runs an
/// invocation list instead of a target.
MonoMethod *
settled_delegate_target_of (MonoObject *held)
{
	if (m_class_get_parent (mono_object_class (held))
	    != mono_defaults.multicastdelegate_class)
		return nullptr;

	auto *delegate = (MonoDelegate *) held;

	if (delegate->method_is_virtual
	    || ((MonoMulticastDelegate *) delegate)->delegates != nullptr)
		return nullptr;

	return delegate->method;
}

/// The static field of \p klass that lives at \p offset in its statics block,
/// or null where no field of that class is declared there.
MonoClassField *
static_field_at (MonoClass *klass, int offset)
{
	gpointer iter = nullptr;

	while (MonoClassField *field = mono_class_get_fields_internal (klass, &iter)) {
		uint32_t flags = mono_field_get_flags (field);

		// A literal is a metadata constant with no storage, so it holds an
		// offset that says nothing and can stand where a real field is.
		if ((flags & FIELD_ATTRIBUTE_STATIC) == 0
		    || (flags & FIELD_ATTRIBUTE_LITERAL) != 0)
			continue;

		if (m_field_get_offset (field) == offset)
			return field;
	}

	return nullptr;
}

/// The object \p field holds, or null where this compile cannot read one.
///
/// `initonly` makes the class initializer the only writer that IL has, so the
/// value read once that initializer has run is the value the field keeps. What
/// a caller may state from it is what stays right while the collector moves the
/// object, so no address is written down.
///
/// Reflection is the writer IL does not have.
/// `mono_field_static_set_value_internal ()` refuses a literal and nothing else,
/// and no compiled body is taken back when a field is written that way. A
/// program that does it reads a stale object here.
MonoObject *
initonly_static_value (MonoDomain *domain, MonoClassField *field)
{
	MonoType *type = mono_field_get_type_internal (field);

	if ((mono_field_get_flags (field) & FIELD_ATTRIBUTE_INIT_ONLY) == 0
	    || !MONO_TYPE_IS_REFERENCE (type))
		return nullptr;

	// A special static lives per thread or per context, so what stands there
	// now says nothing about what a compiled body will read.
	if (field->offset < 0)
		return nullptr;

	ERROR_DECL (vtable_error);
	MonoVTable *vtable = mono_class_vtable_checked (domain, field->parent, vtable_error);

	if (vtable == nullptr) {
		mono_error_cleanup (vtable_error);
		return nullptr;
	}

	/*
	 * The flag goes on once the initializer has run. A body compiled before
	 * that reads whatever the field holds part way through, which the rest of
	 * the initializer is free to replace.
	 */
	if (!vtable->initialized)
		return nullptr;

	return *(MonoObject **) ((char *) mono_vtable_get_static_field_data (vtable)
	                         + field->offset);
}

/// The object \p v reads out of an initonly static, or null where \p v is not
/// such a read or this compile cannot answer for it.
///
/// A shared body fetches its statics off the run-time context instead of naming
/// a block, so the marked global a match needs is the same thing that says the
/// class is closed.
MonoObject *
initonly_static_read (const Value *v)
{
	const auto *load = dyn_cast<LoadInst> (v);

	if (load == nullptr || current_compile ().domain == nullptr)
		return nullptr;

	const DataLayout &layout = load->getModule ()->getDataLayout ();
	const Value *address = load->getPointerOperand ();
	APInt offset (layout.getIndexTypeSizeInBits (address->getType ()), 0);
	const auto *block = dyn_cast<GlobalValue> (address->stripAndAccumulateConstantOffsets (
		layout, offset, /*AllowNonInbounds=*/true));

	if (block == nullptr || offset.isNegative () || !offset.isSignedIntN (32))
		return nullptr;

	MonoClass *klass = marked_statics_class (*block);

	if (klass == nullptr)
		return nullptr;

	MonoClassField *field =
		static_field_at (klass, static_cast<int> (offset.getSExtValue ()));

	return field != nullptr ? initonly_static_value (current_compile ().domain, field)
	                        : nullptr;
}

/// The class an allocation site makes, read off the vtable its first operand
/// names, or null where \p site is not one of the object-allocation builtins,
/// that operand names no class this compile can read, or the vtable does not
/// settle what comes back.
///
/// `mono.exact.class` is the ordinary way an allocation states its class, and
/// it can go missing: `changeToInvokeAndSplitBasicBlock ()`
/// (`llvm/lib/Transforms/Utils/Local.cpp`) is what `InlineFunction ()` calls
/// to turn a folded call into an invoke when the call it was folded into sits
/// in a protected region, and it copies only the debug location, the calling
/// convention, the attributes and `MD_prof` onto the new instruction. The
/// vtable operand is not metadata, so it survives that rewrite, and it names
/// the same class the missing mark would have.
///
/// A marshalbyref or COM class is the one case where the vtable operand does
/// not settle it: `mono_object_new_specific_checked ()` can answer such an
/// allocation with a transparent proxy instead, and a proxy carries a vtable
/// of its own. `MethodLLVMEmitter::allocation_can_be_a_proxy ()`
/// (`method-to-llvm/call.cpp`) is the same rule, read here without the
/// translator: it is two calls into class metadata, not a fact this compile
/// only knows while it is translating.
MonoClass *
allocation_class (const CallBase &site)
{
	const Function *callee = site.getCalledFunction ();

	if (callee == nullptr)
		return nullptr;

	StringRef name = callee->getName ();

	if (name != alloc_object_name && name != alloc_object_kept_name)
		return nullptr;

	const auto *vtable =
		dyn_cast<GlobalObject> (site.getArgOperand (0)->stripPointerCasts ());

	if (vtable == nullptr)
		return nullptr;

	MonoClass *klass = marked_class (*vtable);

	if (klass == nullptr
	    || mono_class_is_marshalbyref (klass) || mono_class_is_com_object (klass))
		return nullptr;

	return klass;
}

/// The class \p v answers on its own, without looking through a `PHINode` or
/// a `SelectInst`. This is the class marked on \p v, the class an initonly
/// static read off it settles, the class an allocation site names through its
/// vtable operand, or the class its parameter type bounds it with.
std::pair<MonoClass *, bool>
leaf_operand_class (const Value *v, const Function &f)
{
	if (const auto *site = dyn_cast<Instruction> (v)) {
		if (MonoClass *marked = class_in (site->getMetadata (exact_class_md), 0))
			return { marked, true };

		if (MonoObject *held = initonly_static_read (site))
			return { settled_class_of (held), true };

		if (const auto *call = dyn_cast<CallBase> (site))
			if (MonoClass *klass = allocation_class (*call))
				return { klass, true };

		return { nullptr, true };
	}

	const auto *arg = dyn_cast<Argument> (v);

	if (arg == nullptr || arg->getParent () != &f)
		return { nullptr, false };

	const MDNode *listed = f.getMetadata (param_classes_md);

	if (listed == nullptr)
		return { nullptr, false };

	for (const MDOperand &entry : listed->operands ()) {
		const auto *pair = dyn_cast<MDNode> (entry);

		if (pair == nullptr || pair->getNumOperands () != 2)
			continue;

		auto *index = mdconst::dyn_extract<ConstantInt> (pair->getOperand (0));

		if (index == nullptr || index->getZExtValue () != arg->getArgNo ())
			continue;

		// A declared type is an upper bound. Every class the slot admits is
		// assignable to it, and none of them has to be it.
		return { class_in (pair, 1), false };
	}

	return { nullptr, false };
}

/*
 * Below is the class rule `value-walk.hpp` runs. `operand_class ()`,
 * `exact_class ()` and `guessed_class ()` are each one call into that walk,
 * each under its own `ClassRule`.
 *
 * A load reaching a matching store joins the walk as a third kind of merge
 * node, beside a phi and a select: `matching_field_stores ()` below gives it
 * arms of its own, one value per store plus a trailing null for the field's
 * zero-filled initial value. The walk's cycle rule covers a store that names
 * the load back, directly or through another merge.
 */

/// How strong an answer the caller needs, which is what decides how the class
/// rule below treats a path it cannot see the end of.
enum class ClassRule {
	/// The answer has to hold on every path, and a null is one of the values a
	/// path can carry. `operand_class ()`.
	settled,

	/// The caller dereferences the answer, so a null path faults in front of
	/// the use and the answer never has to cover it. `exact_class ()`.
	dereferenced,

	/// The caller compares against the answer before it acts on it, so a path
	/// this walk was wrong about costs it nothing. `guessed_class ()`.
	guessed,
};

/// Whether \p rule lets a null skip rather than settle the answer at no class.
bool
skips_null (ClassRule rule)
{
	return rule != ClassRule::settled;
}

/// The stores a walk over one object's uses found, and whether the object
/// stays inside that walk. `field_stores_reaching ()` below fills both in.
struct ReachingStores {
	SmallVector<StoreInst *, 4> stores;
	bool complete;
};

/// Bounds how many uses of one allocation `field_stores_reaching ()` below
/// visits, so a large use graph costs a compile only a small, fixed amount of
/// work.
constexpr unsigned field_use_walk_budget = 128;

/// Bounds how many distinct allocations one field load's base can resolve
/// to, so a wide merge of objects still costs a compile only a small, fixed
/// amount of work. `resolve_base_candidates ()` below refuses outright once
/// this is reached, rather than settling for a partial set: a candidate set
/// this walk could not finish enumerating is not one `field_stores_reaching
/// ()` can be run over completely either.
constexpr unsigned base_candidate_cap = 8;

/// \p site cannot write through the pointer at \p arg_no. This reads what
/// the call states about itself. It does not read the name of the callee.
///
/// `CallBase::onlyReadsMemory (OpNo)` reads only the parameter attributes
/// `readonly` and `readnone`. It does not read the callee's own
/// `MemoryEffects`. A call can state at the function level that argument
/// memory is read-only, with no attribute on the parameter itself.
/// `onlyReadsMemory` then answers false for that call. Checking
/// `getMemoryEffects ().getModRef (IRMemLocation::ArgMem)` as well catches
/// that case. Either check on its own is enough.
bool
cannot_write_through (const CallBase &site, unsigned arg_no)
{
	if (site.onlyReadsMemory (arg_no))
		return true;

	ModRefInfo arg_effect = site.getMemoryEffects ().getModRef (IRMemLocation::ArgMem);

	return !isModSet (arg_effect);
}

/// Every store that writes through \p base, followed across a
/// getelementptr with constant indices and across a phi or a select that
/// renames \p base without computing a new address, or nothing where some
/// other use of \p base makes that unsafe to answer. Every name this walk
/// followed with no getelementptr in between - \p base itself, and every phi
/// or select it renamed through - is appended to \p aliases, because a store
/// found through one of those names is a write through \p base at the same
/// offset a store found through \p base itself would be.
///
/// A refusal covers four uses. One is a store of \p base itself into memory.
/// Another is a getelementptr with an index this cannot read at compile
/// time. Another is a conversion such as `ptrtoint`: a store this walk never
/// sees can turn the integer back into a pointer and write through it. The
/// last is a call argument the call can write through, or keep past the call
/// returning. Each of these lets code outside this walk reach the object, or
/// reach a field this walk cannot rule out as the one \p base's caller is
/// asking about.
///
/// Two other uses are safe, and this walk does not follow past either one. A
/// pointer compare writes nothing and answers an `i1`. It is not a route by
/// which the object is written. A call argument is also safe when this walk
/// can prove two things about the call: it does not write through the
/// pointer, and it does not keep the pointer past the call returning. The
/// write barrier every reference-field store carries meets both conditions.
///
/// `PointerMayBeCaptured ()` answers only the second condition: whether a
/// pointer stays alive past the call it is passed to. A `captures(none)`
/// argument is not kept, and the callee can still write through it for the
/// length of the call. `cannot_write_through ()` above is the separate check
/// this function still needs for the first condition.
///
/// A phi or a select renaming \p base can cycle back to a name this walk
/// already holds - `resolve_base_candidates ()` above resolves a base
/// through exactly such a cycle. Revisiting a phi or a select this walk has
/// already queued contributes nothing rather than refusing the whole
/// answer: every use it has was already queued the first time it was
/// reached, the same rule the class walk's own cycle rule rests on.
///
/// A refused use clears `complete` and the walk carries on over the rest.
/// The stores it still collects are then a subset of what the field can
/// hold, which is what a guessed answer wants and what a settled one has to
/// throw away. Carrying on is also what makes that subset worth having: a
/// walk that stopped at the first refusal would leave `aliases` half filled,
/// so which stores it had found would depend on the order the uses came in.
/// A spent budget stops the walk all the same, because it bounds the work
/// rather than describing the object.
ReachingStores
field_stores_reaching (Value *base, SmallPtrSetImpl<Value *> &aliases)
{
	ReachingStores found { {}, true };
	SmallVector<Value *, 8> work { base };
	SmallPtrSet<Value *, 16> seen { base };
	unsigned budget = field_use_walk_budget;

	aliases.insert (base);

	while (!work.empty ()) {
		Value *v = work.pop_back_val ();

		for (Use &use : v->uses ()) {
			User *user = use.getUser ();

			if (isa<PHINode> (user) || isa<SelectInst> (user)) {
				if (!seen.insert (user).second)
					continue;

				if (budget == 0) {
					found.complete = false;
					return found;
				}
				--budget;

				aliases.insert (user);
				work.push_back (user);
				continue;
			}

			// A user that names v twice is seen twice. Only the first of the
			// two uses is read below, so the second one is a use this walk
			// did not clear rather than one it can skip: a call can take the
			// same pointer as an argument it keeps and an argument it does
			// not.
			if (!seen.insert (user).second) {
				found.complete = false;
				continue;
			}

			if (budget == 0) {
				found.complete = false;
				return found;
			}
			--budget;

			if (const auto *gep = dyn_cast<GEPOperator> (user)) {
				if (!gep->hasAllConstantIndices ()) {
					found.complete = false;
					continue;
				}
				work.push_back (user);
				continue;
			}

			if (const auto *load = dyn_cast<LoadInst> (user)) {
				if (load->getPointerOperand () != v)
					found.complete = false;
				continue;
			}

			if (isa<ICmpInst> (user))
				continue;

			if (auto *call = dyn_cast<CallBase> (user)) {
				// `use` names v itself as an operand, so this covers both a
				// data argument and the callee operand of an indirect call
				// through v. Only the former is safe to skip past. The write
				// barrier passes the field address as a plain argument.
				// Nothing calls through a field's own address.
				if (!call->isArgOperand (&use)) {
					found.complete = false;
					continue;
				}

				unsigned arg_no = call->getArgOperandNo (&use);

				if (!call->doesNotCapture (arg_no)
				    || !cannot_write_through (*call, arg_no))
					found.complete = false;

				continue;
			}

			auto *store = dyn_cast<StoreInst> (user);

			if (store == nullptr) {
				found.complete = false;
				continue;
			}

			// A store of the object itself into memory hands it to code this
			// walk cannot see. The LINQ iterators reach each other this way,
			// each cached in the next one's field.
			if (store->getPointerOperand () != v) {
				found.complete = false;
				continue;
			}

			found.stores.push_back (store);
		}
	}

	return found;
}

SmallVector<const Value *, 4>
matching_field_stores (const LoadInst &load, ClassRule rule,
                       SmallPtrSetImpl<const Value *> &visiting, unsigned &budget,
                       bool *complete = nullptr);

/// Resolves \p v to the allocation call sites a field load's base can name,
/// appending each to \p out once. Answers false where \p v cannot be
/// resolved this way, in which case \p out must be discarded rather than
/// treated as partial.
///
/// \p v is an allocation call (the one candidate for itself), a `PHINode` or
/// a `SelectInst` (every value it merges resolves in turn), or a `LoadInst`
/// (every value `matching_field_stores ()` finds for the field it reads
/// resolves in turn, which makes the two functions mutually recursive). This
/// is what settles a base that mixes a load in with a phi or a select.
///
/// A candidate this arm alone reaches still fails later, at
/// `field_stores_reaching ()` below. The field held the candidate because
/// some store put it there, and that store's *value* operand is the
/// candidate itself - the same shape `field_stores_reaching ()` refuses a
/// direct base under, so it refuses this candidate on sight too.
/// `OperandClassTest
/// .LoadThroughALoadAsBaseAnswersNoClassBecauseTheDiscoveryStoreEscapes`
/// gates that this is the answer, and not a missed case.
///
/// A null constant is skipped rather than refused: this file has two rules
/// for a null merge arm (`operand_class ()`'s and `exact_class ()`'s, both
/// in the header), and the one that applies here is `exact_class ()`'s,
/// because a base is always about to be dereferenced. The load whose class
/// this resolution is for only runs to completion, and hands its answer to
/// a caller, on a path where every base it was computed from was non-null -
/// a null one would have faulted at the dereference that used it, on the
/// same path, before this load ran at all. Every other unresolved value
/// refuses the whole resolution.
///
/// \p visiting and \p budget are the class walk's own. A `PHINode`, a
/// `SelectInst` and a `LoadInst` each spend the shared budget the way a phi
/// or a select does in `walk_operand_class ()`, and a value already on the
/// walk's call stack - this function's own or the class walk's - answers
/// true with nothing added to \p out rather than being visited again. That
/// is sound for the reason the class walk's own cycle rule is: an
/// allocation reachable only by going around the cycle is already reachable
/// by the path that first walked into it.
bool
resolve_base_candidates (Value *v, ClassRule rule, SmallVectorImpl<CallBase *> &out,
                         SmallPtrSetImpl<const Value *> &visiting, unsigned &budget)
{
	v = const_cast<Value *> (strip_casts (v));

	if (isa<ConstantPointerNull> (v) || visiting.count (v))
		return true;

	if (auto *call = dyn_cast<CallBase> (v)) {
		if (allocation_class (*call) == nullptr)
			return false;

		for (CallBase *seen : out)
			if (seen == call)
				return true;

		if (out.size () >= base_candidate_cap)
			return false;

		out.push_back (call);
		return true;
	}

	auto *phi = dyn_cast<PHINode> (v);
	auto *select = phi == nullptr ? dyn_cast<SelectInst> (v) : nullptr;
	auto *load = phi == nullptr && select == nullptr ? dyn_cast<LoadInst> (v) : nullptr;

	if (phi == nullptr && select == nullptr && load == nullptr)
		return false;

	if (budget == 0)
		return false;

	--budget;
	visiting.insert (v);

	bool ok = true;

	if (phi != nullptr) {
		for (Value *incoming : phi->incoming_values ()) {
			if (!resolve_base_candidates (incoming, rule, out, visiting, budget)) {
				ok = false;
				break;
			}
		}
	} else if (select != nullptr) {
		ok = resolve_base_candidates (select->getTrueValue (), rule, out, visiting, budget)
		     && resolve_base_candidates (select->getFalseValue (), rule, out, visiting,
		                                 budget);
	} else {
		SmallVector<const Value *, 4> stores =
			matching_field_stores (*load, rule, visiting, budget);

		if (stores.empty ())
			ok = false;

		for (const Value *stored : stores) {
			if (!resolve_base_candidates (const_cast<Value *> (stored), rule, out,
			                              visiting, budget)) {
				ok = false;
				break;
			}
		}
	}

	visiting.erase (v);
	return ok;
}

/// The values stored into the field \p load reads, where \p load reads a
/// constant-offset field of one or more allocations `resolve_base_candidates
/// ()` above resolves \p load's base to, plus a trailing null standing for
/// the field's initial value. Empty where \p load answers to none of that,
/// in which case the caller falls back to the ordinary leaf answer for a
/// load with no mark of its own.
///
/// Under `ClassRule::settled` and `ClassRule::dereferenced` every resolved
/// candidate has to clear `field_stores_reaching ()` below: one candidate
/// this compile cannot rule out as written from outside makes the whole
/// answer empty, because the stores gathered from the rest would no longer
/// be every store the field can see. `ClassRule::guessed` takes those stores
/// all the same and reports through \p complete that they are a subset.
///
/// `mono.alloc.object` zero-fills what it allocates, so the field reads null
/// until a store runs. A load a store does not dominate, or a load reached on
/// a path where no store to this field ran first, still reads that null. The
/// null this appends stands for that path, and it needs no dominance or
/// reachability proof of its own: `merged_class ()` already settles a merge
/// with a null arm the same way for a phi, by the same rule `exact_class ()`
/// and `operand_class ()` split on. `exact_class ()` skips the null, because
/// its caller dereferences the answer and the null path faults first.
/// `operand_class ()` does not skip it, which is right here too: a type test
/// on this field must see null as an answer the allocation can still give.
SmallVector<const Value *, 4>
matching_field_stores (const LoadInst &load, ClassRule rule,
                       SmallPtrSetImpl<const Value *> &visiting, unsigned &budget,
                       bool *complete)
{
	const DataLayout &dl = load.getModule ()->getDataLayout ();
	Value *address = const_cast<Value *> (load.getPointerOperand ());
	APInt offset (64, 0);
	Value *base =
		address->stripAndAccumulateConstantOffsets (dl, offset, /*AllowNonInbounds=*/true);

	SmallVector<CallBase *, 4> candidates;

	if (!resolve_base_candidates (base, rule, candidates, visiting, budget)
	    || candidates.empty ()) {
		if (complete != nullptr)
			*complete = false;
		return {};
	}

	SmallVector<const Value *, 4> matching;

	for (CallBase *candidate : candidates) {
		SmallPtrSet<Value *, 8> aliases;
		ReachingStores found = field_stores_reaching (candidate, aliases);

		if (!found.complete) {
			if (complete != nullptr)
				*complete = false;
			if (rule != ClassRule::guessed)
				return {};
		}

		for (StoreInst *store : found.stores) {
			APInt at (64, 0);
			Value *store_base = store->getPointerOperand ()->stripAndAccumulateConstantOffsets (
				dl, at, /*AllowNonInbounds=*/true);

			if (aliases.count (store_base) && at == offset)
				matching.push_back (store->getValueOperand ());
		}
	}

	if (!matching.empty ())
		matching.push_back (ConstantPointerNull::get (cast<PointerType> (load.getType ())));

	return matching;
}

/// Reads a class off one value and merges two class answers. `walk_value ()`
/// (`value-walk.hpp`) runs this rule over the merges in front of a value.
class ClassWalk {
public:
	using Answer = std::pair<MonoClass *, bool>;

	ClassWalk (const Function &f, ClassRule rule) : f (f), rule (rule) {}

	bool skips_null () const { return mono::skips_null (rule); }

	Answer exhausted () const { return no_class (); }

	std::optional<Answer> leaf (const Value *v) const
	{
		Answer got = leaf_operand_class (v, f);

		// A guess names a class this function saw an object made under. A
		// parameter's declared class is a bound instead, so a compare
		// against it misses every subclass the slot admits.
		if (rule == ClassRule::guessed && !got.second)
			return std::nullopt;

		return got;
	}

	/// Gives a load the values that reach the field it reads. Empty for every
	/// other value.
	SmallVector<const Value *, 4> arms (const Value *v, WalkState &state) const
	{
		const auto *load = dyn_cast<LoadInst> (v);

		if (load == nullptr)
			return {};

		return matching_field_stores (*load, rule, state.visiting, state.budget);
	}

	/// Folds one arm into \p acc. Returns false, and settles the merge at no
	/// class, when an arm names no class itself or when two arms disagree.
	bool fold (std::optional<Answer> &acc, Answer arm) const
	{
		if (arm.first == nullptr) {
			// A guess is one arm of a compare the caller writes, so an arm
			// with no class is one more path that compare covers. Skip it
			// rather than settle the merge.
			if (rule == ClassRule::guessed)
				return true;

			acc = no_class ();
			return false;
		}

		// Two arms that name two classes still settle the merge. One compare
		// picks one class, and picking either leaves the other arm's whole
		// count on the dispatch.
		if (acc && acc->first != arm.first) {
			acc = no_class ();
			return false;
		}

		acc = Answer{ arm.first, (!acc || acc->second) && arm.second };
		return true;
	}

private:
	static Answer no_class () { return { nullptr, false }; }

	const Function &f;
	ClassRule rule;
};

/// Reads the class \p v holds under \p rule, and whether that is the class it
/// is rather than a bound on it. No class where the walk found none.
std::pair<MonoClass *, bool>
class_of (const Value *v, const Function &f, ClassRule rule)
{
	ClassWalk walk (f, rule);
	WalkState state;

	return walk_value (v, walk, state).value_or (std::make_pair (nullptr, false));
}

} // namespace

void
mark_exact_class (Instruction &site, MonoClass *klass)
{
	LLVMContext &c = site.getContext ();

	site.setMetadata (exact_class_md, MDNode::get (c, { as_metadata (c, klass) }));
}

void
mark_parameter_classes (Function &f, ArrayRef<std::pair<unsigned, MonoClass *>> classes)
{
	if (classes.empty ())
		return;

	LLVMContext &c = f.getContext ();
	SmallVector<Metadata *, 8> pairs;

	for (const auto &entry : classes)
		pairs.push_back (MDNode::get (
			c, { ConstantAsMetadata::get (
				     ConstantInt::get (Type::getInt32Ty (c), entry.first)),
		             as_metadata (c, entry.second) }));

	f.setMetadata (param_classes_md, MDNode::get (c, pairs));
}

std::pair<MonoClass *, bool>
operand_class (const Value *v, const Function &f)
{
	return class_of (v, f, ClassRule::settled);
}

MonoClass *
exact_class (const Value *v, const Function &f)
{
	auto [klass, exact] = class_of (v, f, ClassRule::dereferenced);

	if (klass == nullptr || exact)
		return klass;

	/*
	 * An array is marked sealed and is still not exact. A slot admits every
	 * array of that rank with the same cast class, and each of those carries a
	 * vtable of its own, so `int[]`, `uint[]` and an array of an enum over int
	 * reach three different interface slots.
	 *
	 * Narrowing this to the elements the width fold leaves alone does not work
	 * either. An enum's cast class is its underlying type, and the loader takes
	 * that type off the first instance field without a check -
	 * `mono_class_is_valid_enum ()` has one caller, in `sre.c`. So an image can
	 * declare an enum over any type at all and put its array in the set.
	 * `mono/tests/array-devirt.cs` gates the answer.
	 */
	if (!m_class_is_sealed (klass) || m_class_get_rank (klass) != 0
	    || mono_class_is_marshalbyref (klass))
		return nullptr;

	return klass;
}

MonoClass *
guessed_class (const Value *v, const Function &f)
{
	// Every leaf this rule reads answers exactly, so a merge of them does too.
	return class_of (v, f, ClassRule::guessed).first;
}

FieldValues
field_load_values (const LoadInst &load)
{
	SmallPtrSet<const Value *, 8> visiting { &load };
	unsigned budget = merge_walk_budget;
	FieldValues answer { {}, true };
	SmallVector<const Value *, 4> stores = matching_field_stores (
		load, ClassRule::guessed, visiting, budget, &answer.complete);

	for (const Value *v : stores)
		if (!isa<ConstantPointerNull> (v))
			answer.values.push_back (v);

	return answer;
}

void
mark_delegate_target (Instruction &site, MonoMethod *target)
{
	LLVMContext &c = site.getContext ();

	site.setMetadata (delegate_target_md, MDNode::get (c, { as_metadata (c, target) }));
}

MonoMethod *
delegate_target (const Value *v)
{
	const auto *site = dyn_cast<Instruction> (strip_casts (v));

	if (site == nullptr)
		return nullptr;

	if (MonoMethod *marked = marked_with (*site, delegate_target_md))
		return marked;

	MonoObject *held = initonly_static_read (site);

	return held != nullptr ? settled_delegate_target_of (held) : nullptr;
}

} // namespace mono

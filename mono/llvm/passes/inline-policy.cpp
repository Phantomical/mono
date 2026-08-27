#include "inline-policy.hpp"

#include "cast-func.hpp"
#include "compile-state.hpp"
#include "fold-cast.hpp"
#include "method-symbols.hpp"
#include "operand-class.hpp"
#include "strip-casts.hpp"
#include "vtable-func.hpp"
#include "vtable-snapshot.hpp"

#include "mono/metadata/abi-details.h"
#include "mono/metadata/class-internals.h"

#include <llvm/ADT/APInt.h>
#include <llvm/ADT/SmallVector.h>
#include <llvm/Analysis/CaptureTracking.h>
#include <llvm/Analysis/ConstantFolding.h>
#include <llvm/IR/DataLayout.h>
#include <llvm/IR/Attributes.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/GlobalValue.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/InstrTypes.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Operator.h>
#include <llvm/Support/CommandLine.h>

#include <algorithm>

using namespace llvm;

namespace mono {
namespace {

cl::opt<bool> ImplicitNullCheckFree (
	"mono-inline-implicit-null-free", cl::Hidden, cl::init (true),
	cl::desc ("Leave the raising arm of a folded null check out of a callee's "
	          "cost"));

cl::opt<bool> DispatchIsALoad (
	"mono-inline-dispatch-is-a-load", cl::Hidden, cl::init (true),
	cl::desc ("Price a dispatch read as the load it lowers to rather than as a "
	          "call"));

cl::opt<bool> FoldVTableFields (
	"mono-inline-fold-vtable-fields", cl::Hidden, cl::init (true),
	cl::desc ("Answer a read of the class, type or rank a vtable snapshot "
	          "states"));

cl::opt<bool> FoldVTableSlots (
	"mono-inline-fold-vtable-slots", cl::Hidden, cl::init (true),
	cl::desc ("Answer a read of a dispatch slot a vtable snapshot states"));

cl::opt<bool> AnswerTypeTests (
	"mono-inline-answer-casts", cl::Hidden, cl::init (true),
	cl::desc ("Answer a type test from the class the call site settled its "
	          "operand to"));

/*
 * Each bonus below counts the calls a fold removes, times what the model
 * charges for one call. A dispatch DevirtualizePass then resolves becomes a
 * direct call the simplification behind the inliner can fold again, and an
 * allocation SROA scalarizes takes its allocator call with it.
 *
 * `mono-inline-call-penalty` sets that per-call charge, so a change to it
 * rescales every bonus here.
 */
cl::opt<int> DevirtualizeReturnBonus (
	"mono-inline-devirt-return-bonus", cl::Hidden, cl::init (100),
	cl::desc ("Threshold bonus for a callee that returns an object whose class "
	          "the caller then dispatches on"));

cl::opt<int> DevirtualizeArgumentBonus (
	"mono-inline-devirt-arg-bonus", cl::Hidden, cl::init (50),
	cl::desc ("Threshold bonus for a callee that dispatches on a parameter the "
	          "site fills with an object of a named class"));

cl::opt<int> ScalarizeArgumentBonus (
	"mono-inline-scalarize-arg-bonus", cl::Hidden, cl::init (50),
	cl::desc ("Threshold bonus for a callee that does not capture a parameter "
	          "the site fills with a fresh allocation"));

/// Whether \p v is an object this compile allocated under a class it names.
///
/// `emit_object_alloc ()` stores the vtable into the object's first word right
/// behind the allocation, and that store states the class in the IR. A class
/// whose allocation can return a transparent proxy gets no such store, because
/// the object returned then carries the proxy's vtable.
///
/// Reading the store rather than the alloc kind gives the same result under
/// either collector. Boehm has no managed allocator
/// (`mono_gc_get_managed_allocator ()` returns NULL there), so its allocations
/// take the slow path, which carries no alloc kind for `erasable_allocation ()`
/// below to read. Both paths take the vtable as an argument and both get the
/// store.
bool
allocated_under_a_named_class (const Value *v)
{
	const Value *object = v->stripPointerCasts ();

	if (!isa<CallBase> (object))
		return false;

	for (const User *user : object->users ()) {
		const auto *store = dyn_cast<StoreInst> (user);

		if (store != nullptr && isa<GlobalValue> (strip_casts (store->getValueOperand ()))
		    && store->getPointerOperand ()->stripPointerCasts () == object)
			return true;
	}

	return false;
}

/// Whether LLVM can erase \p v once nothing reads it.
///
/// LLVM erases the call only if the allocator carries an alloc kind. The
/// translator marks one on a managed allocator, and only SGen has one.
bool
erasable_allocation (const Value *v)
{
	const auto *call = dyn_cast<CallBase> (v->stripPointerCasts ());
	const Function *allocator = call != nullptr ? call->getCalledFunction () : nullptr;

	return allocator != nullptr && allocator->hasFnAttribute (Attribute::AllocKind);
}

/// Whether \p f returns an object it allocated under a class it names.
bool
returns_a_named_allocation (const Function &f)
{
	for (const BasicBlock &block : f) {
		const auto *ret = dyn_cast<ReturnInst> (block.getTerminator ());

		if (ret != nullptr && ret->getReturnValue () != nullptr
		    && allocated_under_a_named_class (ret->getReturnValue ()))
			return true;
	}

	return false;
}

/// Whether \p vtable is the vtable of \p dispatched_on, read rather than named.
///
/// A vtable operand that is already a global is resolved, and folding a body in
/// front of such a site changes nothing. MonoObject holds its vtable first, so
/// the read is a load of the object's own address once the zero offset is
/// folded away.
bool
reads_the_vtable_of (const Value *vtable, const Value *dispatched_on)
{
	const auto *read = dyn_cast<LoadInst> (vtable->stripPointerCasts ());

	return read != nullptr
	       && read->getPointerOperand ()->stripPointerCasts () == dispatched_on;
}

/// Whether a site in \p f reads a dispatch table out of \p object and cannot
/// name the method in the slot.
///
/// An ordinary virtual site is the plain load of the slot, which is what folds
/// against a vtable a compile states. The declarations are what is left: an
/// interface, a virtual generic, and a method the runtime could give no slot.
bool
dispatches_unresolved_on (const Value *object, const Function &f)
{
	const Module *m = f.getParent ();
	const Value *dispatched_on = object->stripPointerCasts ();
	const DataLayout &dl = m->getDataLayout ();

	for (StringRef name : { vtable_func_name, imt_func_name, vtable_gfunc_name }) {
		const Function *decl = m->getFunction (name);

		if (decl == nullptr)
			continue;

		for (const User *user : decl->users ()) {
			const auto *site = dyn_cast<CallBase> (user);

			if (site == nullptr || site->getFunction () != &f
			    || site->arg_size () < 1)
				continue;
			if (reads_the_vtable_of (site->getArgOperand (0), dispatched_on))
				return true;
		}
	}

	for (const BasicBlock &block : f) {
		for (const Instruction &instruction : block) {
			const auto *entry = dyn_cast<LoadInst> (&instruction);

			if (entry == nullptr || !entry->getType ()->isPointerTy ())
				continue;

			APInt at (64, 0);
			const Value *base =
				entry->getPointerOperand ()->stripAndAccumulateConstantOffsets (
					dl, at, /*AllowNonInbounds=*/true);

			// The slots are the last field, so an offset short of them is one
			// of the facts a snapshot states rather than a dispatch.
			if (at.uge (MONO_STRUCT_OFFSET (MonoVTable, vtable))
			    && reads_the_vtable_of (base, dispatched_on))
				return true;
		}
	}

	return false;
}

/// The vtable snapshot \p object's allocation stored into its first word, or
/// null where this cannot say what stands there.
///
/// `emit_object_alloc ()` writes that word once, behind an allocation nothing
/// else holds yet, so a read of it anywhere below answers with what the store
/// put there. That is the same fact `DevirtualizePass` stands on, and it is
/// what lets a walk with no memory model follow one store.
///
/// Null covers a class whose allocation can return a transparent proxy, which
/// gets no such store, and a class with no snapshot at all.
GlobalVariable *
stored_vtable_snapshot (Value *object, const DataLayout &dl)
{
	object = object->stripPointerCasts ();

	if (!isa<CallBase> (object))
		return nullptr;

	SmallVector<User *, 8> work (object->users ());
	GlobalVariable *found = nullptr;

	while (!work.empty ()) {
		User *user = work.pop_back_val ();

		// An opaque pointer makes a getelementptr the only address arithmetic,
		// so following the ones that move nowhere reaches every store that can
		// name the first word.
		if (auto *gep = dyn_cast<GEPOperator> (user)) {
			if (gep->hasAllZeroIndices ())
				work.append (user->user_begin (), user->user_end ());
			continue;
		}

		auto *store = dyn_cast<StoreInst> (user);

		if (store == nullptr)
			continue;

		APInt at (64, 0);

		if (store->getPointerOperand ()->stripAndAccumulateConstantOffsets (
			    dl, at, /*AllowNonInbounds=*/true) != object
		    || at != 0)
			continue;

		auto *snapshot = dyn_cast<GlobalVariable> (
			const_cast<Value *> (strip_casts (store->getValueOperand ())));

		// A store this cannot read leaves the word unknown, and two that
		// disagree leave it unknown as well.
		if (snapshot == nullptr || !snapshot->hasMetadata (vtable_snapshot_metadata)
		    || !snapshot->hasDefinitiveInitializer ())
			return nullptr;
		if (found != nullptr && found != snapshot)
			return nullptr;

		found = snapshot;
	}

	return found;
}

/// The class the IR gives \p v, and whether that is the class it is rather than
/// a bound on it.
///
/// \p walked is the function being weighed, and it is what answers for a value
/// the call site settled nothing about.
///
/// `operand_class ()` reads a parameter's class off the function that declares
/// it, so the function it is handed has to own the value. Handing it the caller
/// for a callee's argument reads whatever class the caller declares at the same
/// index, which is a wrong answer that looks like a right one.
std::pair<MonoClass *, bool>
settled_class (Value *v, const Function &walked, SettledValue settled)
{
	v = const_cast<Value *> (strip_casts (v));

	if (Value *caller_side = settled (v)) {
		if (const auto *arg = dyn_cast<Argument> (caller_side))
			return operand_class (caller_side, *arg->getParent ());
		if (const auto *made = dyn_cast<Instruction> (caller_side))
			return operand_class (caller_side, *made->getFunction ());
	}

	return operand_class (v, walked);
}

} // namespace

bool
lowers_to_a_load (const Function &f)
{
	if (!DispatchIsALoad)
		return false;

	// LowerVTableFuncPass writes one load back for each of these, whatever the
	// slot is and whichever table it sits in.
	StringRef name = f.getName ();

	return name == vtable_func_name || name == imt_func_name
	       || name == vtable_gfunc_name;
}

/*
 * Word zero of a MonoObject and word zero of a MonoVTable are both at offset
 * zero, so the offset alone cannot tell the two reads apart. What does
 * is the base: this is the only thing that settles a value to a snapshot, so a
 * base that is one is a vtable, and a base that is not is an object.
 */
Value *
folded_vtable_read (LoadInst &load, SettledValue settled)
{
	if (!FoldVTableFields && !FoldVTableSlots)
		return nullptr;

	const DataLayout &dl = load.getModule ()->getDataLayout ();
	APInt at (64, 0);
	Value *base = load.getPointerOperand ()->stripAndAccumulateConstantOffsets (
		dl, at, /*AllowNonInbounds=*/true);

	if (Value *caller_side = settled (base))
		base = caller_side->stripAndAccumulateConstantOffsets (
			dl, at, /*AllowNonInbounds=*/true);

	if (auto *snapshot = dyn_cast<GlobalVariable> (base)) {
		if (!snapshot->hasMetadata (vtable_snapshot_metadata)
		    || !snapshot->hasDefinitiveInitializer ())
			return nullptr;

		uint64_t offset = at.getZExtValue ();
		bool slot = offset >= MONO_SIZEOF_VTABLE;

		if (!(slot ? FoldVTableSlots : FoldVTableFields))
			return nullptr;
		if (!vtable_snapshot_states (offset, dl.getTypeStoreSize (load.getType ()),
		                             vtable_snapshot_slots (*snapshot)))
			return nullptr;

		return ConstantFoldLoadFromConstPtr (snapshot, load.getType (), at, dl);
	}

	if (at != MONO_STRUCT_OFFSET (MonoObject, vtable) || !load.getType ()->isPointerTy ())
		return nullptr;

	return stored_vtable_snapshot (base, dl);
}

Value *
folded_type_test (CallBase &call, SettledValue settled)
{
	const Function *decl = call.getCalledFunction ();

	if (!AnswerTypeTests || decl == nullptr)
		return nullptr;

	StringRef name = decl->getName ();
	bool raises = name == cast_castclass_name;

	if (!raises && name != cast_isinst_name)
		return nullptr;

	/*
	 * Both classes are pointers the translator wrote into this compile's own
	 * metadata, so they mean nothing to anything reading the module later.
	 */
	if (current_compile ().domain == nullptr)
		return nullptr;

	auto *named = dyn_cast<GlobalValue> (call.getArgOperand (1));
	MonoClass *target = named != nullptr ? marked_class (*named) : nullptr;
	Value *object = call.getArgOperand (0);
	std::pair<MonoClass *, bool> held =
		settled_class (object, *call.getFunction (), settled);

	switch (cast_answer (target, held.first, held.second)) {
	case CastAnswer::Yes:
		return object;
	case CastAnswer::No:
		// castclass raises where isinst answers null, and a throw is not a
		// value the walk can carry. The site keeps its cost.
		return raises ? nullptr : ConstantPointerNull::get (
			               cast<PointerType> (call.getType ()));
	case CastAnswer::Unknown:
		return nullptr;
	}

	return nullptr;
}

BasicBlock *
implicit_null_check_successor (const BranchInst &branch)
{
	if (!ImplicitNullCheckFree || !branch.isConditional ()
	    || branch.getMetadata (LLVMContext::MD_make_implicit) == nullptr)
		return nullptr;

	const auto *test = dyn_cast<ICmpInst> (branch.getCondition ());

	if (test == nullptr
	    || (!isa<ConstantPointerNull> (test->getOperand (0))
	        && !isa<ConstantPointerNull> (test->getOperand (1))))
		return nullptr;

	/*
	 * ImplicitNullChecks folds the test into the dereference in the arm that
	 * survives, and leaves the other one standing as the handler its fault map
	 * names. Mono reads no fault map: it raises NullReferenceException from the
	 * faulting instruction instead (mono_is_addr_implicit_null_check ()), so
	 * no run arrives in that arm. The arms of two checks in one method raise
	 * the same way and the tail merge leaves one block for them.
	 */
	switch (test->getPredicate ()) {
	case ICmpInst::ICMP_EQ:
		return branch.getSuccessor (1);
	case ICmpInst::ICMP_NE:
		return branch.getSuccessor (0);
	default:
		return nullptr;
	}
}

int
call_site_bonus (const CallBase &call, const Function &callee)
{
	const Function *caller = call.getCaller ();

	if (caller == nullptr || callee.isDeclaration ())
		return 0;

	int bonus = 0;

	// The caller dispatches on what this call returns and cannot name the
	// target. The callee returns an object it allocated, so the fold puts a
	// class where the dispatch reads a pointer.
	if (dispatches_unresolved_on (&call, *caller)
	    && returns_a_named_allocation (callee))
		bonus += DevirtualizeReturnBonus;

	unsigned shared =
		std::min<unsigned> (call.arg_size (), callee.arg_size ());

	for (unsigned i = 0; i < shared; i++) {
		if (!allocated_under_a_named_class (call.getArgOperand (i)))
			continue;

		const Argument *param = callee.getArg (i);

		// The class arrives with the argument, so a dispatch the body cannot
		// resolve on its own gets an operand once the body is folded in.
		if (dispatches_unresolved_on (param, callee))
			bonus += DevirtualizeArgumentBonus;

		/*
		 * SROA scalarizes an allocation only when it can see every access, and
		 * a call hides the accesses inside the callee. The fold uncovers them
		 * for a parameter the body does not capture.
		 *
		 * The test asks about provenance alone, because taking the address does
		 * not keep an object in memory. A dereference of the argument compares
		 * it against null first, and LLVM counts that comparison as a capture
		 * of the address.
		 */
		if (erasable_allocation (call.getArgOperand (i))
		    && capturesNothing (PointerMayBeCaptured (
			    param, /*ReturnCaptures=*/true, CaptureComponents::Provenance)))
			bonus += ScalarizeArgumentBonus;
	}

	return bonus;
}

} // namespace mono

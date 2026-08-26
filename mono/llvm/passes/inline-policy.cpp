#include "inline-policy.hpp"

#include "strip-casts.hpp"
#include "vtable-func.hpp"

#include <llvm/Analysis/CaptureTracking.h>
#include <llvm/IR/Attributes.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/GlobalValue.h>
#include <llvm/IR/InstrTypes.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/CommandLine.h>

#include <algorithm>

using namespace llvm;

namespace mono {
namespace {

cl::opt<bool> ImplicitNullCheckFree (
	"mono-inline-implicit-null-free", cl::Hidden, cl::init (true),
	cl::desc ("Leave the raising arm of a folded null check out of a callee's "
	          "cost"));

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

/// Whether a site in \p f reads a dispatch table out of \p object and cannot
/// name the method in the slot.
///
/// A site whose vtable operand is already a global is resolved, and folding a
/// body in front of it changes nothing.
bool
dispatches_unresolved_on (const Value *object, const Function &f)
{
	const Module *m = f.getParent ();
	const Value *dispatched_on = object->stripPointerCasts ();

	for (StringRef name : { vtable_func_name, imt_func_name }) {
		const Function *decl = m->getFunction (name);

		if (decl == nullptr)
			continue;

		for (const User *user : decl->users ()) {
			const auto *site = dyn_cast<CallBase> (user);

			if (site == nullptr || site->getFunction () != &f
			    || site->arg_size () < 1)
				continue;

			const Value *vtable = site->getArgOperand (0)->stripPointerCasts ();

			if (isa<GlobalValue> (vtable))
				continue;

			// MonoObject holds its vtable first, so the read is a load of the
			// object's own address once the zero offset is folded away.
			const auto *read = dyn_cast<LoadInst> (vtable);

			if (read != nullptr
			    && read->getPointerOperand ()->stripPointerCasts () == dispatched_on)
				return true;
		}
	}

	return false;
}

} // namespace

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

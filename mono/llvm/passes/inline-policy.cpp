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
 * Each bonus below is a count of calls the fold is expected to take away, times
 * what the model charges for one. Calls are the unit the win arrives in: a
 * dispatch this lets DevirtualizePass answer becomes a direct call the
 * simplification behind the inliner can fold again, and an allocation SROA
 * scalarizes takes its allocator call with it.
 *
 * `mono-inline-call-penalty` is the same charge from the other side, so moving
 * it moves what these are worth.
 */
cl::opt<int> DevirtualizeReturnBonus (
	"mono-inline-devirt-return-bonus", cl::Hidden, cl::init (100),
	cl::desc ("Threshold bonus for a callee that answers with an object whose "
	          "class the caller then dispatches on"));

cl::opt<int> DevirtualizeArgumentBonus (
	"mono-inline-devirt-arg-bonus", cl::Hidden, cl::init (50),
	cl::desc ("Threshold bonus for a callee that dispatches on a parameter the "
	          "site passes an object of a named class in"));

cl::opt<int> ScalarizeArgumentBonus (
	"mono-inline-scalarize-arg-bonus", cl::Hidden, cl::init (50),
	cl::desc ("Threshold bonus for a callee that does not capture a parameter "
	          "the site passes a fresh allocation in"));

/// Whether \p v is an object this compile allocated under a class it names.
///
/// `emit_object_alloc ()` stores the vtable into the object's first word right
/// behind the allocation, and that store is what states the class in the IR. A
/// class whose allocation can answer with a transparent proxy gets no such
/// store, because what comes back then carries the proxy's vtable.
///
/// Reading the store rather than the alloc kind is what makes this answer the
/// same under either collector. Boehm has no managed allocator
/// (`mono_gc_get_managed_allocator ()` answers null there), so its allocations
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
/// The alloc kind the translator marks a managed allocator with is what lets it
/// go, and only SGen has one to mark.
bool
erasable_allocation (const Value *v)
{
	const auto *call = dyn_cast<CallBase> (v->stripPointerCasts ());
	const Function *allocator = call != nullptr ? call->getCalledFunction () : nullptr;

	return allocator != nullptr && allocator->hasFnAttribute (Attribute::AllocKind);
}

/// Whether \p f answers with an object it allocated under a class it names.
bool
answers_with_a_named_allocation (const Function &f)
{
	for (const BasicBlock &block : f) {
		const auto *ret = dyn_cast<ReturnInst> (block.getTerminator ());

		if (ret != nullptr && ret->getReturnValue () != nullptr
		    && allocated_under_a_named_class (ret->getReturnValue ()))
			return true;
	}

	return false;
}

/// Whether a site in \p in reads a dispatch table out of \p object and cannot
/// name what stands in the slot.
///
/// A site whose vtable operand is already a global has its answer, and a body
/// folded in front of it buys nothing.
bool
dispatches_unresolved_on (const Value *object, const Function &in)
{
	const Module *m = in.getParent ();
	const Value *dispatched_on = object->stripPointerCasts ();

	for (StringRef name : { vtable_func_name, imt_func_name }) {
		const Function *decl = m->getFunction (name);

		if (decl == nullptr)
			continue;

		for (const User *user : decl->users ()) {
			const auto *site = dyn_cast<CallBase> (user);

			if (site == nullptr || site->getFunction () != &in
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

	// The caller asks the answer for a method and cannot say which, and the
	// body it is weighing allocates what it answers with. Folding it in puts a
	// class where the site reads a pointer.
	if (dispatches_unresolved_on (&call, *caller)
	    && answers_with_a_named_allocation (callee))
		bonus += DevirtualizeReturnBonus;

	unsigned shared =
		std::min<unsigned> (call.arg_size (), callee.arg_size ());

	for (unsigned i = 0; i < shared; i++) {
		if (!allocated_under_a_named_class (call.getArgOperand (i)))
			continue;

		const Argument *param = callee.getArg (i);

		// The class travels in with the argument, so a dispatch the body cannot
		// answer on its own has an operand once the body is here.
		if (dispatches_unresolved_on (param, callee))
			bonus += DevirtualizeArgumentBonus;

		/*
		 * SROA scalarizes an allocation whose accesses it can all see, and a
		 * call is what hides those accesses, so a parameter the body does not
		 * capture is one the fold hands over.
		 *
		 * Provenance alone, because taking the address is not what keeps an
		 * object in memory. A dereference of the argument compares it against
		 * null first, and LLVM counts that comparison as a capture of the
		 * address.
		 */
		if (erasable_allocation (call.getArgOperand (i))
		    && capturesNothing (PointerMayBeCaptured (
			    param, /*ReturnCaptures=*/true, CaptureComponents::Provenance)))
			bonus += ScalarizeArgumentBonus;
	}

	return bonus;
}

} // namespace mono

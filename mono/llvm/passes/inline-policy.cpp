#include "inline-policy.hpp"

#include "analysis/operand-class.hpp"
#include "analysis/strip-casts.hpp"
#include "analysis/vtable-info.hpp"
#include "cast-func.hpp"
#include "compile-state.hpp"
#include "fold-cast.hpp"
#include "method-symbols.hpp"
#include "tier-counter.hpp"
#include "vtable-func.hpp"

#include "mono/metadata/abi-details.h"
#include "mono/metadata/class-internals.h"

#include <llvm/ADT/APInt.h>
#include <llvm/ADT/SmallVector.h>
#include <llvm/Analysis/BlockFrequencyInfo.h>
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
#include <llvm/Support/MathExtras.h>

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
	cl::desc ("Fold a read of the class, type or rank off a vtable the call site "
	          "settled"));

cl::opt<bool> AnswerTypeTests (
	"mono-inline-answer-casts", cl::Hidden, cl::init (true),
	cl::desc ("Answer a type test from the class the call site settled its "
	          "operand to"));

/*
 * Each bonus below counts the calls a fold removes, times what the model
 * charges for one call. A dispatch fold_dispatch_sites () then resolves becomes a
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
	"mono-inline-scalarize-arg-bonus", cl::Hidden, cl::init (150),
	cl::desc ("Threshold bonus for a callee that does not capture a parameter "
	          "the site fills with a fresh allocation"));

cl::opt<bool> RankSitesInPromotedBody (
	"mono-inline-tier2-site-heat", cl::Hidden, cl::init (true),
	cl::desc ("Rank a call site against the entry count of the promoted body it "
	          "is in, rather than against the module's profile summary"));

cl::opt<unsigned> PromotedHotMultiple (
	"mono-inline-tier2-hot-multiple", cl::Hidden, cl::init (5),
	cl::desc ("Times the entry count a block must run before a site in it is hot "
	          "in a promoted body"));

cl::opt<unsigned> PromotedColdPercent (
	"mono-inline-tier2-cold-percent", cl::Hidden, cl::init (2),
	cl::desc ("Share of the entry count, as a percentage, a block must run to be "
	          "more than cold in a promoted body"));

/// Whether \p v is an object this compile allocated under a class it names.
///
/// `emit_object_alloc ()` stores the vtable into the object's first word right
/// behind the allocation, and that store states the class in the IR. A class
/// whose allocation can return a transparent proxy gets no such store, because
/// the object returned then carries the proxy's vtable.
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
/// LLVM erases the call only if it carries an alloc kind, and `mono.alloc.*`
/// carries one for every class an erasure is invisible on. That declaration is
/// the same under either collector, so this answers the same as well.
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
/// front of such a site changes nothing.
bool
reads_the_vtable_of (const Value *vtable, const Value *dispatched_on)
{
	Value *object =
		object_vtable_read (const_cast<Value *> (vtable->stripPointerCasts ()));

	return object != nullptr && object->stripPointerCasts () == dispatched_on;
}

/// Whether a site in \p f reads a dispatch table out of \p object.
///
/// Every dispatch is one of the three declarations, so this reads their users
/// rather than walking the body.
bool
dispatches_unresolved_on (const Value *object, const Function &f)
{
	const Module *m = f.getParent ();
	const Value *dispatched_on = object->stripPointerCasts ();

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

	return false;
}

/// The vtable symbol \p object's allocation stored into its first word, or null
/// where this cannot say what stands there.
///
/// `emit_object_alloc ()` writes that word once, behind an allocation nothing
/// else holds yet, so a read of it anywhere below gives what the store put
/// there. That is the same fact `fold_dispatch_sites ()` stands on, and it is
/// what lets a walk with no memory model follow one store.
///
/// Null covers a class whose allocation can return a transparent proxy, which
/// gets no such store, and a class whose vtable symbol carries no info.
GlobalVariable *
stored_vtable (Value *object, const DataLayout &dl)
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

		auto *named = dyn_cast<GlobalVariable> (
			const_cast<Value *> (strip_casts (store->getValueOperand ())));

		// A store this cannot read leaves the word unknown, and two that
		// disagree leave it unknown as well.
		if (named == nullptr || !vtable_info (*named))
			return nullptr;
		if (found != nullptr && found != named)
			return nullptr;

		found = named;
	}

	return found;
}

/// The class the IR gives \p v, and whether that is the class it is rather than
/// a bound on it.
///
/// \p walked is the function being weighed, and it is what answers for a value
/// the call site settled nothing about.
///
/// `stated_class ()` reads a parameter's class off the function that declares
/// it, so the function it is handed has to own the value. Handing it the caller
/// for a callee's argument reads whatever class the caller declares at the same
/// index, which is a wrong answer that looks like a right one.
///
/// The leaf read rather than the merge walk, because the cost model runs with
/// no analysis manager and the value a site settled has its merges resolved
/// already.
std::pair<MonoClass *, bool>
settled_class (Value *v, const Function &walked, SettledValue settled)
{
	v = const_cast<Value *> (strip_casts (v));

	if (Value *caller_side = settled (v)) {
		if (const auto *arg = dyn_cast<Argument> (caller_side))
			return stated_class (caller_side, *arg->getParent ());
		if (const auto *made = dyn_cast<Instruction> (caller_side))
			return stated_class (caller_side, *made->getFunction ());
	}

	return stated_class (v, walked);
}

/// Whether the profile counted any block of \p f at \p bar or above.
bool
a_block_runs_at (const Function &f, BlockFrequencyInfo &bfi, uint64_t bar)
{
	for (const BasicBlock &bb : f)
		if (bfi.getBlockProfileCount (&bb).value_or (0) >= bar)
			return true;

	return false;
}

} // namespace

bool
lowers_to_a_load (const Function &f)
{
	if (!DispatchIsALoad)
		return false;

	// Each of these is one load once lower_vtable_reads () has run, whatever the
	// slot is and whichever table or field it names.
	StringRef name = f.getName ();

	return name == vtable_func_name || name == imt_func_name
	       || name == vtable_gfunc_name || name == vtable_klass_name
	       || name == vtable_type_name || name == vtable_rank_name;
}

Value *
folded_object_vtable (LoadInst &load, SettledValue settled)
{
	if (!FoldVTableFields)
		return nullptr;

	Value *object = object_vtable_read (&load);

	if (object == nullptr)
		return nullptr;

	object = const_cast<Value *> (strip_casts (object));

	if (Value *caller_side = settled (object))
		object = const_cast<Value *> (strip_casts (caller_side));

	/*
	 * This walks the store at an allocation rather than asking `exact_class ()`.
	 * Naming a class's vtable records an external and lays the class out, and a
	 * candidate this model goes on to refuse is owed neither.
	 */
	return stored_vtable (object, load.getModule ()->getDataLayout ());
}

Value *
folded_vtable_read (CallBase &call, SettledValue settled)
{
	const Function *decl = call.getCalledFunction ();

	if (!FoldVTableFields || decl == nullptr)
		return nullptr;

	StringRef name = decl->getName ();

	if (name != vtable_klass_name && name != vtable_type_name
	    && name != vtable_rank_name)
		return nullptr;

	Value *operand = const_cast<Value *> (strip_casts (call.getArgOperand (0)));

	if (Value *caller_side = settled (operand))
		operand = const_cast<Value *> (strip_casts (caller_side));

	auto *named = dyn_cast<GlobalObject> (operand);
	std::optional<VTableInfo> info =
		named != nullptr ? vtable_info (*named) : std::nullopt;

	if (!info)
		return nullptr;

	if (name == vtable_klass_name)
		return info->klass;
	if (name == vtable_type_name)
		return info->type;

	return ConstantInt::get (call.getType (), info->rank);
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

std::optional<SiteHeat>
tier2_site_heat (const CallBase &call, BlockFrequencyInfo *caller_bfi)
{
	const Function *caller = call.getCaller ();

	if (!RankSitesInPromotedBody || caller_bfi == nullptr || caller == nullptr
	    || !caller->hasFnAttribute (tier_counter_attribute))
		return std::nullopt;

	/*
	 * The profile counts rather than the block frequencies LLVM ranks a site by.
	 * Through the frequencies the two collectors ranked one shape differently,
	 * where the counts they gathered agreed. The counts are also what the decline
	 * trace prints, so a reader can check a verdict against the log.
	 */
	std::optional<uint64_t> site =
		caller_bfi->getBlockProfileCount (call.getParent ());
	std::optional<uint64_t> entry =
		caller_bfi->getBlockProfileCount (&caller->getEntryBlock ());

	// Without counts there is nothing to rank the site against, so LLVM decides.
	if (!site || !entry || *entry == 0)
		return std::nullopt;

	/*
	 * A body arrives here only once its tier-2 counter has run out. So a block is
	 * weighed against the body around it, not against the rest of the program.
	 * Cold is then the block that body hardly ever takes. LLVM applies that same
	 * rule where it has no summary to read.
	 */
	if (*site * 100 < *entry * std::min<unsigned> (PromotedColdPercent, 100))
		return SiteHeat::cold;

	uint64_t bar = SaturatingMultiply (*entry, uint64_t (PromotedHotMultiple));

	if (*site >= bar)
		return SiteHeat::hot;

	/*
	 * No block of the body runs much more often than the body is entered, so
	 * nothing in it stands out as the work. Every block it runs each time is
	 * then as hot as the body.
	 */
	if (*site >= *entry && !a_block_runs_at (*caller, *caller_bfi, bar))
		return SiteHeat::hot;

	return SiteHeat::ordinary;
}

} // namespace mono

/**
 * \file
 * \brief The capture walk that says whether an allocation stays inside its own
 * function.
 */

#include "escape.hpp"

#include "alloc-func.hpp"

#include <llvm/ADT/StringRef.h>
#include <llvm/Analysis/CaptureTracking.h>
#include <llvm/Analysis/ValueTracking.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/InstrTypes.h>
#include <llvm/IR/Instructions.h>
#include <llvm/Support/ModRef.h>

using namespace llvm;

namespace mono {
namespace {

/// What `PointerMayBeCaptured ()` reports each capturing use to.
class EscapeTracker : public CaptureTracker {
public:
	explicit EscapeTracker (function_ref<bool (CallBase &)> keeps_it_inside)
		: keeps_it_inside (keeps_it_inside)
	{
	}

	void tooManyUses () override { escaped = true; }

	Action captured (const Use *use, UseCaptureInfo info) override
	{
		/*
		 * Only provenance is a way to the object. A use that learns the
		 * address alone, such as a compare against null, says where the
		 * object is and gives nothing that can read it. The walk follows the
		 * result as well, because a use that hands the pointer back through
		 * its return value carries the provenance there.
		 */
		if (!capturesAnything (info.UseCC & CaptureComponents::Provenance))
			return Continue;

		auto *store = dyn_cast<StoreInst> (use->getUser ());

		// Operand zero is the value a store writes, which is where our
		// pointer leaves this walk's reach.
		if (store != nullptr && use->getOperandNo () == 0 && store->isSimple ()) {
			CallBase *holder = allocation_behind (store->getPointerOperand ());

			if (holder != nullptr && keeps_it_inside (*holder))
				return ContinueIgnoringReturn;
		}

		escaped = true;
		return Stop;
	}

	bool escaped = false;

private:
	function_ref<bool (CallBase &)> keeps_it_inside;
};

} // namespace

CallBase *
allocation_behind (Value *pointer)
{
	auto *call = dyn_cast<CallBase> (getUnderlyingObject (pointer));

	if (call == nullptr)
		return nullptr;

	const Function *callee = call->getCalledFunction ();

	if (callee == nullptr)
		return nullptr;

	StringRef name = callee->getName ();

	return name == alloc_object_name || name == alloc_vector_name
	                       || name == alloc_object_kept_name
	                       || name == alloc_vector_kept_name
	               ? call
	               : nullptr;
}

bool
allocation_escapes (CallBase &alloc, function_ref<bool (CallBase &)> keeps_it_inside)
{
	EscapeTracker tracker (keeps_it_inside);

	PointerMayBeCaptured (&alloc, &tracker);
	return tracker.escaped;
}

} // namespace mono

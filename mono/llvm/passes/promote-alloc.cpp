/**
 * \file
 * \brief The rewrite that puts an object with no way out into the frame.
 */

#include "promote-alloc.hpp"

#include "alloc-func.hpp"
#include "analysis/escape.hpp"
#include "builtins.hpp"

#include "analysis/operand-class.hpp"

#include "mono/metadata/abi-details.h"

#include <llvm/ADT/SmallVector.h>
#include <llvm/Analysis/LoopInfo.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/InstrTypes.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>

using namespace llvm;

namespace mono {
namespace {

/*
 * An object holds pointers, so both collectors hand one back on a pointer
 * boundary. A frame slot with that alignment gives the object what the heap gave
 * it, and asking the class for a wider one would read a guarantee no collector
 * here makes.
 */
Align
object_align ()
{
	return Align (TARGET_SIZEOF_VOID_P);
}

/// The size \p alloc makes, or none where the compile cannot read it.
///
/// A zero names a class whose layout this compile does not know, which
/// `alloc-func.hpp` states for the operand.
std::optional<uint64_t>
promotable_size (const CallInst &alloc)
{
	const auto *size = dyn_cast<ConstantInt> (alloc.getArgOperand (1));

	if (size == nullptr)
		return std::nullopt;

	uint64_t bytes = size->getZExtValue ();

	if (bytes == 0 || bytes > promote_alloc_limit)
		return std::nullopt;

	return bytes;
}

/// Puts \p alloc in the frame and hands its uses the slot.
AllocaInst *
promote (CallInst &alloc, uint64_t bytes)
{
	Function *f = alloc.getFunction ();
	IRBuilder<> entry (&*f->getEntryBlock ().getFirstInsertionPt ());
	AllocaInst *slot =
		entry.CreateAlloca (ArrayType::get (entry.getInt8Ty (), bytes), nullptr,
	                            "promoted");

	slot->setAlignment (object_align ());

	/*
	 * The class the site named moves to the slot. `leaf_operand_class ()`
	 * reads it off the instruction that answers with the object, so a
	 * dispatch this object settles keeps its answer.
	 */
	if (MDNode *klass = alloc.getMetadata (exact_class_md))
		slot->setMetadata (exact_class_md, klass);

	/*
	 * The zeroing stands where the allocation stood rather than beside the
	 * alloca. The alloca is made once for the frame, and the block this site
	 * is in can be entered on some paths and not others.
	 */
	IRBuilder<> site (&alloc);

	site.SetCurrentDebugLocation (alloc.getDebugLoc ());
	site.CreateMemSet (slot, site.getInt8 (0), site.getInt64 (bytes), object_align ());

	alloc.replaceAllUsesWith (slot);
	alloc.eraseFromParent ();
	return slot;
}

} // namespace

bool
promote_allocations (Function &f, const LoopInfo &loops)
{
	SmallVector<AllocaInst *, 4> slots;

	for (CallBase *site : builtin_sites (f, alloc_object_name)) {
		// An invoke names the pads its edges reach, and erasing one asks for
		// a repair of each. The site this pass is written for is a call.
		auto *alloc = dyn_cast<CallInst> (site);

		if (alloc == nullptr)
			continue;

		// One slot serves the whole frame, so a site a loop reaches would
		// hand every turn the object the turn before it is still using.
		if (loops.getLoopFor (alloc->getParent ()) != nullptr)
			continue;

		std::optional<uint64_t> bytes = promotable_size (*alloc);

		if (!bytes)
			continue;

		// Nothing vouches for a destination here. A store of the object into
		// another object keeps it on the heap, even where that object is
		// promoted as well, because the store outlives the frame if the
		// object holding it does.
		if (allocation_escapes (*alloc, [] (CallBase &) { return false; }))
			continue;

		slots.push_back (promote (*alloc, *bytes));
	}

	return !slots.empty ();
}

PreservedAnalyses
PromoteAllocationsPass::run (Function &f, FunctionAnalysisManager &fam)
{
	if (!promote_allocations (f, fam.getResult<LoopAnalysis> (f)))
		return PreservedAnalyses::all ();

	PreservedAnalyses preserved;

	// The rewrite moves no branch and splits no block.
	preserved.preserveSet<CFGAnalyses> ();
	return preserved;
}

} // namespace mono

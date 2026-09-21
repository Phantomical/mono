/** \file Rewrite surviving object-reference null checks into faulting reads. */

#include "null-check-fault.hpp"

#include "arch/arch.hpp"

#include <llvm/CodeGen/MachineBasicBlock.h>
#include <llvm/CodeGen/MachineFunction.h>
#include <llvm/CodeGen/MachineInstr.h>
#include <llvm/CodeGen/TargetInstrInfo.h>
#include <llvm/CodeGen/TargetSubtargetInfo.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Instruction.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/Support/ErrorHandling.h>

using namespace llvm;

namespace mono {
namespace {

/// The tag emit_null_check () sets alongside !make.implicit when the pointer
/// is certainly readable at offset 0 once it is not null.
constexpr StringRef object_reference_md = "mono.null.objref";

/// Use the check's location for the replacement instruction.
DebugLoc
branch_location (MachineBasicBlock &mbb)
{
	for (MachineInstr &mi : mbb.terminators ())
		if (mi.isConditionalBranch ())
			return mi.getDebugLoc ();

	return DebugLoc ();
}

/// Rewrites \p mbb's terminator when it is a still-standing, tagged check on
/// an object reference. Returns whether it changed anything.
bool
rewrite_block (MachineBasicBlock &mbb, const TargetInstrInfo &tii)
{
	const BasicBlock *bb = mbb.getBasicBlock ();
	if (bb == nullptr)
		return false;

	const Instruction *term = bb->getTerminator ();
	if (term->getMetadata (LLVMContext::MD_make_implicit) == nullptr
	    || term->getMetadata (object_reference_md) == nullptr)
		return false;

	using MachineBranchPredicate = TargetInstrInfo::MachineBranchPredicate;
	MachineBranchPredicate mbp;

	// A folded check no longer has a conditional branch to decode.
	if (arch::analyze_null_check_branch (mbb, tii, mbp))
		return false;

	// The front end emits an equality check; its true edge throws.
	if (!(mbp.LHS.isReg () && mbp.RHS.isImm () && mbp.RHS.getImm () == 0
	      && mbp.Predicate == MachineBranchPredicate::PRED_EQ))
		return false;

	// Do not remove a condition with other users.
	if (mbp.ConditionDef && !mbp.SingleUseCondition)
		return false;

	MachineBasicBlock *null_succ = mbp.TrueDest;
	MachineBasicBlock *not_null_succ = mbp.FalseDest;
	Register pointer = mbp.LHS.getReg ();
	DebugLoc dl = branch_location (mbb);

	if (tii.removeBranch (mbb) == 0)
		report_fatal_error ("mono: null-check-fault: analyzeBranchPredicate decoded "
		                    "a branch removeBranch did not find");

	if (mbp.ConditionDef)
		mbp.ConditionDef->eraseFromParent ();

	/* Keep the handler edge and let eh-gather associate it with its clause. */
	arch::emit_faulting_byte_read (mbb, mbb.end (), tii, pointer, null_succ, dl);

	// Preserve the not-null edge when it is not the layout successor.
	if (!mbb.isLayoutSuccessor (not_null_succ))
		tii.insertUnconditionalBranch (mbb, not_null_succ, dl);

	return true;
}

} // namespace

bool
MonoNullCheckFaultPass::runOnMachineFunction (MachineFunction &mf)
{
	const TargetInstrInfo *tii = mf.getSubtarget ().getInstrInfo ();
	bool changed = false;

	for (MachineBasicBlock &mbb : mf)
		changed |= rewrite_block (mbb, *tii);

	return changed;
}

char MonoNullCheckFaultPass::ID = 0;

} // namespace mono

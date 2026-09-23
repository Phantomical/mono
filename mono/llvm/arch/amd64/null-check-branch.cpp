/**
 * \file
 * \brief Recognize the compare and branch forms used for null checks.
 */

#include "arch/arch.hpp"

#include <llvm/ADT/STLExtras.h>
#include <llvm/ADT/SmallVector.h>
#include <llvm/ADT/StringRef.h>
#include <llvm/CodeGen/MachineBasicBlock.h>
#include <llvm/CodeGen/MachineFunction.h>
#include <llvm/CodeGen/MachineInstr.h>
#include <llvm/CodeGen/MachineOperand.h>
#include <llvm/CodeGen/TargetInstrInfo.h>
#include <llvm/CodeGen/TargetRegisterInfo.h>
#include <llvm/CodeGen/TargetSubtargetInfo.h>
#include <llvm/Support/ErrorHandling.h>

using namespace llvm;

namespace mono::arch {
namespace {

using MachineBranchPredicate = TargetInstrInfo::MachineBranchPredicate;

/// Find EFLAGS by name because the target register enum is private.
Register
eflags_register (const TargetRegisterInfo &tri)
{
	static const Register reg = [&] {
		for (unsigned i = 1, n = tri.getNumRegs (); i < n; ++i)
			if (tri.getName (i) == StringRef ("EFLAGS"))
				return Register (i);

		report_fatal_error ("mono: this LLVM's x86 target has no EFLAGS register - "
		                    "check this against the installed LLVM version");
	} ();

	return reg;
}

/// x86 Jcc condition-code values for JE and JNE.
constexpr int64_t cond_code_e = 4;
constexpr int64_t cond_code_ne = 5;

/// Whether \p opcode is the 64-bit `cmp reg, 0` form used for pointers.
bool
is_zero_compare_opcode (const TargetInstrInfo &tii, unsigned opcode)
{
	static const unsigned cmp64ri32 = [&] {
		for (unsigned i = 0, n = tii.getNumOpcodes (); i < n; ++i)
			if (tii.getName (i) == "CMP64ri32")
				return i;

		report_fatal_error ("mono: this LLVM's x86 target has no CMP64ri32 opcode - "
		                    "check this against the installed LLVM version");
	} ();

	return opcode == cmp64ri32;
}

/// Match `cmp reg, 0` followed by `je` or `jne`, using the same result format
/// as analyzeBranchPredicate (). Returns false on success.
bool
match_zero_compare_branch (MachineBasicBlock &mbb, const TargetInstrInfo &tii,
                           MachineBranchPredicate &mbp)
{
	SmallVector<MachineOperand, 4> cond;

	if (tii.analyzeBranch (mbb, mbp.TrueDest, mbp.FalseDest, cond,
	                       /*AllowModify=*/false))
		return true;

	if (cond.size () != 1 || mbp.TrueDest == nullptr)
		return true;

	if (mbp.FalseDest == nullptr)
		mbp.FalseDest = mbb.getNextNode ();

	const TargetRegisterInfo &tri = *mbb.getParent ()->getSubtarget ().getRegisterInfo ();
	Register eflags = eflags_register (tri);

	MachineInstr *condition_def = nullptr;
	bool single_use = true;

	// A conditional branch may be followed by an unconditional branch.
	for (MachineInstr &mi : llvm::reverse (mbb)) {
		if (mi.isTerminator ())
			continue;

		if (mi.modifiesRegister (eflags, &tri)) {
			condition_def = &mi;
			break;
		}

		if (mi.readsRegister (eflags, &tri))
			single_use = false;
	}

	if (condition_def == nullptr)
		return true;

	if (single_use) {
		for (MachineBasicBlock *succ : mbb.successors ())
			if (succ->isLiveIn (eflags))
				single_use = false;
	}

	int64_t cc = cond[0].getImm ();

	if ((cc != cond_code_e && cc != cond_code_ne)
	    || condition_def->getNumExplicitOperands () != 2
	    || !condition_def->getOperand (0).isReg ()
	    || !condition_def->getOperand (1).isImm ()
	    || condition_def->getOperand (1).getImm () != 0
	    || !is_zero_compare_opcode (tii, condition_def->getOpcode ()))
		return true;

	mbp.ConditionDef = condition_def;
	mbp.SingleUseCondition = single_use;
	mbp.LHS = condition_def->getOperand (0);
	mbp.RHS = MachineOperand::CreateImm (0);
	mbp.Predicate = cc == cond_code_ne ? MachineBranchPredicate::PRED_NE
	                                   : MachineBranchPredicate::PRED_EQ;
	return false;
}

} // namespace

bool
analyze_null_check_branch (MachineBasicBlock &mbb, const TargetInstrInfo &tii,
                           MachineBranchPredicate &mbp)
{
	if (!tii.analyzeBranchPredicate (mbb, mbp, /*AllowModify=*/false))
		return false;

	mbp = MachineBranchPredicate ();
	return match_zero_compare_branch (mbb, tii, mbp);
}

} // namespace mono::arch

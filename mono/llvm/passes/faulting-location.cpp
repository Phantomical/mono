/**
 * \file
 * \brief Putting back the debug location a folded null check lost.
 */

#include "faulting-location.hpp"

#include <llvm/CodeGen/MachineBasicBlock.h>
#include <llvm/CodeGen/MachineFunction.h>
#include <llvm/CodeGen/MachineInstr.h>
#include <llvm/CodeGen/TargetOpcodes.h>
#include <llvm/IR/Function.h>
#include <llvm/Support/ErrorHandling.h>

using namespace llvm;

namespace mono {
namespace {

/**
 * The block a faulting instruction hands control to when it faults.
 *
 * That is the block the explicit check branched to when the pointer was null,
 * so it holds the throw the check made, at the IL offset of the dereference the
 * check protected. ImplicitNullChecks writes it as the one machine-block operand
 * of the instruction.
 */
MachineBasicBlock *
handler_of (const MachineInstr &mi)
{
	for (const MachineOperand &mo : mi.operands ())
		if (mo.isMBB ())
			return mo.getMBB ();

	report_fatal_error ("mono: FAULTING_OP names no handler block - LLVM invariant broken");
}

/**
 * The block the check branched to when the pointer was not null.
 *
 * The dereference the fold took came out of that block, so it is where the
 * location the faulting instruction wants was. The fold leaves the check block
 * with the two successors it had.
 */
MachineBasicBlock *
not_null_of (const MachineInstr &mi, const MachineBasicBlock *handler)
{
	for (MachineBasicBlock *succ : mi.getParent ()->successors ())
		if (succ != handler)
			return succ;

	return nullptr;
}

/// The first location in \p mbb, which is the one the block was entered at.
DebugLoc
opening_location (const MachineBasicBlock *mbb)
{
	if (mbb == nullptr)
		return DebugLoc ();

	for (const MachineInstr &mi : *mbb)
		if (const DebugLoc &loc = mi.getDebugLoc ())
			return loc;

	return DebugLoc ();
}

} // namespace

bool
MonoFaultingLocationPass::runOnMachineFunction (MachineFunction &mf)
{
	bool located = false;

	for (MachineBasicBlock &mbb : mf) {
		for (MachineInstr &mi : mbb) {
			if (mi.getOpcode () != TargetOpcode::FAULTING_OP || mi.getDebugLoc ())
				continue;

			MachineBasicBlock *handler = handler_of (mi);
			DebugLoc loc = opening_location (not_null_of (mi, handler));

			/*
			 * A throw block SimplifyCFG merged with the other checks'
			 * throw blocks carries no location, because the locations
			 * disagreed. So it is the second choice rather than the
			 * first, and a fold has no location at all when both blocks
			 * have none. The instruction then keeps the offset in effect
			 * where it sits, which is what the gather reads.
			 */
			if (!loc)
				loc = opening_location (handler);
			if (!loc)
				continue;

			mi.setDebugLoc (loc);
			located = true;
		}
	}

	return located;
}

char MonoFaultingLocationPass::ID = 0;

} // namespace mono

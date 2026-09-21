/** \file Emit an x86 memory read used only to fault on a null pointer. */

#include "arch/arch.hpp"

#include <llvm/CodeGen/FaultMaps.h>
#include <llvm/CodeGen/MachineBasicBlock.h>
#include <llvm/CodeGen/MachineInstrBuilder.h>
#include <llvm/CodeGen/TargetInstrInfo.h>
#include <llvm/CodeGen/TargetOpcodes.h>
#include <llvm/Support/ErrorHandling.h>

using namespace llvm;

namespace mono::arch {
namespace {

/// Find CMP8mi without including the x86 target's private opcode header.
unsigned
cmp8mi_opcode (const TargetInstrInfo &tii)
{
	static const unsigned opcode = [&] {
		for (unsigned i = 0, n = tii.getNumOpcodes (); i < n; ++i)
			if (tii.getName (i) == "CMP8mi")
				return i;

		report_fatal_error ("mono: this LLVM's x86 target has no CMP8mi opcode - "
		                    "check this against the installed LLVM version");
	} ();

	return opcode;
}

} // namespace

void
emit_faulting_byte_read (MachineBasicBlock &mbb, MachineBasicBlock::iterator at,
                         const TargetInstrInfo &tii, Register pointer,
                         MachineBasicBlock *handler, DebugLoc dl)
{
	/* FAULTING_OP carries the handler and the wrapped compare operands. */
	BuildMI (mbb, at, dl, tii.get (TargetOpcode::FAULTING_OP))
		.addReg (0, RegState::Define)
		.addImm (FaultMaps::FaultingLoad)
		.addMBB (handler)
		.addImm (cmp8mi_opcode (tii))
		.addReg (pointer)
		.addImm (1)
		.addReg (0)
		.addImm (0)
		.addReg (0)
		.addImm (0);
}

} // namespace mono::arch

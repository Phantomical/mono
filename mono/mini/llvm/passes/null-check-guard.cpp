/**
 * \file
 * null-check-guard.cpp - MonoNullCheckGuardPass. See the header for why this
 * exists.
 */

/*
 * Same reason engine.cpp drops mono's PIC macro: libtool compiles this TU with
 * -DPIC, and LLVM uses `PIC` as an identifier (PassInstrumentationCallbacks),
 * so the macro would rewrite it and break a header.
 */
#ifdef PIC
#undef PIC
#endif

#include "null-check-guard.hpp"

#include <llvm/CodeGen/MachineBasicBlock.h>
#include <llvm/CodeGen/MachineFunction.h>
#include <llvm/CodeGen/MachineInstr.h>
#include <llvm/CodeGen/MachineMemOperand.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Instruction.h>
#include <llvm/IR/LLVMContext.h>

using namespace llvm;

namespace mono {

namespace {

/*
 * True if any instruction in BB carries a memory operand with neither a
 * Value nor a PseudoSourceValue - the exact shape
 * ImplicitNullChecks::areMemoryOpsAliased() cannot handle.
 */
bool
has_opaque_memory_op (const MachineBasicBlock &bb)
{
	for (const MachineInstr &mi : bb) {
		for (const MachineMemOperand *mmo : mi.memoperands ()) {
			if (!mmo->getValue () && !mmo->getPseudoValue ())
				return true;
		}
	}
	return false;
}

} // namespace

bool
MonoNullCheckGuardPass::runOnMachineFunction (MachineFunction &mf)
{
	/*
	 * Over-approximate rather than replicate ImplicitNullChecks' own
	 * analyzeBranchPredicate ()-based NotNullSucc/NullSucc split: scan every
	 * successor, not just the one the real pass would walk. The null-throw
	 * successor is, in practice, never anything but a call to a throw
	 * trampoline, so this costs nothing in the common case, and erring
	 * towards leaving a safe check untouched is fine - the failure mode we
	 * must avoid is missing an unsafe one, not being slightly too cautious.
	 */
	for (MachineBasicBlock &mbb : mf) {
		const BasicBlock *bb = mbb.getBasicBlock ();
		if (!bb)
			continue;

		const Instruction *term = bb->getTerminator ();
		if (!term->getMetadata (LLVMContext::MD_make_implicit))
			continue;

		bool unsafe = false;
		for (const MachineBasicBlock *succ : mbb.successors ()) {
			if (has_opaque_memory_op (*succ)) {
				unsafe = true;
				break;
			}
		}

		if (unsafe) {
			/*
			 * This MachineFunction's IR is a private per-compile clone (see
			 * MonoLLVMJIT::compile (), engine.cpp), so mutating it here is
			 * safe - nothing else still expects it untouched.
			 */
			const_cast<Instruction *> (term)->setMetadata (LLVMContext::MD_make_implicit, nullptr);
		}
	}

	/*
	 * Nothing about the MachineFunction itself changed - only metadata on
	 * the IR terminators ImplicitNullChecks reads directly off
	 * MBB.getBasicBlock ().
	 */
	return false;
}

char MonoNullCheckGuardPass::ID = 0;

} // namespace mono

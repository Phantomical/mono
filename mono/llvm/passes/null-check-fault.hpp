/** \file Pass for rewriting surviving object-reference null checks. */

#ifndef MONO_LLVM_PASSES_NULL_CHECK_FAULT_HPP
#define MONO_LLVM_PASSES_NULL_CHECK_FAULT_HPP

#include <llvm/CodeGen/MachineFunctionPass.h>

namespace mono {

/**
 * Replace eligible checks left standing with a faulting read: mostly a
 * devirtualized `callvirt` at tier 2, every check at tier 1. The pass runs
 * after the normal machine pipeline and before EH gathering.
 */
class MonoNullCheckFaultPass : public llvm::MachineFunctionPass {
public:
	static char ID;

	MonoNullCheckFaultPass () : llvm::MachineFunctionPass (ID) {}

	llvm::StringRef getPassName () const override
	{
		return "Mono null-check faulting access";
	}

	bool runOnMachineFunction (llvm::MachineFunction &mf) override;
};

} // namespace mono

#endif /* MONO_LLVM_PASSES_NULL_CHECK_FAULT_HPP */

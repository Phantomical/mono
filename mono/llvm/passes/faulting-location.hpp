/**
 * \file
 * \brief Putting back the debug location a folded null check lost.
 */

#ifndef MONO_LLVM_PASSES_FAULTING_LOCATION_HPP
#define MONO_LLVM_PASSES_FAULTING_LOCATION_HPP

#include <llvm/CodeGen/MachineFunctionPass.h>

namespace mono {

/**
 * Gives each faulting instruction the location LLVM dropped when it folded a
 * null check into it.
 *
 * ImplicitNullChecks builds the faulting instruction with an empty location and
 * puts it where the test was, ahead of the code the dereference came from. So
 * the IL offset in effect at the fault is the one the code before the test
 * carried, which is not the dereference's own. The gather reads a range's clause
 * off exactly that offset (eh-gather.cpp), so a fault with no location of its own
 * lands outside the region that has to catch it.
 *
 * The location is taken from the arm the dereference came out of, and from the
 * block the instruction faults to when that arm has none either. Both can be
 * without one, and such a fold keeps the offset in effect where it sits.
 *
 * Run after addMachinePasses (), which is what folds the checks, and before the
 * gather and the AsmPrinter, which are what read the locations back.
 */
class MonoFaultingLocationPass : public llvm::MachineFunctionPass {
public:
	static char ID;

	MonoFaultingLocationPass () : llvm::MachineFunctionPass (ID) {}

	llvm::StringRef getPassName () const override
	{
		return "Mono faulting instruction locations";
	}

	bool runOnMachineFunction (llvm::MachineFunction &mf) override;
};

} // namespace mono

#endif /* MONO_LLVM_PASSES_FAULTING_LOCATION_HPP */

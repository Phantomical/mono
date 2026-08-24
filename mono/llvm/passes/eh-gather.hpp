/**
 * \file
 * \brief The machine-level recovery of mono's EH clauses from the final
 * landing-pad set.
 */

#ifndef MONO_LLVM_PASSES_EH_GATHER_HPP
#define MONO_LLVM_PASSES_EH_GATHER_HPP

/*
 * LLVM uses `PIC` as an identifier (PassInstrumentationCallbacks). Mono's
 * build defines it as a macro.
 */
#ifdef PIC
#undef PIC
#endif

#include <llvm/CodeGen/MachineFunctionPass.h>

namespace mono {

struct MonoEHSideChannel;

/**
 * Gathers the info needed to build mono's exception handling tables for the
 * function being compiled.
 */
class MonoEHGatherPass : public llvm::MachineFunctionPass {
public:
	static char ID;

	/* sc must outlive the pipeline this pass is added to. */
	explicit MonoEHGatherPass (MonoEHSideChannel *sc)
	    : llvm::MachineFunctionPass (ID), sc_ (sc)
	{
	}

	llvm::StringRef getPassName () const override
	{
		return "Mono EH clause gather";
	}

	bool runOnMachineFunction (llvm::MachineFunction &mf) override;

private:
	MonoEHSideChannel *sc_;
};

} // namespace mono

#endif /* MONO_LLVM_PASSES_EH_GATHER_HPP */

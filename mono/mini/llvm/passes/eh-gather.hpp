/**
 * \file
 * eh-gather.hpp - MonoEHGatherPass, the machine-level recovery of mono's EH
 * clauses from the final landing-pad set.
 */

#ifndef MONO_MINI_LLVM_PASSES_EH_GATHER_HPP
#define MONO_MINI_LLVM_PASSES_EH_GATHER_HPP

/*
 * Same reason engine.cpp drops mono's PIC macro: libtool compiles these TUs with
 * -DPIC and LLVM uses `PIC` as an identifier (PassInstrumentationCallbacks), so
 * the macro would rewrite it and break a header.
 */
#ifdef PIC
#undef PIC
#endif

#include <llvm/CodeGen/MachineFunctionPass.h>

namespace mono {

struct MonoEHSideChannel;

/**
 * A pass that gathers the info needed to build mono's exception handling
 * tables for the function being compiled.
 */
class MonoEHGatherPass : public llvm::MachineFunctionPass {
public:
	static char ID;

	/* SC must outlive the pipeline this pass is added to. */
	explicit MonoEHGatherPass (MonoEHSideChannel *sc)
	    : llvm::MachineFunctionPass (ID), sc_ (sc)
	{
	}

	llvm::StringRef getPassName () const override
	{
		return "Mono EH clause gather";
	}

	/* Read-only: never disturb anything the AsmPrinter will emit. */
	void getAnalysisUsage (llvm::AnalysisUsage &au) const override
	{
		au.setPreservesAll ();
		llvm::MachineFunctionPass::getAnalysisUsage (au);
	}

	bool runOnMachineFunction (llvm::MachineFunction &mf) override;

private:
	MonoEHSideChannel *sc_;
};

} // namespace mono

#endif /* MONO_MINI_LLVM_PASSES_EH_GATHER_HPP */

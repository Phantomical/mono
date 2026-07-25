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

/*
 * Runs after addMachinePasses() and before the AsmPrinter (so it sees the final
 * landing-pad set). For each landing pad it records the invoke range, the
 * handler label and the IL clause_index - recovered in-process from the
 * type_info_N global's i32 initializer (mono's clause-index smuggling) - into a
 * per-compile side channel. It EMITS NOTHING and never modifies the
 * MachineFunction (runOnMachineFunction returns false, and it preserves all
 * analyses), so scheduling it leaves the emitted object byte-identical for a
 * non-EH module. C3 turns the side channel into a .mono_lsda section.
 *
 * Robustness (CAP-EH-0): declining is reserved for shapes that are valid input
 * we just don't support yet - right now that is exactly a filter clause
 * (`catch when (...)`), flagged via has_filter so translator.cpp can decline
 * with a specific reason instead of the generic parse failure. Everything else
 * this pass checks is an invariant of LLVM's own landing-pad bookkeeping or of
 * mono's own type_info_N emission (translator-call.cpp) - not something
 * unsupported input can trigger - so violating it means our own code or our
 * assumptions about LLVM are wrong, and report_fatal_error says so loudly
 * instead of quietly declining a method forever with no signal anything broke.
 */
class MonoEHGatherPass : public llvm::MachineFunctionPass {
public:
	/* A static ID is all the legacy PassManager needs; no INITIALIZE_PASS. */
	static char ID;

	/* SC must outlive the pipeline this pass is added to. */
	explicit MonoEHGatherPass (MonoEHSideChannel &sc)
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
	MonoEHSideChannel &sc_;
};

} // namespace mono

#endif /* MONO_MINI_LLVM_PASSES_EH_GATHER_HPP */

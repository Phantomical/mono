/**
 * \file
 * \brief The machine-level recovery of finally-body PC ranges.
 */

#ifndef MONO_LLVM_PASSES_FINALLY_RANGE_HPP
#define MONO_LLVM_PASSES_FINALLY_RANGE_HPP

/*
 * mono/utils/mono-tls.h defines PIC as an empty macro under -fPIC, and LLVM
 * uses `PIC` as an identifier (PassInstrumentationCallbacks), so a stray
 * expansion breaks a header below.
 */
#ifdef PIC
#undef PIC
#endif

#include <llvm/CodeGen/MachineFunctionPass.h>

namespace mono {

struct MonoEHSideChannel;

/**
 * Recovers, at the machine level, the PC ranges each finally body occupies
 * in the compiled function. The thread-abort guard in MonoEHFinallyBody
 * needs these to be exact.
 */
class MonoFinallyRangePass : public llvm::MachineFunctionPass {
public:
	static char ID;

	/* sc must outlive the pipeline this pass is added to. */
	explicit MonoFinallyRangePass (MonoEHSideChannel *sc)
	    : llvm::MachineFunctionPass (ID), sc_ (sc)
	{
	}

	llvm::StringRef getPassName () const override
	{
		return "Mono finally body ranges";
	}

	bool runOnMachineFunction (llvm::MachineFunction &mf) override;

private:
	MonoEHSideChannel *sc_;
};

} // namespace mono

#endif /* MONO_LLVM_PASSES_FINALLY_RANGE_HPP */

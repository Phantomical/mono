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

#include <llvm/ADT/DenseMap.h>
#include <llvm/CodeGen/MachineFunctionPass.h>

#include <cstdint>

namespace llvm {
class DISubprogram;
class Module;
} // namespace llvm

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

	/* Builds ids_ once per module - eh-gather.hpp's doInitialization () does the
	 * same for the same reason. */
	bool doInitialization (llvm::Module &m) override;

	bool runOnMachineFunction (llvm::MachineFunction &mf) override;

private:
	MonoEHSideChannel *sc_;
	llvm::DenseMap<const llvm::DISubprogram *, std::uint64_t> ids_;
};

} // namespace mono

#endif /* MONO_LLVM_PASSES_FINALLY_RANGE_HPP */

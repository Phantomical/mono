/**
 * \file
 * finally-range.hpp - MonoFinallyRangePass, the machine-level recovery of the PC
 * ranges each finally handler body occupies.
 */

#ifndef MONO_MINI_LLVM_PASSES_FINALLY_RANGE_HPP
#define MONO_MINI_LLVM_PASSES_FINALLY_RANGE_HPP

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
 * This pass figures out the code ranges that belong to finally blocks in the
 * function. C# requires that when a thread is aborted while it is executing a
 * finally block that the exception must be delayed until the finally block
 * completes, this pass figures out what regions of optimized code are the finally
 * blocks.
 */
class MonoFinallyRangePass : public llvm::MachineFunctionPass {
public:
	static char ID;

	/* SC must outlive the pipeline this pass is added to. */
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

#endif /* MONO_MINI_LLVM_PASSES_FINALLY_RANGE_HPP */

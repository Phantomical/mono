/**
 * \file
 * null-check-guard.hpp - MonoNullCheckGuardPass, a defensive pre-pass for
 * LLVM's ImplicitNullChecks.
 *
 * LLVM 18's ImplicitNullChecks::areMemoryOpsAliased() (ImplicitNullChecks.cpp)
 * assumes every MachineMemOperand it compares a null-check candidate against
 * carries either a Value or a PseudoSourceValue. It guards the
 * PseudoSourceValue case but not the "neither" one, and falls through to an
 * alias query built from a null Value*, which segfaults in
 * Value::stripPointerCastsForAliasAnalysis (upstream:
 * llvm/llvm-project#63585, open and unfixed as of LLVM 18). Our GC
 * write-barrier card-mark store is exactly such an operand - its address is
 * inttoptr'd shifted/masked arithmetic, not a GEP off any IR-traceable
 * pointer - and sharing a block with a `!make.implicit`-tagged null check
 * (the tag the translator's emit_cond_system_exception attaches, see
 * translator-emit.cpp) is routine: an out-parameter struct write right after
 * a null check on the receiver, say.
 *
 * This pass runs just ahead of ImplicitNullChecks (engine.cpp's emit_object
 * inserts it after ExpandPostRAPseudosID - the nearest exported anchor
 * symbol upstream of ImplicitNullChecksID; LLVM's insertPass only supports
 * "after", and the passes actually adjacent to ImplicitNullChecksID are
 * anonymous-namespace types with no exported AnalysisID to anchor on). For
 * every block whose terminator carries `!make.implicit`, if either successor
 * contains an instruction with a memory operand lacking both a Value and a
 * PseudoSourceValue, the tag is stripped so ImplicitNullChecks leaves that
 * check alone. Every other tagged check - the overwhelming majority - is
 * untouched and still gets folded.
 */

#ifndef MONO_MINI_LLVM_PASSES_NULL_CHECK_GUARD_HPP
#define MONO_MINI_LLVM_PASSES_NULL_CHECK_GUARD_HPP

/*
 * Same reason engine.cpp drops mono's PIC macro: libtool compiles this TU with
 * -DPIC, and LLVM uses `PIC` as an identifier (PassInstrumentationCallbacks),
 * so the macro would rewrite it and break a header.
 */
#ifdef PIC
#undef PIC
#endif

#include <llvm/CodeGen/MachineFunctionPass.h>

namespace mono {

class MonoNullCheckGuardPass : public llvm::MachineFunctionPass {
public:
	static char ID;

	MonoNullCheckGuardPass () : llvm::MachineFunctionPass (ID) {}

	llvm::StringRef getPassName () const override
	{
		return "Mono implicit-null-check MMO guard";
	}

	/* Only ever clears IR-level metadata on a terminator; nothing about the
	 * MachineFunction itself changes. */
	void getAnalysisUsage (llvm::AnalysisUsage &au) const override
	{
		au.setPreservesAll ();
		llvm::MachineFunctionPass::getAnalysisUsage (au);
	}

	bool runOnMachineFunction (llvm::MachineFunction &mf) override;
};

} // namespace mono

#endif /* MONO_MINI_LLVM_PASSES_NULL_CHECK_GUARD_HPP */

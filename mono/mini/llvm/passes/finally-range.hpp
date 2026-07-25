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

/*
 * The runtime's thread-abort guard (find_last_handler_block, mini-exceptions.c)
 * answers one question about a stopped thread: is this frame inside a finally
 * handler body? If it is, the abort must not be delivered there - it is deferred
 * through the clause's exvar and rethrown once the finally returns, which is what
 * keeps a finally from being torn in half (ECMA-335 12.4.2).
 *
 * Answering it needs the PCs the body occupies, and nothing structural survives
 * codegen to say which those are. The optimizer duplicates a body along its entry
 * paths and merges each copy into whatever it flows through; BranchFolding then
 * merges what is left back into foreign predecessors, and the surviving
 * MachineBasicBlock reports the FOREIGN block as its origin. Neither IR blocks nor
 * machine blocks still mean "body" by the time this runs.
 *
 * Instructions do survive - passes move and clone them rather than rewriting them
 * - so the translator brackets each body with a pair of stackmap markers
 * (emit_finally_guard_stackmap / emit_finally_end_stackmap, translator-call.cpp)
 * and this pass recovers the ranges between them. It must therefore be scheduled
 * after every pass that can move code.
 *
 * Nothing is paired ACROSS blocks, which is what defeated an earlier attempt at
 * this: a forward dataflow decides whether each block STARTS inside a body, and
 * every range is then a run of instructions within one block, bracketed where it
 * actually lies. A clause can end up with several ranges, and that is fine - the
 * runtime scans the clause array for a PC match, so several entries sharing a
 * clause_index and exvar answer the question exactly as one would.
 */
class MonoFinallyRangePass : public llvm::MachineFunctionPass {
public:
	static char ID;

	/* SC must outlive the pipeline this pass is added to. */
	explicit MonoFinallyRangePass (MonoEHSideChannel &sc)
	    : llvm::MachineFunctionPass (ID), sc_ (sc)
	{
	}

	llvm::StringRef getPassName () const override
	{
		return "Mono finally body ranges";
	}

	bool runOnMachineFunction (llvm::MachineFunction &mf) override;

private:
	MonoEHSideChannel &sc_;
};

} // namespace mono

#endif /* MONO_MINI_LLVM_PASSES_FINALLY_RANGE_HPP */

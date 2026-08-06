/**
 * \file
 * eh-side-channel.hpp - what the machine-level EH passes hand back to the
 * compiler that scheduled them.
 *
 * MonoEHGatherPass (passes/eh-gather.cpp) and MonoFinallyRangePass
 * (passes/finally-range.cpp) emit nothing. They run inside the object-emission
 * pipeline, read the target-neutral machine IR, and fill in the structs below;
 * the `.mono_lsda` streamer then joins the two halves by function name and
 * writes the section. The channel is a stack local of one compile threaded into
 * the passes, so nothing here is shared between concurrent compiles.
 *
 * The MCSymbol* fields are the very symbols the AsmPrinter emits into .text.
 * They are kept as symbols because that is what the streamer needs to turn them
 * into func_begin-relative label differences; they are not resolvable at the
 * point the passes run, so nothing may dereference them.
 */

#ifndef MONO_LLVM_EH_SIDE_CHANNEL_HPP
#define MONO_LLVM_EH_SIDE_CHANNEL_HPP

#include <cstdint>
#include <string>
#include <vector>

namespace llvm {
class MCSymbol;
} // namespace llvm

namespace mono {

/*
 * One (invoke range, catch clause) tuple, as the gather pass sees it. A landing
 * pad carries ONE (begin,end) pair per invoke that unwinds to it, so a try with
 * N protected calls produces N clauses that share a handler and clause_index but
 * cover disjoint invoke ranges. .mono_lsda is thus one entry per invoke range.
 */
struct MonoEHClause {
	/* This invoke's try range: a paired LandingPadInfo BeginLabels[i]/EndLabels[i]. */
	const llvm::MCSymbol *try_begin = nullptr;
	const llvm::MCSymbol *try_end = nullptr;
	/* The handler entry: LandingPadInfo LandingPadLabel. */
	const llvm::MCSymbol *handler = nullptr;
	/* The IL clause index, smuggled through the type_info_N initializer. */
	int clause_index = -1;
	/*
	 * The clause's IL flags (a MonoExceptionEnum: NONE=0/catch, FINALLY=2,
	 * FAULT=4), smuggled alongside clause_index through the type_info_N global's
	 * 2-word {i32 clause_index, i32 kind} initializer. MonoLSDAStreamer writes it
	 * into the v2 section's kind column so the section is self-describing. For a
	 * catch clause it is 0; the legacy 1-word i32 initializer (clause_index only)
	 * leaves it 0 too.
	 */
	int kind = 0;
	/*
	 * False if the clause_index could not be safely recovered (the type_info was
	 * not a GlobalVariable carrying a ConstantInt / 2-word struct initializer):
	 * downstream must decline, never guess.
	 */
	bool clause_resolved = false;
};

/* Everything the gather pass found for one EH-bearing MachineFunction. */
struct MonoEHFunctionClauses {
	/*
	 * The function's name (MF.getName()). The streamer keys the emitted section
	 * to the function symbol; the JIT is one function per module, but this is
	 * structured so it can emit for the right function.
	 */
	std::string function;
	std::vector<MonoEHClause> clauses;
	/*
	 * A negative TypeId (an exception-specification filter) was seen. Recorded so
	 * a caller can decline the method; never a crash.
	 */
	bool has_filter = false;
	/*
	 * Something unexpected was seen (a missing begin/end/lpad label, an
	 * unresolvable type_info, or a filter/cleanup TypeId): downstream must
	 * decline this method rather than publish a table it cannot trust.
	 */
	bool declined = false;
};

/*
 * One PC range a FINALLY clause's handler body occupies, as MonoFinallyRangePass
 * found it. A clause can have SEVERAL: the optimizer duplicates a body along its
 * entry paths, and each surviving copy is a range of its own.
 *
 * This is what the runtime's thread-abort guard asks about a stopped frame ("is it
 * inside this finally?"), so the bounds have to be exact. They are labels the pass
 * plants at the run's two ends - a label emits no code and can sit anywhere in a
 * block, so the range names where the body actually lies.
 */
struct MonoEHFinallyBody {
	const llvm::MCSymbol *body_begin = nullptr;
	const llvm::MCSymbol *body_end = nullptr;
	/* The IL clause index, read back from the markers bracketing the run. */
	int clause_index = -1;
	/*
	 * Where the clause's thread-abort guard byte sits in the frame, as the DWARF
	 * number of the register it is addressed off and a displacement from it - what
	 * install_handler_block_guard () needs to reach it from a stack walk. -1 when
	 * the opening marker named no slot, which is how a caller that recovers the
	 * slot from the stackmap section instead uses these markers.
	 */
	int exvar_dwarf_reg = -1;
	std::int64_t exvar_offset = 0;
};

/* The finally body ranges MonoFinallyRangePass found in one MachineFunction. */
struct MonoEHFinallyFunction {
	/*
	 * MF.getName (), which the `.mono_lsda` streamer matches against
	 * MonoEHFunctionClauses::function to write both into one record.
	 */
	std::string function;
	std::vector<MonoEHFinallyBody> bodies;
};

/*
 * The per-compile side channel: one entry per EH-bearing function. A non-EH
 * module (no landing pads) leaves this empty, so the gather pass is inert.
 *
 * finally_functions is a SEPARATE list because a different pass fills it, at a
 * different point in the pipeline; the streamer joins the two by function name
 * when it writes the section.
 */
struct MonoEHSideChannel {
	std::vector<MonoEHFunctionClauses> functions;
	std::vector<MonoEHFinallyFunction> finally_functions;
};

} // namespace mono

#endif /* MONO_LLVM_EH_SIDE_CHANNEL_HPP */

/**
 * \file
 * eh-side-channel.hpp - what the machine-level EH passes hand back to the
 * compiler that scheduled them.
 *
 * MonoEHGatherPass (passes/eh-gather.cpp) and MonoFinallyRangePass
 * (passes/finally-range.cpp) emit nothing. They run inside the object-emission
 * pipeline, read the target-neutral machine IR, and fill in the structs below.
 * SideTableEmitPass (compiler.cpp) then writes each struct to a section of its
 * own: `.mono_lsda` for the clauses, `.mono_guards` for the finally bodies.
 * Both sections key their blocks by function name. The channel is a stack
 * local of one compile threaded into the passes, so it is not shared between
 * concurrent compiles.
 *
 * The MCSymbol* fields are the very symbols the AsmPrinter emits into .text.
 * They are kept as symbols because that is what turns them into
 * func_begin-relative label differences. They are not resolvable at the point
 * the passes run, so nothing can dereference them.
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

/**
 * One (protected range, clause) tuple, as the gather pass sees it.
 *
 * A range is one invoke's range widened over the code around it that belongs to
 * the same try region (eh-gather.cpp), and a pad dispatches a whole chain of
 * clauses, so one range yields one entry per clause in that chain. `.mono_lsda`
 * is one entry per (range, clause).
 */
struct MonoEHClause {
	/// The protected range this entry covers.
	const llvm::MCSymbol *try_begin = nullptr;
	const llvm::MCSymbol *try_end = nullptr;
	/// The handler entry: LandingPadInfo LandingPadLabel.
	const llvm::MCSymbol *handler = nullptr;
	/// The IL clause index, smuggled through the type_info_N initializer.
	int clause_index = -1;
	/// Which method clause_index indexes into: 0 for the method this compile is
	/// building (every clause until a fold merges a live one in), otherwise
	/// (uint64_t)(uintptr_t) of a folded body's own MonoMethod*, the same
	/// convention IlInlineRow::callee uses (jit.hpp) - eh-gather.cpp resolves it
	/// off the landing pad's own DILocation scope, through the same id map
	/// il_debug_subprogram_ids () (il-line-table.hpp) hands .mono_inlines.
	std::uint64_t owner = 0;
	/// The clause's IL flags, a MonoExceptionEnum: NONE=0 for catch, FINALLY=2,
	/// FAULT=4. It rides alongside clause_index in the same type_info_N global,
	/// and reaches the section's kind column, which is what makes the section
	/// self-describing. A marker kind (mono_lsda_format.hpp) instead says the
	/// pad is something other than a clause's handler.
	int kind = 0;
	/// False if the clause_index cannot be recovered safely. Downstream must
	/// then decline rather than guess.
	bool clause_resolved = false;
};

/// Everything the gather pass found for one EH-bearing MachineFunction.
struct MonoEHFunctionClauses {
	/// The function's name, from MF.getName (). The emitted section is keyed to
	/// the function symbol, so a module holding more than one function still
	/// emits against the right one.
	std::string function;
	std::vector<MonoEHClause> clauses;
	/// A negative TypeId, an exception-specification filter, was seen. Recorded
	/// so a caller can decline the method rather than crash on it.
	bool has_filter = false;
	/// Something unexpected was seen: a missing begin, end or landing-pad label,
	/// an unresolvable type_info, or a filter or cleanup TypeId. Downstream must
	/// decline this method rather than publish a table it cannot trust.
	bool declined = false;
};

/*
 * One PC range a FINALLY clause's handler body occupies, as MonoFinallyRangePass
 * found it. A clause can have several. The optimizer duplicates a body along its
 * entry paths, and each surviving copy is a range of its own.
 *
 * The runtime's thread-abort guard asks these bounds whether a stopped frame is
 * inside this finally, so they have to be exact. They are labels the pass plants
 * at the run's two ends. A label emits no code and can sit anywhere in a block,
 * so the range names where the body actually lies.
 */
struct MonoEHFinallyBody {
	const llvm::MCSymbol *body_begin = nullptr;
	const llvm::MCSymbol *body_end = nullptr;
	/// The IL clause index, read back from the markers bracketing the run.
	int clause_index = -1;
	/// Same convention and same resolution as MonoEHClause::owner above, read
	/// off the opening marker's own DILocation scope.
	std::uint64_t owner = 0;
	/// Where the clause's thread-abort guard byte sits in the frame: the DWARF
	/// number of the register it is addressed off, and a displacement from it.
	/// That is what mono_install_handler_block_guard () needs to reach it from a
	/// stack walk. The register is -1 when the opening marker named no slot.
	/// That is how a caller uses these markers when it recovers the slot from
	/// the stackmap section instead.
	int exvar_dwarf_reg = -1;
	std::int64_t exvar_offset = 0;
};

struct MonoEHFinallyFunction {
	/// MF.getName (). SideTableEmitPass keys `.mono_guards` blocks by this
	/// name, the same way it keys `.mono_lsda` blocks by
	/// MonoEHFunctionClauses::function.
	std::string function;
	std::vector<MonoEHFinallyBody> bodies;
};

/*
 * The per-compile side channel: one entry per EH-bearing function. A module with
 * no landing pads leaves this empty, so the gather pass is inert.
 *
 * finally_functions is a separate list because a different pass fills it, at a
 * different point in the pipeline. Both lists key their entries by function
 * name, and SideTableEmitPass writes each to a section of its own.
 */
struct MonoEHSideChannel {
	std::vector<MonoEHFunctionClauses> functions;
	std::vector<MonoEHFinallyFunction> finally_functions;
};

} // namespace mono

#endif /* MONO_LLVM_EH_SIDE_CHANNEL_HPP */

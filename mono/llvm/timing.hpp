/**
 * \file
 * \brief Per-phase accounting of where a compile's time goes.
 */

#ifndef MONO_LLVM_TIMING_HPP
#define MONO_LLVM_TIMING_HPP

#include <cstdint>

namespace mono {
namespace timing {

/// The phases a compile is broken into. They nest, so each is reported with
/// both the time spent inside it and the time spent inside it but not inside
/// any phase below it - the second is the one to read.
/// Publishing a method's own stubs is only inside `compile` when it happened on
/// the way to a caller's - that is, under `resolve`. A method the runtime asks
/// for by name has its stubs published before any of this starts, and that is
/// not accounted here.
///
/// Everything from `ctxnew` down is a finer split of a phase above it, and is
/// only recorded when MONO_LLVM_JIT_TIMING names `fine`. Left off, the time
/// stays in the enclosing phase's self column.
enum class Phase {
	compile,   ///< translating a method and linking what came out.
	metadata,  ///< loading the method header and whatever it drags in.
	translate, ///< CIL to LLVM IR.
	resolve,   ///< laying out the classes a callee names, publishing its stubs.
	orc,       ///< handing the module to ORC and getting an address back.
	dylib,     ///< making the JITDylib the module is compiled into.
	pipeline,  ///< the tier-0 IR pass pipeline.
	codegen,   ///< ISel through the AsmPrinter, side tables included.
	cgsetup,   ///< building codegen's pass pipeline, streamer and printer.
	cgrun,     ///< running them.
	dwarf,     ///< reading the IL line table back out of the object.
	jinfo,     ///< turning the object's side tables into MonoJitInfo.

	ctxnew,    ///< in compile: the fresh LLVMContext and Module.
	addir,     ///< in orc: handing the module to the JIT.
	jlink,     ///< in orc: JITLink, from pre-prune to post-fixup.
	pbsetup,   ///< in pipeline: the PassBuilder, the analysis managers, the passes.
	prun,      ///< in pipeline: running them.
	mmi,       ///< in cgsetup: MachineModuleInfo, which owns the MCContext.
	cgpass,    ///< in cgsetup: the target pass config and the machine passes.
	strm,      ///< in cgsetup: the asm backend, code emitter and object streamer.
	aprint,    ///< in cgsetup: the AsmPrinter.
	isel,      ///< in cgrun: the pre-ISel IR passes and ISel, per function.
	mpass,     ///< in cgrun: the machine passes over each function, after ISel.
	emit,      ///< in cgrun: the AsmPrinter over each function.
	sidetbl,   ///< in cgrun: writing .mono_lsda, .mono_guards and .mono_unwind.
	objout,    ///< in cgrun: laying the object out and writing it, plus finalization.
	pmfree,    ///< in codegen: taking the codegen pass manager back down.
	tsmfree,   ///< in orc: dropping the module and its LLVMContext.
	lgraph,    ///< in orc: turning the object into a LinkGraph.
	memfin,    ///< in orc: finalizing the code memory the link allocated.
	count,
};

/// Whether MONO_LLVM_JIT_TIMING asked for the breakdown.
bool enabled ();

/// Whether it asked for the finer split too.
bool fine ();

/// Accumulates the time between its construction and destruction against a
/// phase, and against the enclosing scope's children so that self time comes
/// out right. A Scope costs nothing beyond a predictable branch when the
/// breakdown was not asked for.
class Scope {
public:
	explicit Scope (Phase phase);
	~Scope ();

	Scope (const Scope &) = delete;
	Scope &operator= (const Scope &) = delete;

private:
	friend void span_end (Phase phase, uint64_t start);

	Phase phase_;
	Scope *parent_;
	uint64_t start_;
	uint64_t children_ = 0;
};

/// The same accounting as a Scope for a span whose two ends are in different
/// functions - a pass that runs inside a pass manager, say. Zero from
/// span_begin () means nothing was recorded and span_end () is a no-op.
uint64_t span_begin (Phase phase);
void span_end (Phase phase, uint64_t start);

} // namespace timing
} // namespace mono

#endif /* MONO_LLVM_TIMING_HPP */

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
enum class Phase {
	compile,   ///< everything the runtime asked for, from stub to code.
	metadata,  ///< loading the method header and whatever it drags in.
	translate, ///< CIL to LLVM IR.
	resolve,   ///< laying out the classes a callee names, publishing its stub.
	orc,       ///< handing the module to ORC and getting an address back.
	dylib,     ///< making the JITDylib the module is compiled into.
	pipeline,  ///< the tier-0 IR pass pipeline.
	codegen,   ///< ISel through the AsmPrinter, side tables included.
	cgsetup,   ///< building codegen's pass pipeline, streamer and printer.
	cgrun,     ///< running them.
	dwarf,     ///< reading the IL line table back out of the object.
	jinfo,     ///< turning the object's side tables into MonoJitInfo.
	count,
};

/// Whether MONO_LLVM_JIT_TIMING asked for the breakdown.
bool enabled ();

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
	Phase phase_;
	Scope *parent_;
	uint64_t start_;
	uint64_t children_ = 0;
};

} // namespace timing
} // namespace mono

#endif /* MONO_LLVM_TIMING_HPP */

/**
 * \file
 * il-line-table.hpp - IL offsets as LLVM debug info.
 *
 * A tier-1 method's native_offset -> il_offset map (stack traces, profiler
 * attribution) rides on debug info: each OP_IL_SEQ_POINT sets a DILocation whose
 * line is the IL offset, and the engine reads it back out of the emitted object.
 *
 * Every function translated into a compile's module gets a subprogram - the root
 * and each callee the tier-1 inliner materializes. When LLVM folds a callee in it
 * builds the inlinedAt chain itself, so the emitted DWARF says both "this address
 * is IL offset N of the callee" and "which the root reached from its IL offset M".
 * The engine keeps the root's offset as the method's own map (a frame in the root
 * must report the root's call site) and hands the rest back as inline frames.
 *
 * This lives in its own translation unit because LLVM's debug-info headers pull
 * in llvm/BinaryFormat/Dwarf.h, whose enumerators collide with the DW_* macros
 * mono/utils/freebsd-dwarf.h defines - and translator-internal.hpp includes that.
 * So the types below stay opaque: nothing here may leak an LLVM debug type into a
 * translator TU.
 */

#ifndef MONO_LLVM_IL_LINE_TABLE_HPP
#define MONO_LLVM_IL_LINE_TABLE_HPP

#include <cstdint>
#include <memory>

#include <llvm/IR/IRBuilder.h>

namespace llvm {
class Function;
class Module;
}

namespace mono {

/*
 * One function's subprogram, plus the IL offset currently in effect for it.
 * Owned by the IlDebugModule that handed it out; opaque here so that a
 * translator TU can hold one without seeing an LLVM debug type.
 */
struct IlDebugScope;

/*
 * The debug info for one compile's module: a single compile unit, and a
 * subprogram per function translated into it.
 */
class IlDebugModule {
public:
	explicit IlDebugModule (llvm::Module *module);
	~IlDebugModule ();

	IlDebugModule (const IlDebugModule &) = delete;
	IlDebugModule &operator= (const IlDebugModule &) = delete;

	/*
	 * Attach a subprogram named NAME to FN. NAME is what comes back out of the
	 * emitted DWARF, so it has to be the key the caller can resolve a MonoMethod
	 * from - mono_method_full_name (), which is what the translator names
	 * functions with anyway.
	 */
	IlDebugScope *add_function (llvm::Function *fn, const char *name);

	/* Close the metadata off. Without this it is malformed and the verifier says so. */
	void finish ();

private:
	struct Impl;
	std::unique_ptr<Impl> impl_;
};

/* Attribute everything BUILDER emits from here on to IL_OFFSET within SCOPE. */
void il_debug_set_location (IlDebugScope *scope, llvm::IRBuilder<> *builder, uint32_t il_offset);

/*
 * Put SCOPE's current location onto BUILDER. Called for each newly created
 * builder, so a block that starts before its first OP_IL_SEQ_POINT does not emit
 * unattributed instructions - an inlinable call with no location in a function
 * that has debug info is a verifier error, not merely a gap in the table.
 */
void il_debug_reapply (IlDebugScope *scope, llvm::IRBuilder<> *builder);

/*
 * DWARF reserves line 0 for "no source location" and IL offset 0 is an ordinary
 * first instruction, so the line is the IL offset plus this. The engine subtracts
 * it when reading the debug info back.
 */
constexpr uint32_t IL_OFFSET_LINE_BIAS = 1;

} // namespace mono

#endif /* MONO_LLVM_IL_LINE_TABLE_HPP */

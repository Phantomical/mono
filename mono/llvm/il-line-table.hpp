/**
 * \file
 * \brief IL offsets as LLVM debug info.
 *
 * A method's native_offset -> il_offset map (stack traces, profiler attribution)
 * rides on debug locations: the translator gives each IL instruction's emitted
 * code its own DILocation, which survives the optimizer and reaches the machine
 * instructions. The printer reads the lines back off them and writes
 * `.mono_lines` (sidetables.hpp); no DWARF is emitted, and the compile unit
 * below says so.
 *
 * Every function translated into a compile's module gets a subprogram, since a
 * location needs a scope and a filter body is a frame of its own with a map of
 * its own.
 *
 * This lives in its own translation unit because LLVM's debug-info headers pull
 * in llvm/BinaryFormat/Dwarf.h, whose enumerators collide with the DW_* macros
 * mini-unwind.h defines, and method-to-llvm.hpp includes that through mini.h.
 * So the types below stay opaque, and this header must not leak an LLVM debug
 * type into a translator TU.
 */

#ifndef MONO_LLVM_IL_LINE_TABLE_HPP
#define MONO_LLVM_IL_LINE_TABLE_HPP

#include <cstdint>
#include <memory>

#include <llvm/IR/IRBuilder.h>

namespace llvm {
class Function;
class Instruction;
class Module;
}

namespace mono {

/// One function's subprogram, plus the IL offset currently in effect for it.
///
/// Owned by the IlDebugModule that handed it out. It stays opaque here, so a
/// translator TU can hold one without seeing an LLVM debug type.
struct IlDebugScope;

/// The debug info for one compile's module: a single compile unit, and a
/// subprogram per function translated into it.
class IlDebugModule {
public:
	explicit IlDebugModule (llvm::Module *module);
	~IlDebugModule ();

	IlDebugModule (const IlDebugModule &) = delete;
	IlDebugModule &operator= (const IlDebugModule &) = delete;

	/// Attaches a subprogram named name to fn.
	///
	/// name is what comes back out of the emitted DWARF, so it has to be the key
	/// the caller can resolve a MonoMethod from - mono_method_full_name (), which
	/// is what the translator names functions with anyway.
	IlDebugScope *add_function (llvm::Function *fn, const char *name);

	/// Closes the metadata off. Without this it is malformed, and the verifier
	/// says so.
	void finish ();

private:
	struct Impl;
	std::unique_ptr<Impl> impl_;
};

/// Attributes everything builder emits from here on to il_offset within scope.
void il_debug_set_location (IlDebugScope *scope, llvm::IRBuilder<> *builder, uint32_t il_offset);

/// Re-attributes one instruction that has already been emitted, leaving the
/// location in effect for everything after it alone.
void il_debug_set_instruction_location (IlDebugScope *scope, llvm::Instruction *inst,
                                        uint32_t il_offset);

/// Puts scope's current location onto builder.
///
/// Call this for each newly created builder, so instructions it emits before
/// the next call to il_debug_set_location () still carry a location. An
/// inlinable call with no location in a function that has debug info is a
/// verifier error, not merely a gap in the table.
void il_debug_reapply (IlDebugScope *scope, llvm::IRBuilder<> *builder);

/// DWARF reserves line 0 for "no source location" and IL offset 0 is an
/// ordinary first instruction, so the line is the IL offset plus this. The
/// engine subtracts it when reading the debug info back.
constexpr uint32_t IL_OFFSET_LINE_BIAS = 1;

} // namespace mono

#endif /* MONO_LLVM_IL_LINE_TABLE_HPP */

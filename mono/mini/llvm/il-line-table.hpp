/**
 * \file
 * il-line-table.hpp - IL offsets as LLVM line-table debug info.
 *
 * A tier-1 method's native_offset -> il_offset map (stack traces, profiler
 * attribution) rides on a DWARF line table: each OP_IL_SEQ_POINT sets a
 * DILocation whose line is the IL offset, and the engine reads the rows back out
 * of the emitted object's `.debug_line`.
 *
 * This lives in its own translation unit because LLVM's debug-info headers pull
 * in llvm/BinaryFormat/Dwarf.h, whose enumerators collide with the DW_* macros
 * mono/utils/freebsd-dwarf.h defines - and translator-internal.hpp includes that.
 * So nothing here may be merged into the translator TUs; the interface below is
 * deliberately narrow enough that it does not need to be.
 */

#ifndef __MONO_LLVM_IL_LINE_TABLE_HPP__
#define __MONO_LLVM_IL_LINE_TABLE_HPP__

#include <cstdint>
#include <memory>

#include <llvm/IR/IRBuilder.h>

namespace llvm {
class Function;
class Module;
}

namespace mono {

/*
 * One method's line table. Construct it for a tier-1 root just after its
 * Function exists, move it along with set_location () as the translator walks
 * the IL, and finish () before the module is optimized.
 *
 * Deliberately NOT created for a materialized inliner callee: a body with no
 * debug info at all is what makes LLVM's inliner stamp the whole inlined range
 * with the ROOT's call-site location (fixupLineNumbers (), InlineFunction.cpp),
 * which is the IL offset a frame in the root should report. Give the callee its
 * own line table and the root's table ends up interleaved with IL offsets from a
 * different method.
 */
class IlLineTable {
public:
	IlLineTable (llvm::Module *module, llvm::Function *fn, const char *name);
	~IlLineTable ();

	IlLineTable (const IlLineTable &) = delete;
	IlLineTable &operator= (const IlLineTable &) = delete;

	/* Attribute everything BUILDER emits from here on to IL_OFFSET. */
	void set_location (llvm::IRBuilder<> *builder, uint32_t il_offset);

	/*
	 * Put the location currently in effect onto BUILDER. Called for each newly
	 * created builder, so a block that starts before its first OP_IL_SEQ_POINT
	 * does not emit unattributed instructions - an inlinable call with no
	 * location in a function that has debug info is a verifier error.
	 */
	void reapply (llvm::IRBuilder<> *builder) const;

	/* Close the metadata off. Without this it is malformed and the verifier says so. */
	void finish ();

private:
	struct Impl;
	std::unique_ptr<Impl> impl_;
};

/*
 * DWARF reserves line 0 for "no source location" and IL offset 0 is an ordinary
 * first instruction, so the line is the IL offset plus this. engine.cpp
 * subtracts it when reading the table back.
 */
constexpr uint32_t IL_OFFSET_LINE_BIAS = 1;

} // namespace mono

#endif /* __MONO_LLVM_IL_LINE_TABLE_HPP__ */

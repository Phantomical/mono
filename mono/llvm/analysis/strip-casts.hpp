/**
 * \file
 * \brief Comparing two operands that name one runtime value.
 */

#ifndef MONO_LLVM_ANALYSIS_STRIP_CASTS_HPP
#define MONO_LLVM_ANALYSIS_STRIP_CASTS_HPP

#include <llvm/IR/Operator.h>
#include <llvm/IR/Value.h>

namespace mono {

/// Returns v with the operations that do not change its address peeled off, so
/// two spellings of one runtime address compare equal.
///
/// A `freeze` is peeled as well. It gives an arbitrary address for a poison
/// operand. So the peel changes the answer only where a use of the unfrozen
/// value is undefined behaviour already.
inline const llvm::Value *
strip_casts (const llvm::Value *v)
{
	while (const auto *op = llvm::dyn_cast<llvm::Operator> (v)) {
		unsigned opcode = op->getOpcode ();

		if (opcode != llvm::Instruction::BitCast
		    && opcode != llvm::Instruction::PtrToInt
		    && opcode != llvm::Instruction::IntToPtr
		    && opcode != llvm::Instruction::Freeze)
			break;
		v = op->getOperand (0);
	}

	return v;
}

} // namespace mono

#endif

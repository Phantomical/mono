/**
 * \file
 * \brief Comparing two operands that name one runtime value.
 */

#ifndef MONO_LLVM_ANALYSIS_STRIP_CASTS_HPP
#define MONO_LLVM_ANALYSIS_STRIP_CASTS_HPP

#include <llvm/IR/Operator.h>
#include <llvm/IR/Value.h>

namespace mono {

/// Returns v with any address-preserving cast peeled off, so two spellings of
/// one runtime address compare equal.
inline const llvm::Value *
strip_casts (const llvm::Value *v)
{
	while (const auto *op = llvm::dyn_cast<llvm::Operator> (v)) {
		unsigned opcode = op->getOpcode ();

		if (opcode != llvm::Instruction::BitCast
		    && opcode != llvm::Instruction::PtrToInt
		    && opcode != llvm::Instruction::IntToPtr)
			break;
		v = op->getOperand (0);
	}

	return v;
}

} // namespace mono

#endif

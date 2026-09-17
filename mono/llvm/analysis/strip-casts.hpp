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

namespace detail {

/// v with the casts that never change its value peeled off: a bitcast, a
/// freeze, or a ptrtoint/inttoptr round trip that lands back on its own
/// starting type.
inline const llvm::Value *
strip_identity_casts (const llvm::Value *v)
{
	while (const auto *op = llvm::dyn_cast<llvm::Operator> (v)) {
		unsigned opcode = op->getOpcode ();

		if (opcode == llvm::Instruction::BitCast || opcode == llvm::Instruction::Freeze) {
			v = op->getOperand (0);
			continue;
		}

		if (opcode != llvm::Instruction::PtrToInt && opcode != llvm::Instruction::IntToPtr)
			break;

		const auto *inner = llvm::dyn_cast<llvm::Operator> (op->getOperand (0));
		unsigned other = opcode == llvm::Instruction::PtrToInt ? llvm::Instruction::IntToPtr
		                                                       : llvm::Instruction::PtrToInt;

		if (!inner || inner->getOpcode () != other
		    || inner->getOperand (0)->getType () != op->getType ())
			break;

		v = inner->getOperand (0);
	}

	return v;
}

} // namespace detail

/// Whether a and b name one runtime address, once a bitcast, a freeze, and a
/// ptrtoint/inttoptr round trip are peeled from each.
///
/// A lone ptrtoint or inttoptr is peeled only where both sides still carry
/// one after that. Peeling one side alone would let an integer that merely
/// holds a pointer's bits compare equal to that pointer, a different runtime
/// value.
inline bool
is_same_address (const llvm::Value *a, const llvm::Value *b)
{
	for (;;) {
		a = detail::strip_identity_casts (a);
		b = detail::strip_identity_casts (b);

		if (a == b)
			return true;

		const auto *op_a = llvm::dyn_cast<llvm::Operator> (a);
		const auto *op_b = llvm::dyn_cast<llvm::Operator> (b);

		if (!op_a || !op_b || op_a->getOpcode () != op_b->getOpcode ())
			return false;
		if (op_a->getOpcode () != llvm::Instruction::PtrToInt
		    && op_a->getOpcode () != llvm::Instruction::IntToPtr)
			return false;

		a = op_a->getOperand (0);
		b = op_b->getOperand (0);
	}
}

} // namespace mono

#endif

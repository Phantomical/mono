#include "method-to-llvm.hpp"
#include "mono/metadata/class-internals.h"
#include "mono/metadata/metadata.h"
#include <llvm/IR/Constants.h>
#include <llvm/Support/ErrorHandling.h>

namespace mono {

namespace {

/// Returns the shift amount, resized to type's width and masked into its range.
///
/// An LLVM shift needs both operands in one type. Table III.6 pairs an int32 or native
/// int amount with any of the three shiftable value types. One operand always changes
/// width to match the other.
///
/// The mask keeps the result defined. If the shift amount reaches the operand's width,
/// the spec leaves the result unspecified. LLVM treats that case as poison instead, and
/// poison can affect code far from where it happened.
///
/// The mask costs an extra instruction at run time. Codegen runs through FastISel,
/// since this backend always compiles at `CodeGenOptLevel::None`, and FastISel does not
/// fold the mask into the shift instruction. What is left also matches what the classic
/// JIT computes for the same shift.
llvm::Value *
shift_amount (llvm::IRBuilder<> &builder, llvm::Value *amount, llvm::Type *type)
{
	unsigned bits = type->getIntegerBitWidth ();
	llvm::Value *value = amount;

	if (value->getType ()->isPointerTy ())
		value = builder.CreatePtrToInt (value, type);

	unsigned from = value->getType ()->getIntegerBitWidth ();

	if (from > bits)
		value = builder.CreateTrunc (value, type);
	else if (from < bits)
		value = builder.CreateZExt (value, type);

	return builder.CreateAnd (value, llvm::ConstantInt::get (type, bits - 1));
}

} // namespace

/*
 * III.3.3  and - bitwise AND
 *
 *   Format   Instruction   Description
 *   5F       and           Bitwise AND of two integral values, returns an integral
 *                          value.
 *
 * Stack Transition:
 *
 *   ..., value1, value2 -> ..., result
 *
 * Description:
 *
 *   The and instruction computes the bitwise AND of value1 and value2 and pushes the
 *   result on the stack. The acceptable operand types and their corresponding result
 *   data type are encapsulated in Table 5: Integer Operations.
 *
 * Exceptions:
 *
 *   None.
 *
 * Correctness and Verifiability:
 *
 *   See Table 5: Integer Operations.
 */
llvm::Error
MethodLLVMEmitter::emit_and (MonoIrBuilder &builder)
{
	llvm::Expected<BinaryOperands> operands = pop_binary_operands (BinaryOp::And);
	if (!operands)
		return operands.takeError ();

	auto [value1, value2, result] = *operands;
	llvm::Expected<llvm::Type *> type = convert_type (result);
	if (!type)
		return type.takeError ();

	push_stack (builder.CreateAnd (coerce (builder, value1.value, *type),
	                               coerce (builder, value2.value, *type)),
	            result);
	return llvm::Error::success ();
}

/*
 * III.3.53  or - bitwise OR
 *
 *   Format   Instruction   Description
 *   60       or            Bitwise OR of two integer values, returns an integer.
 *
 * Stack Transition:
 *
 *   ..., value1, value2 -> ..., result
 *
 * Description:
 *
 *   The or instruction computes the bitwise OR of the top two values on the stack and
 *   leaves the result on the stack.
 *
 *   The acceptable operand types and their corresponding result data type are
 *   encapsulated in Table 5: Integer Operations.
 *
 * Exceptions:
 *
 *   None.
 *
 * Correctness and Verifiability:
 *
 *   See Table 5: Integer Operations.
 */
llvm::Error
MethodLLVMEmitter::emit_or (MonoIrBuilder &builder)
{
	llvm::Expected<BinaryOperands> operands = pop_binary_operands (BinaryOp::Or);
	if (!operands)
		return operands.takeError ();

	auto [value1, value2, result] = *operands;
	llvm::Expected<llvm::Type *> type = convert_type (result);
	if (!type)
		return type.takeError ();

	push_stack (builder.CreateOr (coerce (builder, value1.value, *type),
	                              coerce (builder, value2.value, *type)),
	            result);
	return llvm::Error::success ();
}

/*
 * III.3.67  xor - bitwise XOR
 *
 *   Format   Assembly Format   Description
 *   61       xor               Bitwise XOR of integer values, returns an integer.
 *
 * Stack Transition:
 *
 *   ..., value1, value2 -> ..., result
 *
 * Description:
 *
 *   The xor instruction computes the bitwise XOR of value1 and value2 and leaves the
 *   result on the stack.
 *
 *   The acceptable operand types and their corresponding result data type is
 *   encapsulated in Table III.5: Integer Operations.
 *
 * Exceptions:
 *
 *   None.
 *
 * Correctness and Verifiability:
 *
 *   See Table III.5: Integer Operations.
 */
llvm::Error
MethodLLVMEmitter::emit_xor (MonoIrBuilder &builder)
{
	llvm::Expected<BinaryOperands> operands = pop_binary_operands (BinaryOp::Xor);
	if (!operands)
		return operands.takeError ();

	auto [value1, value2, result] = *operands;
	llvm::Expected<llvm::Type *> type = convert_type (result);
	if (!type)
		return type.takeError ();

	push_stack (builder.CreateXor (coerce (builder, value1.value, *type),
	                               coerce (builder, value2.value, *type)),
	            result);
	return llvm::Error::success ();
}

/*
 * III.3.52  not - bitwise complement
 *
 *   Format   Assembly Format   Description
 *   66       not               Bitwise complement.
 *
 * Stack Transition:
 *
 *   ..., value -> ..., result
 *
 * Description:
 *
 *   The not instruction computes the bitwise complement of the integer value on top of
 *   the stack and leaves the result on top of the stack. The return type is the same as
 *   the operand type.
 *
 *   The acceptable operand types and their corresponding result data type are
 *   encapsulated in Table 5: Integer Operations.
 *
 * Exceptions:
 *
 *   None.
 *
 * Correctness and Verifiability:
 *
 *   See Table 5: Integer Operations.
 */
llvm::Error
MethodLLVMEmitter::emit_not (MonoIrBuilder &builder)
{
	if (stack.empty ())
		return unbalanced_stack (1);

	StackValue value = get_stack (0);
	StackType type = stack_type (value.type);
	MonoType *result;

	// Table III.5 has one operand on each axis. With only one operand, `not` reads
	// its diagonal: int32, int64 and native int, each paired with itself.
	switch (type) {
	case Int32:
		result = mono_get_int32_type ();
		break;
	case Int64:
		result = m_class_get_byval_arg (mono_defaults.int64_class);
		break;
	case NativeInt:
		result = mono_get_int_type ();
		break;
	default:
		return invalid_il (llvm::Twine ("not is not defined for operand type ")
		                   + describe (value.type, type));
	}

	llvm::Expected<llvm::Type *> ltype = convert_type (result);
	if (!ltype)
		return ltype.takeError ();

	llvm::Value *complement = builder.CreateNot (coerce (builder, value.value, *ltype));

	pop_stack (1);
	push_stack (complement, result);
	return llvm::Error::success ();
}

/*
 * III.3.58  shl - shift integer left
 *
 *   Format   Assembly Format   Description
 *   62       shl               Shift an integer left (shifting in zeros), return an
 *                              integer.
 *
 * Stack Transition:
 *
 *   ..., value, shiftAmount -> ..., result
 *
 * Description:
 *
 *   The shl instruction shifts value (int32, int64 or native int) left by the number of
 *   bits specified by shiftAmount. shiftAmount is of type int32 or native int. The
 *   return value is unspecified if shiftAmount is greater than or equal to the width of
 *   value. See Table III.6: Shift Operations for details of which operand types are
 *   allowed, and their corresponding result type.
 *
 * Exceptions:
 *
 *   None.
 *
 * Correctness and Verifiability:
 *
 *   See Table 5: Integer Operations.
 *
 *
 * III.3.59  shr - shift integer right
 *
 *   Format   Assembly Format   Description
 *   63       shr               Shift an integer right (shift in sign), return an
 *                              integer.
 *
 * Stack Transition:
 *
 *   ..., value, shiftAmount -> ..., result
 *
 * Description:
 *
 *   The shr instruction shifts value (int32, int64 or native int) right by the number
 *   of bits specified by shiftAmount. shiftAmount is of type int32 or native int. The
 *   return value is unspecified if shiftAmount is greater than or equal to the width of
 *   value. shr replicates the high order bit on each shift, preserving the sign of the
 *   original value in result. See Table III.6: Shift Operations for details of which
 *   operand types are allowed, and their corresponding result type.
 *
 * Exceptions:
 *
 *   None.
 *
 * Correctness and Verifiability:
 *
 *   See Table 5: Integer Operations.
 *
 *
 * III.3.60  shr.un - shift integer right, unsigned
 *
 *   Format   Assembly Format   Description
 *   64       shr.un            Shift an integer right (shift in zero), return an
 *                              integer.
 *
 * Stack Transition:
 *
 *   ..., value, shiftAmount -> ..., result
 *
 * Description:
 *
 *   The shr.un instruction shifts value (int32, int64 or native int) right by the
 *   number of bits specified by shiftAmount. shiftAmount is of type int32 or native
 *   int. The return value is unspecified if shiftAmount is greater than or equal to the
 *   width of value. shr.un inserts a zero bit on each shift. See Table III.6: Shift
 *   Operations for details of which operand types are allowed, and their corresponding
 *   result type.
 *
 * Exceptions:
 *
 *   None.
 *
 * Correctness and Verifiability:
 *
 *   See Table 5: Integer Operations.
 */
llvm::Error
MethodLLVMEmitter::emit_shift (MonoIrBuilder &builder, BinaryOp op)
{
	llvm::Expected<BinaryOperands> operands = pop_binary_operands (op);
	if (!operands)
		return operands.takeError ();

	auto [value, amount, result] = *operands;
	llvm::Expected<llvm::Type *> type = convert_type (result);
	if (!type)
		return type.takeError ();

	llvm::Value *shifted = coerce (builder, value.value, *type);
	llvm::Value *by = shift_amount (builder, amount.value, *type);
	llvm::Value *complete;

	switch (op) {
	case BinaryOp::Shl:
		complete = builder.CreateShl (shifted, by);
		break;
	case BinaryOp::Shr:
		complete = builder.CreateAShr (shifted, by);
		break;
	case BinaryOp::ShrUn:
		complete = builder.CreateLShr (shifted, by);
		break;
	default:
		llvm::report_fatal_error ("emit_shift: not a shift instruction");
	}

	push_stack (complete, result);
	return llvm::Error::success ();
}

} // namespace mono

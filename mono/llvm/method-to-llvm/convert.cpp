#include "method-to-llvm.hpp"
#include "mono/metadata/class-internals.h"
#include "mono/metadata/metadata.h"
#include <llvm/ADT/APInt.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Intrinsics.h>
#include <llvm/Support/ErrorHandling.h>

#include <algorithm>

namespace mono {

namespace {

/// What Table III.8 says a conversion does.
///
/// Its four integer-to-integer cells - Nop, Truncate, Sign extend, Zero extend - come
/// out as one here. Which of them applies is decided by the two widths and by how the
/// target type fills, both of which the descriptor below already carries. X is the
/// table's invalid box.
enum Action { X, IntToInt, PtrToInt, FloatToInt, IntToFloat, FloatToFloat };

/// The rows of Table III.8. The signed and unsigned target of a width share one, since
/// the table gives them the same cells.
enum TargetRow { NarrowIntRow, Int32Row, Int64Row, NativeIntRow, FloatRow, TARGET_ROW_COUNT };

/*
 * ECMA-335 III.1.5, Table III.8: Conversion Operations. Indexed [target][source].
 *
 * The two pointer columns are the spec's "Stop GC tracking": once an object reference
 * or a managed pointer has become a number the collector no longer updates it, which
 * is why only the 64-bit and native-int rows accept one and why the table shades those
 * cells as unverifiable.
 */
constexpr Action CONVERSIONS[TARGET_ROW_COUNT][STACK_TYPE_COUNT] = {
	/*              int32       int64       native int  F             &         O */
	/* int8/16 */ { IntToInt,   IntToInt,   IntToInt,   FloatToInt,   X,        X },
	/* int32   */ { IntToInt,   IntToInt,   IntToInt,   FloatToInt,   X,        X },
	/* int64   */ { IntToInt,   IntToInt,   IntToInt,   FloatToInt,   PtrToInt, PtrToInt },
	/* nint    */ { IntToInt,   IntToInt,   IntToInt,   FloatToInt,   PtrToInt, PtrToInt },
	/* float   */ { IntToFloat, IntToFloat, IntToFloat, FloatToFloat, X,        X },
};

TargetRow
row_of (ConvType type)
{
	switch (type) {
	case ConvType::I1:
	case ConvType::U1:
	case ConvType::I2:
	case ConvType::U2:
		return NarrowIntRow;
	case ConvType::I4:
	case ConvType::U4:
		return Int32Row;
	case ConvType::I8:
	case ConvType::U8:
		return Int64Row;
	case ConvType::I:
	case ConvType::U:
		return NativeIntRow;
	case ConvType::R4:
	case ConvType::R8:
		return FloatRow;
	}

	llvm::report_fatal_error ("row_of: unknown conversion target");
}

/// The target type itself, as against the wider slot the result is pushed in.
struct Target {
	unsigned bits;
	bool is_signed;
	bool is_float;
};

constexpr unsigned NATIVE_BITS = TARGET_SIZEOF_VOID_P * 8;

Target
target_of (ConvType type)
{
	switch (type) {
	case ConvType::I1:
		return { 8, true, false };
	case ConvType::U1:
		return { 8, false, false };
	case ConvType::I2:
		return { 16, true, false };
	case ConvType::U2:
		return { 16, false, false };
	case ConvType::I4:
		return { 32, true, false };
	case ConvType::U4:
		return { 32, false, false };
	case ConvType::I8:
		return { 64, true, false };
	case ConvType::U8:
		return { 64, false, false };
	case ConvType::I:
		return { NATIVE_BITS, true, false };
	case ConvType::U:
		return { NATIVE_BITS, false, false };
	case ConvType::R4:
		return { 32, true, true };
	case ConvType::R8:
		return { 64, true, true };
	}

	llvm::report_fatal_error ("target_of: unknown conversion target");
}

/// How wide the result sits on the evaluation stack, which the spec fixes at a minimum
/// of four bytes however narrow the target type is.
unsigned
stack_bits (Target target)
{
	return std::max (target.bits, 32u);
}

/// The name Table III.8 gives this target, for the refusal message.
const char *
target_name (ConvType type)
{
	switch (type) {
	case ConvType::I1:
		return "int8";
	case ConvType::U1:
		return "unsigned int8";
	case ConvType::I2:
		return "int16";
	case ConvType::U2:
		return "unsigned int16";
	case ConvType::I4:
		return "int32";
	case ConvType::U4:
		return "unsigned int32";
	case ConvType::I8:
		return "int64";
	case ConvType::U8:
		return "unsigned int64";
	case ConvType::I:
		return "native int";
	case ConvType::U:
		return "native unsigned int";
	case ConvType::R4:
		return "float32";
	case ConvType::R8:
		return "float64";
	}

	llvm::report_fatal_error ("target_name: unknown conversion target");
}

llvm::Type *
native_int_type (llvm::IRBuilder<> &builder)
{
	return builder.getIntNTy (NATIVE_BITS);
}

/// VALUE at BITS wide, filled the way IS_SIGNED says when it has to grow.
llvm::Value *
adjust (llvm::IRBuilder<> &builder, llvm::Value *value, unsigned bits, bool is_signed)
{
	unsigned from = value->getType ()->getIntegerBitWidth ();
	llvm::Type *to = builder.getIntNTy (bits);

	if (from == bits)
		return value;
	if (from > bits)
		return builder.CreateTrunc (value, to);

	return is_signed ? builder.CreateSExt (value, to) : builder.CreateZExt (value, to);
}

/// The operand as a number.
///
/// Table III.8's pointer columns and MONO_TYPE_PTR both reach the integer paths this
/// way; the difference between them is one of GC tracking, which the table has already
/// ruled on by the time this runs.
llvm::Value *
as_integer (llvm::IRBuilder<> &builder, llvm::Value *value)
{
	if (value->getType ()->isPointerTy ())
		return builder.CreatePtrToInt (value, native_int_type (builder));

	return value;
}

/// VALUE narrowed or widened to TARGET's own width and then back out to the stack's.
///
/// That round trip is what leaves conv.u1 of 0x1234ABCD as 205 and conv.i1 of it as
/// -51: both truncate to 0xCD, and only the fill differs.
llvm::Value *
int_to_int (llvm::IRBuilder<> &builder, llvm::Value *value, Target target)
{
	llvm::Value *narrowed = adjust (builder, value, target.bits, target.is_signed);

	return adjust (builder, narrowed, stack_bits (target), target.is_signed);
}

/// VALUE truncated toward zero into TARGET.
///
/// The spec says that this returns an unspecified value if the value doesn't
/// fit in the target type. We use freeze to represent this in the LLVM IR.
llvm::Value *
float_to_int (llvm::IRBuilder<> &builder, llvm::Value *value, Target target)
{
	llvm::Type *to = builder.getIntNTy (stack_bits (target));
	llvm::Value *converted = target.is_signed ? builder.CreateFPToSI (value, to)
	                                          : builder.CreateFPToUI (value, to);

	/* Convert at the stack width and narrow after, the way the classic JIT does. */
	return int_to_int (builder, builder.CreateFreeze (converted), target);
}

/// The type the result is tracked as once it is pushed.
MonoType *
result_type (ConvType type)
{
	switch (row_of (type)) {
	case NarrowIntRow:
	case Int32Row:
		return mono_get_int32_type ();
	case Int64Row:
		return m_class_get_byval_arg (mono_defaults.int64_class);
	case NativeIntRow:
		return mono_get_int_type ();
	case FloatRow:
		return target_of (type).bits == 32
		               ? m_class_get_byval_arg (mono_defaults.single_class)
		               : m_class_get_byval_arg (mono_defaults.double_class);
	default:
		llvm::report_fatal_error ("result_type: unknown conversion target");
	}
}

} // namespace

/// Refuse the conversions Table III.8 leaves blank.
llvm::Error
MethodLLVMEmitter::check_conversion (ConvType type, MonoType *source)
{
	StackType from = stack_type (source);

	if (from != Invalid && CONVERSIONS[row_of (type)][from] != X)
		return llvm::Error::success ();

	return invalid_il (llvm::Twine ("conversion to ") + target_name (type)
	                   + " is not defined for operand type " + describe (source, from));
}

/*
 * III.3.27  conv.<to type> - data conversion
 *
 *   Format   Assembly Format   Description
 *   67       conv.i1           Convert to int8, pushing int32 on stack.
 *   68       conv.i2           Convert to int16, pushing int32 on stack.
 *   69       conv.i4           Convert to int32, pushing int32 on stack.
 *   6A       conv.i8           Convert to int64, pushing int64 on stack.
 *   6B       conv.r4           Convert to float32, pushing F on stack.
 *   6C       conv.r8           Convert to float64, pushing F on stack.
 *   D2       conv.u1           Convert to unsigned int8, pushing int32 on stack.
 *   D1       conv.u2           Convert to unsigned int16, pushing int32 on stack.
 *   6D       conv.u4           Convert to unsigned int32, pushing int32 on stack.
 *   6E       conv.u8           Convert to unsigned int64, pushing int64 on stack.
 *   D3       conv.i            Convert to native int, pushing native int on stack.
 *   E0       conv.u            Convert to native unsigned int, pushing native int on
 *                              stack.
 *   76       conv.r.un         Convert unsigned integer to floating-point, pushing F on
 *                              stack.
 *
 * Stack Transition:
 *
 *   ..., value -> ..., result
 *
 * Description:
 *
 *   Convert the value on top of the stack to the type specified in the opcode, and
 *   leave that converted value on the top of the stack. The verification type on the
 *   stack is as specified in §III.1.8.1.2.1 for the target type. Note that integer
 *   values of less than 4 bytes are extended to int32 (not native int) when they are
 *   loaded onto the evaluation stack, and floating-point values are converted to the F
 *   type.
 *
 *   Conversion from floating-point numbers to integral values truncates the number
 *   toward zero. When converting from a float64 to a float32, precision might be lost.
 *   If value is too large to fit in a float32, the IEC 60559:1989 positive infinity (if
 *   value is positive) or IEC 60559:1989 negative infinity (if value is negative) is
 *   returned. If overflow occurs when converting one integer type to another, the
 *   high-order bits are silently truncated. If the result is smaller than an int32,
 *   then the value is sign-extended to fill the slot.
 *
 *   If overflow occurs converting a floating-point type to an integer, or if the
 *   floating-point value being converted to an integer is a NaN, the value returned is
 *   unspecified. The conv.r.un operation takes an integer off the stack, interprets it
 *   as unsigned, and replaces it with an F type floating-point number to represent the
 *   integer.
 *
 *   The acceptable operand types and their corresponding result data type is
 *   encapsulated in Table 8: Conversion Operations.
 *
 * Exceptions:
 *
 *   No exceptions are ever thrown. See conv.ovf for instructions that will throw an
 *   exception when the result type cannot properly represent the result value.
 *
 * Correctness:
 *
 *   Correct CIL has at least one value, of a type specified in Table 8: Conversion
 *   Operations, on the stack.
 *
 * Verifiability:
 *
 *   The table Table 8: Conversion Operations specifies a restricted set of types that
 *   are acceptable in verified code.
 */
llvm::Error
MethodLLVMEmitter::emit_conv (MonoIrBuilder &builder, ConvType type)
{
	if (stack.empty ())
		return unbalanced_stack (1);

	StackValue value = get_stack (0);
	if (llvm::Error error = check_conversion (type, value.type))
		return error;

	Target target = target_of (type);
	bool from_float = stack_type (value.type) == Float;
	llvm::Value *result;

	if (target.is_float) {
		llvm::Type *to = target.bits == 32 ? builder.getFloatTy () : builder.getDoubleTy ();

		/* conv.r4 and conv.r8 read the integer as signed; conv.r.un is the other one. */
		result = from_float ? builder.CreateFPCast (value.value, to)
		                    : builder.CreateSIToFP (as_integer (builder, value.value), to);
	} else if (from_float) {
		result = float_to_int (builder, value.value, target);
	} else {
		result = int_to_int (builder, as_integer (builder, value.value), target);
	}

	pop_stack (1);
	push_stack (result, result_type (type));
	return llvm::Error::success ();
}

/// conv.r.un, which is the one conv that reads its integer operand as unsigned.
llvm::Error
MethodLLVMEmitter::emit_conv_r_un (MonoIrBuilder &builder)
{
	if (stack.empty ())
		return unbalanced_stack (1);

	StackValue value = get_stack (0);
	if (llvm::Error error = check_conversion (ConvType::R8, value.type))
		return error;

	llvm::Type *to = builder.getDoubleTy ();
	llvm::Value *result =
		stack_type (value.type) == Float
			? builder.CreateFPCast (value.value, to)
			: builder.CreateUIToFP (as_integer (builder, value.value), to);

	pop_stack (1);
	push_stack (result, m_class_get_byval_arg (mono_defaults.double_class));
	return llvm::Error::success ();
}

/// VALUE converted to TARGET, throwing OverflowException if it does not fit.
///
/// The round trip is the check: widen to something that holds both the source's range
/// and the target's, narrow to the target, then widen back the way the target fills.
/// A value that does not come back unchanged did not fit.
///
/// The extra bit is what makes that work when the two widths are equal but their signs
/// disagree - conv.ovf.u8 of int32 -1 has to throw, and at 64 bits the narrowing step
/// is a no-op that would otherwise hide it.
llvm::Value *
MethodLLVMEmitter::emit_checked_int_conv (MonoIrBuilder &builder, llvm::Value *value,
                                          ConvType type, bool source_unsigned)
{
	Target target = target_of (type);
	unsigned wide = std::max (value->getType ()->getIntegerBitWidth (), target.bits) + 1;
	llvm::Value *widened = adjust (builder, value, wide, !source_unsigned);
	llvm::Value *narrowed = adjust (builder, widened, target.bits, target.is_signed);
	llvm::Value *back = adjust (builder, narrowed, wide, target.is_signed);

	emit_cond_exception (builder, builder.CreateICmpNE (back, widened), "OverflowException");

	return adjust (builder, narrowed, stack_bits (target), target.is_signed);
}

/// VALUE truncated toward zero into TARGET, throwing OverflowException if it does not
/// fit.
///
/// This is the one float conversion that pays for the saturating intrinsic, because it
/// is the one that has to know whether the value fit: a frozen poison is a fine result
/// but says nothing about where it came from.
///
/// Saturating two bits wider than the target is what makes the check possible: a value
/// that overflowed is still outside the target's range once clamped, so the bounds can
/// be compared as integers, where they are exact. Comparing against them as floats
/// would need 2^(n-1)+1 to be representable, and at 64 bits it is not.
///
/// The saturation is signed even for an unsigned target, so that a negative operand
/// stays negative and fails the range check rather than clamping to zero and passing.
/// NaN saturates to zero, which would pass, so it is asked about on its own.
llvm::Value *
MethodLLVMEmitter::emit_checked_float_conv (MonoIrBuilder &builder, llvm::Value *value,
                                            ConvType type)
{
	Target target = target_of (type);
	unsigned wide = target.bits + 2;
	llvm::Type *to = builder.getIntNTy (wide);
	llvm::Value *saturated = builder.CreateIntrinsic (llvm::Intrinsic::fptosi_sat,
	                                                  { to, value->getType () }, { value });

	llvm::APInt lo = target.is_signed
	                         ? llvm::APInt::getSignedMinValue (target.bits).sext (wide)
	                         : llvm::APInt::getZero (wide);
	llvm::APInt hi = target.is_signed
	                         ? llvm::APInt::getSignedMaxValue (target.bits).sext (wide)
	                         : llvm::APInt::getMaxValue (target.bits).zext (wide);

	llvm::Value *out_of_range = builder.CreateOr (
		builder.CreateICmpSLT (saturated, llvm::ConstantInt::get (context (), lo)),
		builder.CreateICmpSGT (saturated, llvm::ConstantInt::get (context (), hi)));

	emit_cond_exception (builder,
	                     builder.CreateOr (builder.CreateFCmpUNO (value, value), out_of_range),
	                     "OverflowException");

	return adjust (builder, saturated, stack_bits (target), target.is_signed);
}

/*
 * III.3.28  conv.ovf.<to type> - data conversion with overflow detection
 *
 *   Format   Assembly Format   Description
 *   B3       conv.ovf.i1       Convert to an int8 (on the stack as int32) and throw an
 *                              exception on overflow.
 *   B5       conv.ovf.i2       Convert to an int16 (on the stack as int32) and throw an
 *                              exception on overflow.
 *   B7       conv.ovf.i4       Convert to an int32 (on the stack as int32) and throw an
 *                              exception on overflow.
 *   B9       conv.ovf.i8       Convert to an int64 (on the stack as int64) and throw an
 *                              exception on overflow.
 *   B4       conv.ovf.u1       Convert to an unsigned int8 (on the stack as int32) and
 *                              throw an exception on overflow.
 *   B6       conv.ovf.u2       Convert to an unsigned int16 (on the stack as int32) and
 *                              throw an exception on overflow.
 *   B8       conv.ovf.u4       Convert to an unsigned int32 (on the stack as int32) and
 *                              throw an exception on overflow
 *   BA       conv.ovf.u8       Convert to an unsigned int64 (on the stack as int64) and
 *                              throw an exception on overflow.
 *   D4       conv.ovf.i        Convert to a native int (on the stack as native int) and
 *                              throw an exception on overflow.
 *   D5       conv.ovf.u        Convert to a native unsigned int (on the stack as native
 *                              int) and throw an exception on overflow.
 *
 * Stack Transition:
 *
 *   ..., value -> ..., result
 *
 * Description:
 *
 *   Convert the value on top of the stack to the type specified in the opcode, and
 *   leave that converted value on the top of the stack. If the result cannot be
 *   represented in the target type, an exception is thrown.
 *
 *   Conversions from floating-point numbers to integral values truncate the number
 *   toward zero. Note that integer values of less than 4 bytes are extended to int32
 *   (not native int) on the evaluation stack.
 *
 *   The acceptable operand types and their corresponding result data type is
 *   encapsulated in Table 8: Conversion Operations.
 *
 * Exceptions:
 *
 *   System.OverflowException is thrown if the result cannot be represented in the
 *   result type.
 *
 * Correctness:
 *
 *   Correct CIL has at least one value, of a type specified in Table 8: Conversion
 *   Operations, on the stack.
 *
 * Verifiability:
 *
 *   The table Table 8: Conversion Operations specifies a restricted set of types that
 *   are acceptable in verified code.
 *
 *
 * III.3.29  conv.ovf.<to type>.un - unsigned data conversion with overflow detection
 *
 *   Format   Assembly Format   Description
 *   82       conv.ovf.i1.un    Convert unsigned to an int8 (on the stack as int32) and
 *                              throw an exception on overflow.
 *   83       conv.ovf.i2.un    Convert unsigned to an int16 (on the stack as int32) and
 *                              throw an exception on overflow.
 *   84       conv.ovf.i4.un    Convert unsigned to an int32 (on the stack as int32) and
 *                              throw an exception on overflow.
 *   85       conv.ovf.i8.un    Convert unsigned to an int64 (on the stack as int64) and
 *                              throw an exception on overflow.
 *   86       conv.ovf.u1.un    Convert unsigned to an unsigned int8 (on the stack as
 *                              int32) and throw an exception on overflow.
 *   87       conv.ovf.u2.un    Convert unsigned to an unsigned int16 (on the stack as
 *                              int32) and throw an exception on overflow.
 *   88       conv.ovf.u4.un    Convert unsigned to an unsigned int32 (on the stack as
 *                              int32) and throw an exception on overflow.
 *   89       conv.ovf.u8.un    Convert unsigned to an unsigned int64 (on the stack as
 *                              int64) and throw an exception on overflow.
 *   8A       conv.ovf.i.un     Convert unsigned to a native int (on the stack as native
 *                              int) and throw an exception on overflow.
 *   8B       conv.ovf.u.un     Convert unsigned to a native unsigned int (on the stack
 *                              as native int) and throw an exception on overflow.
 *
 * Stack Transition:
 *
 *   ..., value -> ..., result
 *
 * Description:
 *
 *   Convert the value on top of the stack to the type specified in the opcode, and
 *   leave that converted value on the top of the stack. If the value cannot be
 *   represented, an exception is thrown. The item on the top of the stack is treated as
 *   an unsigned value before the conversion.
 *
 *   Conversions from floating-point numbers to integral values truncate the number
 *   toward zero. Note that integer values of less than 4 bytes are extended to int32
 *   (not native int) on the evaluation stack.
 *
 *   The acceptable operand types and their corresponding result data type are
 *   encapsulated in Table 8: Conversion Operations.
 *
 * Exceptions:
 *
 *   System.OverflowException is thrown if the result cannot be represented in the
 *   result type.
 *
 * Correctness:
 *
 *   Correct CIL has at least one value, of a type specified in Table 8: Conversion
 *   Operations, on the stack.
 *
 * Verifiability:
 *
 *   The table Table 8: Conversion Operations specifies a restricted set of types that
 *   are acceptable in verified code.
 */
llvm::Error
MethodLLVMEmitter::emit_conv_ovf (MonoIrBuilder &builder, ConvType type, bool source_unsigned)
{
	if (stack.empty ())
		return unbalanced_stack (1);

	StackValue value = get_stack (0);
	if (llvm::Error error = check_conversion (type, value.type))
		return error;

	llvm::Value *result;

	/*
	 * There is no conv.ovf.r, so the target is always integral. Reading the operand as
	 * unsigned is what .un asks for and only means anything for an integer, so a float
	 * operand converts the same either way.
	 */
	if (stack_type (value.type) == Float)
		result = emit_checked_float_conv (builder, value.value, type);
	else
		result = emit_checked_int_conv (builder, as_integer (builder, value.value), type,
		                                source_unsigned);

	pop_stack (1);
	push_stack (result, result_type (type));
	return llvm::Error::success ();
}

} // namespace mono

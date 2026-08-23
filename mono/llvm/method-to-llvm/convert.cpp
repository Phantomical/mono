#include "method-to-llvm.hpp"
#include "mono/metadata/class-internals.h"
#include "mono/metadata/metadata.h"
#include <llvm/ADT/APFloat.h>
#include <llvm/ADT/APInt.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/FPEnv.h>
#include <llvm/IR/Intrinsics.h>
#include <llvm/Support/ErrorHandling.h>

#include <algorithm>

namespace mono {

namespace {

/// A cell of Table III.8, the ECMA-335 conversion-operations table.
///
/// Table III.8 gives integer-to-integer conversions four separate cells: Nop, Truncate,
/// Sign extend and Zero extend. This enum merges them into one value, IntToInt. The two
/// widths and the target's own sign, both carried by the descriptor below, decide which
/// of the four applies. X marks a cell the table calls invalid.
enum Action {
	X,
	IntToInt,
	PtrToInt,
	FloatToInt,
	IntToFloat,
	FloatToFloat
};

/// A row of Table III.8, one row per target width.
///
/// A signed and an unsigned target of the same width share a row even where the table
/// gives them different cells. adjust () supplies the fill direction separately, from
/// target.is_signed.
enum TargetRow {
	NarrowIntRow,
	Int32Row,
	Int64Row,
	NativeIntRow,
	FloatRow,
	TARGET_ROW_COUNT
};

// ECMA-335 III.1.5, Table III.8: Conversion Operations. The first index is the target
// row (Convert-To). The second is the source column (Input from the evaluation stack).
//
// The & and O columns are the table's "Stop GC tracking" cells. Only the 64-bit and
// native-int rows accept them. Once a reference or a managed pointer becomes a number,
// the collector stops updating it. The table shades those cells: they are valid CIL
// but not verifiable.
constexpr Action CONVERSIONS[TARGET_ROW_COUNT][STACK_TYPE_COUNT] = {
	/*              int32       int64       native int  F             &         O */
	/* int8/16 */ {IntToInt, IntToInt, IntToInt, FloatToInt, X, X},
	/* int32   */ {IntToInt, IntToInt, IntToInt, FloatToInt, X, X},
	/* int64   */ {IntToInt, IntToInt, IntToInt, FloatToInt, PtrToInt, PtrToInt},
	/* nint    */ {IntToInt, IntToInt, IntToInt, FloatToInt, PtrToInt, PtrToInt},
	/* float   */ {IntToFloat, IntToFloat, IntToFloat, FloatToFloat, X, X},
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
		return {8, true, false};
	case ConvType::U1:
		return {8, false, false};
	case ConvType::I2:
		return {16, true, false};
	case ConvType::U2:
		return {16, false, false};
	case ConvType::I4:
		return {32, true, false};
	case ConvType::U4:
		return {32, false, false};
	case ConvType::I8:
		return {64, true, false};
	case ConvType::U8:
		return {64, false, false};
	case ConvType::I:
		return {NATIVE_BITS, true, false};
	case ConvType::U:
		return {NATIVE_BITS, false, false};
	case ConvType::R4:
		return {32, true, true};
	case ConvType::R8:
		return {64, true, true};
	}

	llvm::report_fatal_error ("target_of: unknown conversion target");
}

unsigned
stack_bits (Target target)
{
	return std::max (target.bits, 32u);
}

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

struct FloatBound {
	llvm::Constant *bound;
	llvm::CmpInst::Predicate pred;
};

/// bound converted to fp, with the predicate adjusted so the comparison still means what
/// exclusive meant against the exact integer bound.
///
/// Round-to-zero picks the representable float closest to bound on the range's own side. If
/// bound is not exactly representable, no float sits between the rounded value and the true
/// bound. Switching to inclusive at the rounded value then keeps the comparison exact.
FloatBound
float_bound (llvm::Type *fp, const llvm::APInt &bound, llvm::CmpInst::Predicate exclusive,
             llvm::CmpInst::Predicate inclusive)
{
	llvm::APFloat value (fp->getFltSemantics ());
	bool exact = value.convertFromAPInt (bound, /* IsSigned */ true,
	                                     llvm::APFloat::rmTowardZero) == llvm::APFloat::opOK;

	return {llvm::ConstantFP::get (fp, value), exact ? exclusive : inclusive};
}

/// value as an integer, converting it from a pointer if it is one.
///
/// Table III.8's & and O columns and MONO_TYPE_PTR both reach the integer paths through
/// here. Only the GC tracking differs between them. check_conversion () has already ruled
/// on that before this runs.
llvm::Value *
as_integer (llvm::IRBuilder<> &builder, llvm::Value *value)
{
	if (value->getType ()->isPointerTy ())
		return builder.CreatePtrToInt (value, native_int_type (builder));

	return value;
}

/// value narrowed or widened to target's own width, then widened back out to the
/// stack's width.
///
/// That round trip is why conv.u1 of 0x1234ABCD gives 205 and conv.i1 of it gives -51.
/// Both truncate to 0xCD first. Only the fill on the way back out differs.
llvm::Value *
int_to_int (llvm::IRBuilder<> &builder, llvm::Value *value, Target target)
{
	llvm::Value *narrowed = adjust (builder, value, target.bits, target.is_signed);

	return adjust (builder, narrowed, stack_bits (target), target.is_signed);
}

/// value truncated toward zero into to, through one of the constrained intrinsics.
///
/// The spec leaves the out-of-range result unspecified. Any answer is legal, but every
/// path must reach the same one, and plain fptosi and fptoui cannot do that. They are
/// poison out of range, and poison is not a value. LLVM folds it to zero wherever it can
/// see the operand. A value known only at run time instead keeps what the hardware
/// conversion left behind. So one call path converts -1.0f to ushort as 65535 and
/// another converts it as 0, with only the constant between them.
///
/// The constrained intrinsics carry no poison clause, and LLVM constant folds none of
/// them, so each one reaches the target's own conversion instruction. On amd64 that is
/// cvttsd2si, which answers with the integer indefinite value. The interpreter's C cast
/// compiles to the same instruction, so the two engines agree with no range test in
/// front of either of them.
///
/// fpexcept.ignore asks for the value and no more than the value. The strictfp attribute
/// is what a constrained intrinsic requires of the function that holds it.
llvm::Value *
constrained_float_to_int (llvm::IRBuilder<> &builder, llvm::Value *value, llvm::Type *to,
                          bool is_signed)
{
	builder.GetInsertBlock ()->getParent ()->addFnAttr (llvm::Attribute::StrictFP);

	llvm::Intrinsic::ID convert = is_signed
	                                      ? llvm::Intrinsic::experimental_constrained_fptosi
	                                      : llvm::Intrinsic::experimental_constrained_fptoui;

	return builder.CreateConstrainedFPCast (convert, value, to, {}, "", nullptr,
	                                        std::nullopt, llvm::fp::ebIgnore);
}

/// value truncated toward zero into an unsigned int64.
///
/// This is the one conversion amd64 has no instruction for. cvttsd2si is signed, so a
/// value from 2^63 up has to come back through the low half: subtract 2^63, convert, and
/// put the bit back.
///
/// The direction of the test decides what a NaN gives, and the two directions disagree.
/// The test here is "below 2^63", which a NaN fails. A NaN therefore takes the
/// subtraction and comes out as zero. mono_fconv_u8 () (mono/mini/jit-icalls.c) tests the
/// same way, and the interpreter reaches that helper for MINT_CONV_U8_R8. So this shape is
/// what the two engines agree on, not the one LLVM picks for itself.
llvm::Value *
float_to_uint64 (llvm::IRBuilder<> &builder, llvm::Value *value, llvm::Type *to)
{
	llvm::Value *two63 = llvm::ConstantFP::get (value->getType (), 9223372036854775808.0);
	llvm::Value *below = builder.CreateFCmpOLT (value, two63);
	llvm::Value *operand =
		builder.CreateSelect (below, value, builder.CreateFSub (value, two63));
	llvm::Value *converted = constrained_float_to_int (builder, operand, to, true);
	llvm::Value *put_back =
		builder.CreateAdd (converted, llvm::ConstantInt::get (to, 1ull << 63));

	return builder.CreateSelect (below, converted, put_back);
}

llvm::Value *
float_to_int (llvm::IRBuilder<> &builder, llvm::Value *value, Target target)
{
	llvm::Type *to = builder.getIntNTy (stack_bits (target));
	llvm::Value *converted =
		!target.is_signed && target.bits == 64
			? float_to_uint64 (builder, value, to)
			: constrained_float_to_int (builder, value, to, target.is_signed);

	// int_to_int () applies the same truncation conv already uses for oversized integers.
	return int_to_int (builder, converted, target);
}

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

		// conv.r4 and conv.r8 read the integer as signed. conv.r.un has its own emitter,
		// emit_conv_r_un (), for the unsigned case below.
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

/// conv.r.un reads its integer operand as unsigned. No other conv opcode does that.
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

/// value converted to target, throwing OverflowException if it does not fit.
///
/// The round trip is the check. Widen to a width that holds both the source's range and
/// the target's, narrow to the target, then widen back the way the target fills. A value
/// that does not come back unchanged did not fit.
///
/// The extra bit matters when the two widths are equal but the signs disagree. conv.ovf.u8
/// of int32 -1 must throw. Dropping the extra bit turns the 64-to-64 narrowing step into a
/// no-op. A no-op narrowing step catches nothing.
llvm::Value *
MethodLLVMEmitter::emit_checked_int_conv (MonoIrBuilder &builder, llvm::Value *value, ConvType type,
                                          bool source_unsigned)
{
	Target target = target_of (type);
	unsigned wide = std::max (value->getType ()->getIntegerBitWidth (), target.bits) + 1;
	llvm::Value *widened = adjust (builder, value, wide, !source_unsigned);
	llvm::Value *narrowed = adjust (builder, widened, target.bits, target.is_signed);
	llvm::Value *back = adjust (builder, narrowed, wide, target.is_signed);

	emit_cond_exception (builder, builder.CreateICmpNE (back, widened), "OverflowException");

	return adjust (builder, narrowed, stack_bits (target), target.is_signed);
}

/// value truncated toward zero into target, throwing OverflowException if it does not
/// fit.
///
/// The check runs on the operand, not on the result. The values that survive truncation
/// are exactly those in the open interval (lo-1, hi+1). Anything in (lo-1, lo] or [hi,
/// hi+1) loses its fraction and lands back inside the target's range. Both ends of that
/// interval sit one past the range, which is what makes them representable where lo and
/// hi are not. 2^(n-1) is a power of two, but 2^(n-1)-1 needs more mantissa bits than the
/// source type can carry.
///
/// The comparisons are ordered, so a NaN lands on neither side and overflows. The
/// conversion goes in the block the test falls through to, so it runs only on a value it
/// has a result for.
llvm::Value *
MethodLLVMEmitter::emit_checked_float_conv (MonoIrBuilder &builder, llvm::Value *value,
                                            ConvType type)
{
	Target target = target_of (type);
	llvm::Type *fp = value->getType ();
	unsigned wide = target.bits + 2;

	llvm::APInt lo = target.is_signed ? llvm::APInt::getSignedMinValue (target.bits).sext (wide)
	                                  : llvm::APInt::getZero (wide);
	llvm::APInt hi = target.is_signed ? llvm::APInt::getSignedMaxValue (target.bits).sext (wide)
	                                  : llvm::APInt::getMaxValue (target.bits).zext (wide);

	FloatBound below = float_bound (fp, lo - 1, llvm::CmpInst::FCMP_OGT, llvm::CmpInst::FCMP_OGE);
	FloatBound above = float_bound (fp, hi + 1, llvm::CmpInst::FCMP_OLT, llvm::CmpInst::FCMP_OLE);

	llvm::Value *in_range =
		builder.CreateAnd (builder.CreateFCmp (below.pred, value, below.bound),
	                           builder.CreateFCmp (above.pred, value, above.bound));

	emit_cond_exception (builder, builder.CreateNot (in_range), "OverflowException");

	llvm::Type *to = builder.getIntNTy (target.bits);
	llvm::Value *converted = constrained_float_to_int (builder, value, to, target.is_signed);

	return adjust (builder, converted, stack_bits (target), target.is_signed);
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

	// conv.ovf.r does not exist, so the target here is always an integer.
	// Reading the operand as unsigned is what .un asks for, and that only matters for an
	// integer operand. A float operand converts the same way whether or not .un is set.
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

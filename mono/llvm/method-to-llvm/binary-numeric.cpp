#include "method-to-llvm.hpp"
#include "mono/metadata/class-inlines.h"
#include "mono/metadata/class-internals.h"
#include "mono/metadata/debug-helpers.h"
#include "mono/metadata/metadata.h"
#include <llvm/Support/ErrorHandling.h>

#include <string>

namespace mono {

namespace {

/// The six types the CLI tracks on the evaluation stack (ECMA-335 III.1.5), and
/// a seventh for everything that cannot appear as a numeric operand.
enum StackType { Int32, Int64, NativeInt, Float, ManagedPtr, ObjectRef, Invalid };

constexpr size_t STACK_TYPE_COUNT = ObjectRef + 1;

constexpr uint16_t
bit (BinaryOp op)
{
	return 1u << static_cast<unsigned> (op);
}

constexpr uint16_t ADD = bit (BinaryOp::Add);
constexpr uint16_t DIV = bit (BinaryOp::Div);
constexpr uint16_t MUL = bit (BinaryOp::Mul);
constexpr uint16_t REM = bit (BinaryOp::Rem);
constexpr uint16_t SUB = bit (BinaryOp::Sub);
constexpr uint16_t NUMERIC = ADD | DIV | MUL | REM | SUB;

constexpr uint16_t DIV_UN = bit (BinaryOp::DivUn);
constexpr uint16_t REM_UN = bit (BinaryOp::RemUn);
constexpr uint16_t INTEGER_ALL = DIV_UN | REM_UN;

constexpr uint16_t ADD_OVF = bit (BinaryOp::AddOvf);
constexpr uint16_t ADD_OVF_UN = bit (BinaryOp::AddOvfUn);
constexpr uint16_t MUL_OVF = bit (BinaryOp::MulOvf);
constexpr uint16_t MUL_OVF_UN = bit (BinaryOp::MulOvfUn);
constexpr uint16_t SUB_OVF = bit (BinaryOp::SubOvf);
constexpr uint16_t SUB_OVF_UN = bit (BinaryOp::SubOvfUn);
constexpr uint16_t OVERFLOW_ALL =
	ADD_OVF | ADD_OVF_UN | MUL_OVF | MUL_OVF_UN | SUB_OVF | SUB_OVF_UN;

/// One cell of an operand table: what A op B leaves on the stack, and which of that
/// table's instructions the cell holds for. X is the table's invalid box.
struct Cell {
	StackType result = Invalid;
	uint16_t ops = 0;
};

using OperandTable = Cell[STACK_TYPE_COUNT][STACK_TYPE_COUNT];

constexpr Cell X = {};

constexpr Cell I4_ALL = { Int32, NUMERIC };
constexpr Cell I8_ALL = { Int64, NUMERIC };
constexpr Cell NI_ALL = { NativeInt, NUMERIC };
constexpr Cell F_ALL = { Float, NUMERIC };
constexpr Cell NI_SUB = { NativeInt, SUB };
constexpr Cell MP_ADD = { ManagedPtr, ADD };
constexpr Cell MP_ADD_SUB = { ManagedPtr, ADD | SUB };

/*
 * ECMA-335 III.1.5, Table III.2: Binary Numeric Operations. Indexed [A's type][B's
 * type], as are the two below.
 *
 * The managed-pointer cells are the ones the spec shades as unverifiable; the JIT is
 * not a verifier, so they are accepted like any other.
 */
constexpr OperandTable BINARY_NUMERIC = {
	/*              int32       int64    native int     F        &      O */
	/* int32 */ { I4_ALL,     X,        NI_ALL,     X,      MP_ADD, X },
	/* int64 */ { X,          I8_ALL,   X,          X,      X,      X },
	/* nint  */ { NI_ALL,     X,        NI_ALL,     X,      MP_ADD, X },
	/* F     */ { X,          X,        X,          F_ALL,  X,      X },
	/* &     */ { MP_ADD_SUB, X,        MP_ADD_SUB, X,      NI_SUB, X },
	/* O     */ { X,          X,        X,          X,      X,      X },
};

constexpr Cell I4_INT = { Int32, INTEGER_ALL };
constexpr Cell I8_INT = { Int64, INTEGER_ALL };
constexpr Cell NI_INT = { NativeInt, INTEGER_ALL };

/*
 * Table III.5: Integer Operations. Every box here is verifiable - neither float nor
 * anything the GC tracks has an unsigned division.
 */
constexpr OperandTable INTEGER = {
	/*            int32    int64   native int   F   &   O */
	/* int32 */ { I4_INT, X,      NI_INT,     X,  X,  X },
	/* int64 */ { X,      I8_INT, X,          X,  X,  X },
	/* nint  */ { NI_INT, X,      NI_INT,     X,  X,  X },
	/* F     */ { X,      X,      X,          X,  X,  X },
	/* &     */ { X,      X,      X,          X,  X,  X },
	/* O     */ { X,      X,      X,          X,  X,  X },
};

constexpr Cell I4_OVF = { Int32, OVERFLOW_ALL };
constexpr Cell I8_OVF = { Int64, OVERFLOW_ALL };
constexpr Cell NI_OVF = { NativeInt, OVERFLOW_ALL };
constexpr Cell NI_SUB_UN = { NativeInt, SUB_OVF_UN };
constexpr Cell MP_ADD_UN = { ManagedPtr, ADD_OVF_UN };
constexpr Cell MP_ADD_SUB_UN = { ManagedPtr, ADD_OVF_UN | SUB_OVF_UN };

/*
 * Table III.7: Overflow Arithmetic Operations. The same shape as Table III.2, except
 * that only the unsigned forms may touch a pointer.
 */
constexpr OperandTable OVERFLOW_ARITHMETIC = {
	/*                int32          int64   native int      F      &        O */
	/* int32 */ { I4_OVF,        X,      NI_OVF,        X, MP_ADD_UN, X },
	/* int64 */ { X,             I8_OVF, X,             X, X,         X },
	/* nint  */ { NI_OVF,        X,      NI_OVF,        X, MP_ADD_UN, X },
	/* F     */ { X,             X,      X,             X, X,         X },
	/* &     */ { MP_ADD_SUB_UN, X,      MP_ADD_SUB_UN, X, NI_SUB_UN, X },
	/* O     */ { X,             X,      X,             X, X,         X },
};

/// The operand table ECMA-335 gives for OP.
const OperandTable &
table_for (BinaryOp op)
{
	switch (op) {
	case BinaryOp::Add:
	case BinaryOp::Div:
	case BinaryOp::Mul:
	case BinaryOp::Rem:
	case BinaryOp::Sub:
		return BINARY_NUMERIC;
	case BinaryOp::DivUn:
	case BinaryOp::RemUn:
		return INTEGER;
	case BinaryOp::AddOvf:
	case BinaryOp::AddOvfUn:
	case BinaryOp::MulOvf:
	case BinaryOp::MulOvfUn:
	case BinaryOp::SubOvf:
	case BinaryOp::SubOvfUn:
		return OVERFLOW_ARITHMETIC;
	}

	llvm::report_fatal_error ("table_for: unknown binary operation");
}

/// How the CLI categorizes T on the evaluation stack.
StackType
stack_type (MonoType *t)
{
	if (t->byref)
		return ManagedPtr;

	t = mini_get_underlying_type (t);

	switch (t->type) {
	/* Anything narrower than four bytes is tracked as int32 once it is pushed. */
	case MONO_TYPE_BOOLEAN:
	case MONO_TYPE_CHAR:
	case MONO_TYPE_I1:
	case MONO_TYPE_U1:
	case MONO_TYPE_I2:
	case MONO_TYPE_U2:
	case MONO_TYPE_I4:
	case MONO_TYPE_U4:
		return Int32;
	case MONO_TYPE_I8:
	case MONO_TYPE_U8:
		return Int64;
	case MONO_TYPE_I:
	case MONO_TYPE_U:
	case MONO_TYPE_PTR:
	case MONO_TYPE_FNPTR:
		return NativeInt;
	case MONO_TYPE_R4:
	case MONO_TYPE_R8:
		return Float;
	case MONO_TYPE_STRING:
	case MONO_TYPE_CLASS:
	case MONO_TYPE_OBJECT:
	case MONO_TYPE_ARRAY:
	case MONO_TYPE_SZARRAY:
	/* Generic sharing hands us these as references. */
	case MONO_TYPE_VAR:
	case MONO_TYPE_MVAR:
		return ObjectRef;
	case MONO_TYPE_GENERICINST:
		return mono_type_generic_inst_is_valuetype (t) ? Invalid : ObjectRef;
	default:
		return Invalid;
	}
}

/// The CLI's name for T's category, or T's own name when it has none.
std::string
describe (MonoType *t, StackType type)
{
	switch (type) {
	case Int32:
		return "int32";
	case Int64:
		return "int64";
	case NativeInt:
		return "native int";
	case Float:
		return "float";
	case ManagedPtr:
		return "managed pointer";
	case ObjectRef:
		return "object reference";
	case Invalid:
		break;
	}

	char *name = mono_type_full_name (t);
	std::string text = name;

	g_free (name);
	return text;
}

const char *
op_name (BinaryOp op)
{
	switch (op) {
	case BinaryOp::Add:
		return "add";
	case BinaryOp::Div:
		return "div";
	case BinaryOp::Mul:
		return "mul";
	case BinaryOp::Rem:
		return "rem";
	case BinaryOp::Sub:
		return "sub";
	case BinaryOp::DivUn:
		return "div.un";
	case BinaryOp::RemUn:
		return "rem.un";
	case BinaryOp::AddOvf:
		return "add.ovf";
	case BinaryOp::AddOvfUn:
		return "add.ovf.un";
	case BinaryOp::MulOvf:
		return "mul.ovf";
	case BinaryOp::MulOvfUn:
		return "mul.ovf.un";
	case BinaryOp::SubOvf:
		return "sub.ovf";
	case BinaryOp::SubOvfUn:
		return "sub.ovf.un";
	}

	llvm::report_fatal_error ("op_name: unknown binary operation");
}

bool
is_r8 (MonoType *t)
{
	return mini_get_underlying_type (t)->type == MONO_TYPE_R8;
}

llvm::Type *
native_int_type (llvm::IRBuilder<> &builder)
{
	return builder.getIntNTy (TARGET_SIZEOF_VOID_P * 8);
}

/// VALUE as an operand of TYPE, widening it if the two operands of a binary numeric
/// operation did not arrive as the same thing.
///
/// Only ever a widening: Table III.2 never pairs an operand with a result narrower
/// than itself.
llvm::Value *
coerce (llvm::IRBuilder<> &builder, llvm::Value *value, llvm::Type *type)
{
	llvm::Type *from = value->getType ();

	if (from == type)
		return value;
	if (type->isFloatingPointTy ())
		return builder.CreateFPExt (value, type);
	/* An unmanaged pointer is tracked as native int but travels as a pointer. */
	if (from->isPointerTy ())
		return builder.CreatePtrToInt (value, type);
	/* int32 paired with native int is sign-extended, never zero-extended. */
	return builder.CreateSExt (value, type);
}

} // namespace

/// The type A op B leaves on the evaluation stack, per whichever of the ECMA-335
/// III.1.5 operand tables governs OP, or an InvalidProgramException for the
/// combinations that table rules out.
llvm::Expected<MonoType *>
MethodLLVMEmitter::binary_result (BinaryOp op, MonoType *lhs, MonoType *rhs)
{
	StackType a = stack_type (lhs);
	StackType b = stack_type (rhs);
	Cell cell = a == Invalid || b == Invalid ? Cell () : table_for (op)[a][b];

	if ((cell.ops & bit (op)) == 0)
		return invalid_il (llvm::Twine (op_name (op)) + " is not defined for operand types "
		                   + describe (lhs, a) + " and " + describe (rhs, b));

	switch (cell.result) {
	case Int32:
		return mono_get_int32_type ();
	case Int64:
		return m_class_get_byval_arg (mono_defaults.int64_class);
	case NativeInt:
		return mono_get_int_type ();
	case Float:
		/* The CLI tracks a single float type, so keep the wider of the two. */
		return is_r8 (lhs) || is_r8 (rhs)
		               ? m_class_get_byval_arg (mono_defaults.double_class)
		               : m_class_get_byval_arg (mono_defaults.single_class);
	case ManagedPtr:
		/* Pointer arithmetic keeps pointing at whatever the pointer operand did. */
		return a == ManagedPtr ? lhs : rhs;
	default:
		llvm::report_fatal_error ("binary_result: unreachable result type");
	}
}

/// Take OP's two operands off the evaluation stack, along with the type it will push
/// back, or fail the way Table III.2 says to if the stack cannot supply a valid pair.
llvm::Expected<MethodLLVMEmitter::BinaryOperands>
MethodLLVMEmitter::pop_binary_operands (BinaryOp op)
{
	if (stack.size () < 2)
		return unbalanced_stack (2);

	StackValue value1 = get_stack (1);
	StackValue value2 = get_stack (0);

	llvm::Expected<MonoType *> result = binary_result (op, value1.type, value2.type);
	if (!result)
		return result.takeError ();

	pop_stack (2);
	return BinaryOperands { value1, value2, *result };
}

/*
 * III.3.1  add - add numeric values
 *
 *   Format   Assembly Format   Description
 *   58       add               Add two values, returning a new value.
 *
 * Stack Transition:
 *
 *   ..., value1, value2 -> ..., result
 *
 * Description:
 *
 *   The add instruction adds value2 to value1 and pushes the result on the stack.
 *   Overflow is not detected for integral operations (but see add.ovf);
 *   floating-point overflow returns +inf or -inf. The acceptable operand types and
 *   their corresponding result data type are encapsulated in Table 2: Binary Numeric
 *   Operations.
 *
 * Exceptions:
 *
 *   None.
 *
 * Correctness and Verifiability:
 *
 *   See Table 2: Binary Numeric Operations.
 */
llvm::Error
MethodLLVMEmitter::emit_add (MonoIrBuilder &builder)
{
	llvm::Expected<BinaryOperands> operands = pop_binary_operands (BinaryOp::Add);
	if (!operands)
		return operands.takeError ();

	auto [value1, value2, result] = *operands;
	llvm::Value *sum;

	if (result->byref) {
		/*
		 * A managed pointer plus an integer stays a managed pointer, so index the
		 * pointer rather than doing the arithmetic on it - that keeps the result
		 * something the GC can still recognize as pointing into its object.
		 */
		bool value1_is_pointer = value1.type->byref;
		llvm::Value *base = value1_is_pointer ? value1.value : value2.value;
		llvm::Value *index = value1_is_pointer ? value2.value : value1.value;

		sum = builder.CreateGEP (builder.getInt8Ty (), base,
		                         coerce (builder, index, native_int_type (builder)));
	} else {
		llvm::Expected<llvm::Type *> type = convert_type (result);
		if (!type)
			return type.takeError ();

		llvm::Value *lhs = coerce (builder, value1.value, *type);
		llvm::Value *rhs = coerce (builder, value2.value, *type);

		sum = (*type)->isFloatingPointTy () ? builder.CreateFAdd (lhs, rhs)
		                                    : builder.CreateAdd (lhs, rhs);
	}

	push_stack (sum, result);
	return llvm::Error::success ();
}

/*
 * III.3.64  sub - subtract numeric values
 *
 *   Format   Assembly Format   Description
 *   59       sub               Subtract value2 from value1, returning a new value.
 *
 * Stack Transition:
 *
 *   ..., value1, value2 -> ..., result
 *
 * Description:
 *
 *   The sub instruction subtracts value2 from value1 and pushes the result on the
 *   stack. Overflow is not detected for the integral operations (see sub.ovf); for
 *   floating-point operands, sub returns +inf on positive overflow, -inf on negative
 *   overflow, and zero on floating-point underflow. The acceptable operand types and
 *   their corresponding result data type are encapsulated in Table III.2: Binary
 *   Numeric Operations.
 *
 * Exceptions:
 *
 *   None.
 *
 * Correctness and Verifiability:
 *
 *   See Table 2: Binary Numeric Operations.
 */
llvm::Error
MethodLLVMEmitter::emit_sub (MonoIrBuilder &builder)
{
	llvm::Expected<BinaryOperands> operands = pop_binary_operands (BinaryOp::Sub);
	if (!operands)
		return operands.takeError ();

	auto [value1, value2, result] = *operands;
	llvm::Value *difference;

	if (result->byref) {
		/*
		 * Only & - int gets a managed pointer back, so value1 is the pointer;
		 * int - & is not in the table. Index it the way add does, backwards.
		 */
		llvm::Value *index = coerce (builder, value2.value, native_int_type (builder));

		difference = builder.CreateGEP (builder.getInt8Ty (), value1.value,
		                                builder.CreateNeg (index));
	} else {
		llvm::Expected<llvm::Type *> type = convert_type (result);
		if (!type)
			return type.takeError ();

		/* & - & lands here: coerce turns both pointers into their addresses. */
		llvm::Value *lhs = coerce (builder, value1.value, *type);
		llvm::Value *rhs = coerce (builder, value2.value, *type);

		difference = (*type)->isFloatingPointTy () ? builder.CreateFSub (lhs, rhs)
		                                           : builder.CreateSub (lhs, rhs);
	}

	push_stack (difference, result);
	return llvm::Error::success ();
}

/*
 * III.3.48  mul - multiply values
 *
 *   Format   Assembly Format   Description
 *   5A       mul               Multiply values.
 *
 * Stack Transition:
 *
 *   ..., value1, value2 -> ..., result
 *
 * Description:
 *
 *   The mul instruction multiplies value1 by value2 and pushes the result on the
 *   stack. Integral operations silently truncate the upper bits on overflow (see
 *   mul.ovf). For floating-point types, 0 x infinity = NaN. The acceptable operand
 *   types and their corresponding result data types are encapsulated in Table III.2:
 *   Binary Numeric Operations.
 *
 * Exceptions:
 *
 *   None.
 *
 * Correctness and Verifiability:
 *
 *   See Table 2: Binary Numeric Operations.
 */
llvm::Error
MethodLLVMEmitter::emit_mul (MonoIrBuilder &builder)
{
	llvm::Expected<BinaryOperands> operands = pop_binary_operands (BinaryOp::Mul);
	if (!operands)
		return operands.takeError ();

	auto [value1, value2, result] = *operands;
	llvm::Expected<llvm::Type *> type = convert_type (result);
	if (!type)
		return type.takeError ();

	llvm::Value *lhs = coerce (builder, value1.value, *type);
	llvm::Value *rhs = coerce (builder, value2.value, *type);
	llvm::Value *product = (*type)->isFloatingPointTy () ? builder.CreateFMul (lhs, rhs)
	                                                     : builder.CreateMul (lhs, rhs);

	push_stack (product, result);
	return llvm::Error::success ();
}

/// Branch around the divisors an integer division has no answer for.
///
/// LLVM leaves those poison rather than trapping, so nothing downstream would raise
/// the exception the CIL spec asks for if they were left to the hardware.
void
MethodLLVMEmitter::emit_division_guards (MonoIrBuilder &builder, llvm::Value *lhs,
                                         llvm::Value *rhs, bool is_signed)
{
	llvm::Type *type = lhs->getType ();
	llvm::Value *zero = llvm::ConstantInt::get (type, 0);

	emit_cond_exception (builder, builder.CreateICmpEQ (rhs, zero), "DivideByZeroException");

	if (!is_signed)
		return;

	/* The one quotient with nowhere to go: -2^(n-1) / -1 is 2^(n-1). */
	llvm::Value *minus_one = llvm::ConstantInt::getSigned (type, -1);
	llvm::Value *smallest = llvm::ConstantInt::get (
		type, llvm::APInt::getSignedMinValue (type->getIntegerBitWidth ()));
	llvm::Value *overflow = builder.CreateAnd (builder.CreateICmpEQ (lhs, smallest),
	                                           builder.CreateICmpEQ (rhs, minus_one));

	emit_cond_exception (builder, overflow, "OverflowException");
}

/// LHS op RHS under the given `llvm.*.with.overflow` intrinsic, throwing
/// OverflowException if the answer did not fit.
llvm::Value *
MethodLLVMEmitter::emit_checked (MonoIrBuilder &builder, llvm::Intrinsic::ID intrinsic,
                                 llvm::Value *lhs, llvm::Value *rhs)
{
	llvm::Value *checked = builder.CreateBinaryIntrinsic (intrinsic, lhs, rhs);
	llvm::Value *value = builder.CreateExtractValue (checked, 0);

	emit_cond_exception (builder, builder.CreateExtractValue (checked, 1),
	                     "OverflowException");
	return value;
}

/// BASE moved INDEX bytes, forwards or backwards, with the address arithmetic
/// checked for wraparound.
///
/// The check runs on the address, but the result comes back out of a GEP: handing the
/// intrinsic's integer to inttoptr would leave the GC a pointer it can no longer tie
/// to the object it points into. Only the unsigned forms have a pointer cell in
/// Table III.7, so the check is unsigned too.
llvm::Value *
MethodLLVMEmitter::emit_checked_pointer_offset (MonoIrBuilder &builder, llvm::Value *base,
                                                llvm::Value *index, bool subtract)
{
	llvm::Value *address = builder.CreatePtrToInt (base, native_int_type (builder));

	emit_checked (builder,
	              subtract ? llvm::Intrinsic::usub_with_overflow
	                       : llvm::Intrinsic::uadd_with_overflow,
	              address, index);

	return builder.CreateGEP (builder.getInt8Ty (), base,
	                          subtract ? builder.CreateNeg (index) : index);
}

/*
 * III.3.31  div - divide values
 *
 *   Format   Assembly Format   Description
 *   5B       div               Divide two values to return a quotient or
 *                              floating-point result.
 *
 * Stack Transition:
 *
 *   ..., value1, value2 -> ..., result
 *
 * Description:
 *
 *   result = value1 div value2 satisfies the following conditions:
 *
 *     |result| = |value1| / |value2|, and
 *     sign(result) = +, if sign(value1) = sign(value2), or
 *                    -, if sign(value1) ~= sign(value2)
 *
 *   The div instruction computes result and pushes it on the stack.
 *
 *   Integer division truncates towards zero.
 *
 *   Floating-point division is per IEC 60559:1989. In particular, division of a
 *   finite number by 0 produces the correctly signed infinite value and
 *
 *     0 / 0 = NaN
 *     infinity / infinity = NaN.
 *     X / infinity = 0
 *
 *   The acceptable operand types and their corresponding result data type are
 *   encapsulated in Table 2: Binary Numeric Operations.
 *
 * Exceptions:
 *
 *   Integral operations throw System.ArithmeticException if the result cannot be
 *   represented in the result type. (This can happen if value1 is the smallest
 *   representable integer value, and value2 is -1.)
 *
 *   Integral operations throw DivideByZeroException if value2 is zero.
 *
 *   Floating-point operations never throw an exception (they produce NaNs or
 *   infinities instead, see Partition I).
 *
 * Example:
 *
 *   +14 div +3  is 4
 *   +14 div -3  is -4
 *   -14 div +3  is -4
 *   -14 div -3  is 4
 *
 * Correctness and Verifiability:
 *
 *   See Table 2: Binary Numeric Operations.
 */
llvm::Error
MethodLLVMEmitter::emit_div (MonoIrBuilder &builder)
{
	llvm::Expected<BinaryOperands> operands = pop_binary_operands (BinaryOp::Div);
	if (!operands)
		return operands.takeError ();

	auto [value1, value2, result] = *operands;
	llvm::Expected<llvm::Type *> type = convert_type (result);
	if (!type)
		return type.takeError ();

	llvm::Value *lhs = coerce (builder, value1.value, *type);
	llvm::Value *rhs = coerce (builder, value2.value, *type);
	llvm::Value *quotient;

	if ((*type)->isFloatingPointTy ()) {
		quotient = builder.CreateFDiv (lhs, rhs);
	} else {
		/* sdiv, not udiv: div.un is a separate instruction with its own table. */
		emit_division_guards (builder, lhs, rhs, true);
		quotient = builder.CreateSDiv (lhs, rhs);
	}

	push_stack (quotient, result);
	return llvm::Error::success ();
}

/*
 * III.3.55  rem - compute remainder
 *
 *   Format   Assembly Format   Description
 *   5D       rem               Remainder when dividing one value by another.
 *
 * Stack Transition:
 *
 *   ..., value1, value2 -> ..., result
 *
 * Description:
 *
 *   The rem instruction divides value1 by value2 and pushes the remainder result on
 *   the stack. The acceptable operand types and their corresponding result data type
 *   are encapsulated in Table 2: Binary Numeric Operations.
 *
 *   For integer operands
 *
 *     result = value1 rem value2 satisfies the following conditions:
 *
 *       result = value1 - value2 x (value1 div value2), and
 *       0 <= |result| < |value2|, and
 *       sign(result) = sign(value1),
 *
 *     where div is the division instruction, which truncates towards zero.
 *
 *   For floating-point operands
 *
 *     rem is defined similarly as for integer operands, except that, if value2 is
 *     zero or value1 is infinity, result is NaN. If value2 is infinity, result is
 *     value1. This definition is different from the one for floating-point remainder
 *     in the IEC 60559:1989 Standard. That Standard specifies that value1 div value2
 *     is the nearest integer instead of truncating towards zero.
 *     System.Math.IEEERemainder (see Partition IV) provides the IEC 60559:1989
 *     behavior.
 *
 * Exceptions:
 *
 *   Integral operations throw System.DivideByZeroException if value2 is zero.
 *
 *   Integral operations can throw System.ArithmeticException if value1 is the
 *   smallest representable integer value and value2 is -1.
 *
 * Example:
 *
 *   +10 rem +6  is 4  (+10 div +6 = 1)
 *   +10 rem -6  is 4  (+10 div -6 = -1)
 *   -10 rem +6  is -4 (-10 div +6 = -1)
 *   -10 rem -6  is -4 (-10 div -6 = 1)
 *
 * Correctness and Verifiability:
 *
 *   See Table 2: Binary Numeric Operations.
 */
llvm::Error
MethodLLVMEmitter::emit_rem (MonoIrBuilder &builder)
{
	llvm::Expected<BinaryOperands> operands = pop_binary_operands (BinaryOp::Rem);
	if (!operands)
		return operands.takeError ();

	auto [value1, value2, result] = *operands;
	llvm::Expected<llvm::Type *> type = convert_type (result);
	if (!type)
		return type.takeError ();

	llvm::Value *lhs = coerce (builder, value1.value, *type);
	llvm::Value *rhs = coerce (builder, value2.value, *type);
	llvm::Value *remainder;

	if ((*type)->isFloatingPointTy ()) {
		/* frem is fmod, which truncates towards zero - not IEEERemainder. */
		remainder = builder.CreateFRem (lhs, rhs);
	} else {
		emit_division_guards (builder, lhs, rhs, true);
		remainder = builder.CreateSRem (lhs, rhs);
	}

	push_stack (remainder, result);
	return llvm::Error::success ();
}

/*
 * III.3.32  div.un - divide integer values, unsigned
 *
 *   Format   Assembly Format   Description
 *   5C       div.un            Divide two values, unsigned, returning a quotient.
 *
 * Stack Transition:
 *
 *   ..., value1, value2 -> ..., result
 *
 * Description:
 *
 *   The div.un instruction computes value1 divided by value2, both taken as unsigned
 *   integers, and pushes the result on the stack. The acceptable operand types and
 *   their corresponding result data type are encapsulated in Table 5: Integer
 *   Operations.
 *
 * Exceptions:
 *
 *   System.DivideByZeroException is thrown if value2 is zero.
 *
 * Example:
 *
 *   +5 div.un +3  is 1
 *   +5 div.un -3  is 0
 *   -5 div.un +3  is 1431655763 or 0x55555553
 *   -5 div.un -3  is 0
 *
 * Correctness and Verifiability:
 *
 *   See Table 5: Integer Operations.
 */
llvm::Error
MethodLLVMEmitter::emit_div_un (MonoIrBuilder &builder)
{
	llvm::Expected<BinaryOperands> operands = pop_binary_operands (BinaryOp::DivUn);
	if (!operands)
		return operands.takeError ();

	auto [value1, value2, result] = *operands;
	llvm::Expected<llvm::Type *> type = convert_type (result);
	if (!type)
		return type.takeError ();

	llvm::Value *lhs = coerce (builder, value1.value, *type);
	llvm::Value *rhs = coerce (builder, value2.value, *type);

	/* No overflow case to guard: only signed division has a quotient that cannot fit. */
	emit_division_guards (builder, lhs, rhs, false);

	push_stack (builder.CreateUDiv (lhs, rhs), result);
	return llvm::Error::success ();
}

/*
 * III.3.56  rem.un - compute integer remainder, unsigned
 *
 *   Format   Assembly Format   Description
 *   5E       rem.un            Remainder when dividing one unsigned value by another.
 *
 * Stack Transition:
 *
 *   ..., value1, value2 -> ..., result
 *
 * Description:
 *
 *   The rem.un instruction divides value1 by value2 and pushes the remainder result
 *   on the stack. (rem.un treats its arguments as unsigned integers, while rem treats
 *   them as signed integers.)
 *
 *   result = value1 rem.un value2 satisfies the following conditions:
 *
 *     result = value1 - value2 x (value1 div.un value2), and
 *     0 <= result < value2,
 *
 *   where div.un is the unsigned division instruction. rem.un is unspecified for
 *   floating-point numbers. The acceptable operand types and their corresponding
 *   result data type are encapsulated in Table 5: Integer Operations.
 *
 * Exceptions:
 *
 *   Integral operations throw System.DivideByZeroException if value2 is zero.
 *
 * Example:
 *
 *   +5 rem.un +3  is 2  (+5 div.un +3 = 1)
 *   +5 rem.un -3  is 5  (+5 div.un -3 = 0)
 *   -5 rem.un +3  is 2  (-5 div.un +3 = 1431655763 or 0x55555553)
 *   -5 rem.un -3  is -5 or 0xfffffffb  (-5 div.un -3 = 0)
 *
 * Correctness and Verifiability:
 *
 *   See Table 5: Integer Operations.
 */
llvm::Error
MethodLLVMEmitter::emit_rem_un (MonoIrBuilder &builder)
{
	llvm::Expected<BinaryOperands> operands = pop_binary_operands (BinaryOp::RemUn);
	if (!operands)
		return operands.takeError ();

	auto [value1, value2, result] = *operands;
	llvm::Expected<llvm::Type *> type = convert_type (result);
	if (!type)
		return type.takeError ();

	llvm::Value *lhs = coerce (builder, value1.value, *type);
	llvm::Value *rhs = coerce (builder, value2.value, *type);

	emit_division_guards (builder, lhs, rhs, false);

	push_stack (builder.CreateURem (lhs, rhs), result);
	return llvm::Error::success ();
}

/*
 * III.3.2  add.ovf.<signed> - add integer values with overflow check
 *
 *   Format   Assembly Format   Description
 *   D6       add.ovf           Add signed integer values with overflow check.
 *   D7       add.ovf.un        Add unsigned integer values with overflow check.
 *
 * Stack Transition:
 *
 *   ..., value1, value2 -> ..., result
 *
 * Description:
 *
 *   The add.ovf instruction adds value1 and value2 and pushes the result on the
 *   stack. The acceptable operand types and their corresponding result data type are
 *   encapsulated in Table 7: Overflow Arithmetic Operations.
 *
 * Exceptions:
 *
 *   System.OverflowException is thrown if the result cannot be represented in the
 *   result type.
 *
 * Correctness and Verifiability:
 *
 *   See Table 7: Overflow Arithmetic Operations.
 */
llvm::Error
MethodLLVMEmitter::emit_add_ovf (MonoIrBuilder &builder, bool is_unsigned)
{
	BinaryOp op = is_unsigned ? BinaryOp::AddOvfUn : BinaryOp::AddOvf;
	llvm::Expected<BinaryOperands> operands = pop_binary_operands (op);
	if (!operands)
		return operands.takeError ();

	auto [value1, value2, result] = *operands;
	llvm::Value *sum;

	if (result->byref) {
		bool value1_is_pointer = value1.type->byref;
		llvm::Value *base = value1_is_pointer ? value1.value : value2.value;
		llvm::Value *index = value1_is_pointer ? value2.value : value1.value;

		sum = emit_checked_pointer_offset (
			builder, base, coerce (builder, index, native_int_type (builder)), false);
	} else {
		llvm::Expected<llvm::Type *> type = convert_type (result);
		if (!type)
			return type.takeError ();

		sum = emit_checked (builder,
		                    is_unsigned ? llvm::Intrinsic::uadd_with_overflow
		                                : llvm::Intrinsic::sadd_with_overflow,
		                    coerce (builder, value1.value, *type),
		                    coerce (builder, value2.value, *type));
	}

	push_stack (sum, result);
	return llvm::Error::success ();
}

/*
 * III.3.65  sub.ovf.<type> - subtract integer values, checking for overflow
 *
 *   Format   Assembly Format   Description
 *   DA       sub.ovf           Subtract native int from a native int. Signed result
 *                              shall fit in same size.
 *   DB       sub.ovf.un        Subtract native unsigned int from a native unsigned
 *                              int. Unsigned result shall fit in same size.
 *
 * Stack Transition:
 *
 *   ..., value1, value2 -> ..., result
 *
 * Description:
 *
 *   The sub.ovf instruction subtracts value2 from value1 and pushes the result on the
 *   stack. The type of the values and the return type are specified by the
 *   instruction. An exception is thrown if the result does not fit in the result type.
 *   The acceptable operand types and their corresponding result data type is
 *   encapsulated in Table 7: Overflow Arithmetic Operations.
 *
 * Exceptions:
 *
 *   System.OverflowException is thrown if the result can not be represented in the
 *   result type.
 *
 * Correctness and Verifiability:
 *
 *   See Table 7: Overflow Arithmetic Operations.
 */
llvm::Error
MethodLLVMEmitter::emit_sub_ovf (MonoIrBuilder &builder, bool is_unsigned)
{
	BinaryOp op = is_unsigned ? BinaryOp::SubOvfUn : BinaryOp::SubOvf;
	llvm::Expected<BinaryOperands> operands = pop_binary_operands (op);
	if (!operands)
		return operands.takeError ();

	auto [value1, value2, result] = *operands;
	llvm::Value *difference;

	if (result->byref) {
		/* As with sub, only & - int gets a pointer back, so value1 is the pointer. */
		difference = emit_checked_pointer_offset (
			builder, value1.value,
			coerce (builder, value2.value, native_int_type (builder)), true);
	} else {
		llvm::Expected<llvm::Type *> type = convert_type (result);
		if (!type)
			return type.takeError ();

		/* & - & lands here: coerce turns both pointers into their addresses. */
		difference = emit_checked (builder,
		                           is_unsigned ? llvm::Intrinsic::usub_with_overflow
		                                       : llvm::Intrinsic::ssub_with_overflow,
		                           coerce (builder, value1.value, *type),
		                           coerce (builder, value2.value, *type));
	}

	push_stack (difference, result);
	return llvm::Error::success ();
}

/*
 * III.3.49  mul.ovf.<type> - multiply integer values with overflow check
 *
 *   Format   Assembly Format   Description
 *   D8       mul.ovf           Multiply signed integer values. Signed result shall
 *                              fit in same size.
 *   D9       mul.ovf.un        Multiply unsigned integer values. Unsigned result
 *                              shall fit in same size.
 *
 * Stack Transition:
 *
 *   ..., value1, value2 -> ..., result
 *
 * Description:
 *
 *   The mul.ovf instruction multiplies integers, value1 and value2, and pushes the
 *   result on the stack. An exception is thrown if the result will not fit in the
 *   result type. The acceptable operand types and their corresponding result data
 *   types are encapsulated in Table 7: Overflow Arithmetic Operations.
 *
 * Exceptions:
 *
 *   System.OverflowException is thrown if the result can not be represented in the
 *   result type.
 *
 * Correctness and Verifiability:
 *
 *   See Table 8: Conversion Operations.
 */
llvm::Error
MethodLLVMEmitter::emit_mul_ovf (MonoIrBuilder &builder, bool is_unsigned)
{
	BinaryOp op = is_unsigned ? BinaryOp::MulOvfUn : BinaryOp::MulOvf;
	llvm::Expected<BinaryOperands> operands = pop_binary_operands (op);
	if (!operands)
		return operands.takeError ();

	/* Table III.7 gives mul.ovf no pointer cell, so there is only the scalar case. */
	auto [value1, value2, result] = *operands;
	llvm::Expected<llvm::Type *> type = convert_type (result);
	if (!type)
		return type.takeError ();

	llvm::Value *product = emit_checked (builder,
	                                     is_unsigned ? llvm::Intrinsic::umul_with_overflow
	                                                 : llvm::Intrinsic::smul_with_overflow,
	                                     coerce (builder, value1.value, *type),
	                                     coerce (builder, value2.value, *type));

	push_stack (product, result);
	return llvm::Error::success ();
}

} // namespace mono

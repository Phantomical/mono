#include "method-to-llvm.hpp"
#include "mono/metadata/class-inlines.h"
#include "mono/metadata/class-internals.h"
#include "mono/metadata/debug-helpers.h"
#include "mono/metadata/metadata.h"
#include <llvm/IR/Constants.h>
#include <llvm/IR/Intrinsics.h>
#include <llvm/Support/ErrorHandling.h>

#include <string>

namespace mono {

namespace {

constexpr uint32_t
bit (BinaryOp op)
{
	return 1u << static_cast<unsigned> (op);
}

constexpr uint32_t ADD = bit (BinaryOp::Add);
constexpr uint32_t DIV = bit (BinaryOp::Div);
constexpr uint32_t MUL = bit (BinaryOp::Mul);
constexpr uint32_t REM = bit (BinaryOp::Rem);
constexpr uint32_t SUB = bit (BinaryOp::Sub);
constexpr uint32_t NUMERIC = ADD | DIV | MUL | REM | SUB;

constexpr uint32_t DIV_UN = bit (BinaryOp::DivUn);
constexpr uint32_t REM_UN = bit (BinaryOp::RemUn);
constexpr uint32_t AND = bit (BinaryOp::And);
constexpr uint32_t OR = bit (BinaryOp::Or);
constexpr uint32_t XOR = bit (BinaryOp::Xor);
constexpr uint32_t INTEGER_ALL = DIV_UN | REM_UN | AND | OR | XOR;

constexpr uint32_t SHL = bit (BinaryOp::Shl);
constexpr uint32_t SHR = bit (BinaryOp::Shr);
constexpr uint32_t SHR_UN = bit (BinaryOp::ShrUn);
constexpr uint32_t SHIFT_ALL = SHL | SHR | SHR_UN;

constexpr uint32_t BEQ = bit (BinaryOp::Beq);
constexpr uint32_t BGE = bit (BinaryOp::Bge);
constexpr uint32_t BGT = bit (BinaryOp::Bgt);
constexpr uint32_t BLE = bit (BinaryOp::Ble);
constexpr uint32_t BLT = bit (BinaryOp::Blt);
constexpr uint32_t BNE_UN = bit (BinaryOp::BneUn);
constexpr uint32_t BGE_UN = bit (BinaryOp::BgeUn);
constexpr uint32_t BGT_UN = bit (BinaryOp::BgtUn);
constexpr uint32_t BLE_UN = bit (BinaryOp::BleUn);
constexpr uint32_t BLT_UN = bit (BinaryOp::BltUn);
constexpr uint32_t COMPARE_ALL =
	BEQ | BGE | BGT | BLE | BLT | BNE_UN | BGE_UN | BGT_UN | BLE_UN | BLT_UN;
// The cells the table spells out by name rather than accepting wholesale.
constexpr uint32_t COMPARE_EQ = BEQ | BNE_UN;

constexpr uint32_t ADD_OVF = bit (BinaryOp::AddOvf);
constexpr uint32_t ADD_OVF_UN = bit (BinaryOp::AddOvfUn);
constexpr uint32_t MUL_OVF = bit (BinaryOp::MulOvf);
constexpr uint32_t MUL_OVF_UN = bit (BinaryOp::MulOvfUn);
constexpr uint32_t SUB_OVF = bit (BinaryOp::SubOvf);
constexpr uint32_t SUB_OVF_UN = bit (BinaryOp::SubOvfUn);
constexpr uint32_t OVERFLOW_ALL =
	ADD_OVF | ADD_OVF_UN | MUL_OVF | MUL_OVF_UN | SUB_OVF | SUB_OVF_UN;

/// One cell of an operand table: what a op b leaves on the stack, and which of that
/// table's instructions the cell holds for. X is the table's invalid box.
///
/// A pairing can push different things for different instructions, so a cell can name a
/// second result that covers a subset of its own ops. For example, add on a pointer and
/// a number gives a pointer, but sub on two pointers gives the distance between them.
struct Cell {
	StackType result = Invalid;
	uint32_t ops = 0;
	uint32_t alt_ops = 0;
	StackType alt_result = Invalid;
};

using OperandTable = Cell[STACK_TYPE_COUNT][STACK_TYPE_COUNT];

constexpr Cell X = {};

constexpr Cell I4_ALL = {Int32, NUMERIC};
constexpr Cell I8_ALL = {Int64, NUMERIC};
constexpr Cell NI_ALL = {NativeInt, NUMERIC};
constexpr Cell F_ALL = {Float, NUMERIC};
constexpr Cell NI_SUB = {NativeInt, SUB};
constexpr Cell MP_ADD_NI_SUB = {ManagedPtr, ADD | SUB, SUB, NativeInt};
constexpr Cell MP_ADD_SUB = {ManagedPtr, ADD | SUB};

/*
 * ECMA-335 III.1.5, Table III.2: Binary Numeric Operations. Indexed [a's type][b's
 * type], as are the two tables below.
 *
 * The spec shades the managed-pointer cells as unverifiable. This JIT is not a
 * verifier, so it accepts them like any other cell.
 *
 * The int64 column and row carry cells the spec leaves empty. This table accepts
 * int64 mixed with int32 or native int, and so do the integer table and the overflow
 * table below. coerce () sign-extends the narrower operand into the full register.
 *
 * The spec also has no cell for `number - &`. A C# compiler reaches it whenever a
 * pointer difference has a `fixed` local on the right. That local is a managed
 * pointer, and the other operand is a plain unmanaged one. Every C# compiler emits
 * this pattern, so a JIT that refuses it refuses ordinary code. The distance between
 * two addresses is a number, not an address, so the result type is native int.
 */
constexpr OperandTable BINARY_NUMERIC = {
	/*              int32       int64    native int     F        &      O */
	/* int32 */ {I4_ALL, I8_ALL, NI_ALL, X, MP_ADD_NI_SUB, X},
	/* int64 */ {I8_ALL, I8_ALL, NI_ALL, X, X, X},
	/* nint  */ {NI_ALL, NI_ALL, NI_ALL, X, MP_ADD_NI_SUB, X},
	/* F     */ {X, X, X, F_ALL, X, X},
	/* &     */ {MP_ADD_SUB, X, MP_ADD_SUB, X, NI_SUB, X},
	/* O     */ {X, X, X, X, X, X},
};

constexpr Cell I4_INT = {Int32, INTEGER_ALL};
constexpr Cell I8_INT = {Int64, INTEGER_ALL};
constexpr Cell NI_INT = {NativeInt, INTEGER_ALL};

// Table III.5: Integer Operations. Every box here is verifiable - neither float nor
// anything the GC tracks has a bitwise operation or an unsigned division.
constexpr OperandTable INTEGER = {
	/*            int32    int64   native int   F   &   O */
	/* int32 */ {I4_INT, I8_INT, NI_INT, X, X, X},
	/* int64 */ {I8_INT, I8_INT, NI_INT, X, X, X},
	/* nint  */ {NI_INT, NI_INT, NI_INT, X, X, X},
	/* F     */ {X, X, X, X, X, X},
	/* &     */ {X, X, X, X, X, X},
	/* O     */ {X, X, X, X, X, X},
};

constexpr Cell I4_SHIFT = {Int32, SHIFT_ALL};
constexpr Cell I8_SHIFT = {Int64, SHIFT_ALL};
constexpr Cell NI_SHIFT = {NativeInt, SHIFT_ALL};

// Table III.6: Shift Operations, indexed [value to be shifted][shift amount].
//
// A shift does not change what it shifts. The result type follows only the left
// operand, so every cell in a row carries that row's own type. This table refuses an
// int64 shift amount.
constexpr OperandTable SHIFT = {
	/*             int32     int64  native int   F   &   O */
	/* int32 */ {I4_SHIFT, X, I4_SHIFT, X, X, X},
	/* int64 */ {I8_SHIFT, X, I8_SHIFT, X, X, X},
	/* nint  */ {NI_SHIFT, X, NI_SHIFT, X, X, X},
	/* F     */ {X, X, X, X, X, X},
	/* &     */ {X, X, X, X, X, X},
	/* O     */ {X, X, X, X, X, X},
};

constexpr Cell I4_CMP = {Int32, COMPARE_ALL};
constexpr Cell I8_CMP = {Int64, COMPARE_ALL};
constexpr Cell NI_CMP = {NativeInt, COMPARE_ALL};
constexpr Cell F_CMP = {Float, COMPARE_ALL};
constexpr Cell MP_CMP = {ManagedPtr, COMPARE_ALL};
constexpr Cell O_EQ = {ObjectRef, COMPARE_EQ};

/*
 * Table III.4: Binary Comparison or Branch Operations.
 *
 * The object-reference cell names beq, bne.un, and ceq specifically, instead of
 * accepting every predicate in the table. The spec restricts it that way because
 * comparing two object references only makes sense as an equality check.
 *
 * & against & takes every predicate in the row. The spec's footnote limits anything
 * other than equality to two pointers into the same array, a rule neither the CLI nor
 * this JIT checks.
 *
 * int64 against native int is accepted, the same 64-bit leniency as the tables above.
 * This table refuses int64 against int32, even for comparisons.
 *
 * Native int against & also takes every predicate in the row, not just the spec's beq
 * and bne.un. The reason matches `number - &` in Table III.2 above. Order comparisons
 * on a `fixed` local against a plain pointer are ordinary C#, and the two operands are
 * the same address either way.
 */
constexpr OperandTable COMPARISON = {
	/*            int32   int64   native int   F      &       O */
	/* int32 */ {I4_CMP, X, NI_CMP, X, X, X},
	/* int64 */ {X, I8_CMP, NI_CMP, X, X, X},
	/* nint  */ {NI_CMP, NI_CMP, NI_CMP, X, NI_CMP, X},
	/* F     */ {X, X, X, F_CMP, X, X},
	/* &     */ {X, X, NI_CMP, X, MP_CMP, X},
	/* O     */ {X, X, X, X, X, O_EQ},
};

constexpr Cell I4_OVF = {Int32, OVERFLOW_ALL};
constexpr Cell I8_OVF = {Int64, OVERFLOW_ALL};
constexpr Cell NI_OVF = {NativeInt, OVERFLOW_ALL};
constexpr Cell NI_SUB_UN = {NativeInt, SUB_OVF_UN};
constexpr Cell MP_ADD_NI_SUB_UN = {ManagedPtr, ADD_OVF_UN | SUB_OVF_UN, SUB_OVF_UN, NativeInt};
constexpr Cell MP_ADD_SUB_UN = {ManagedPtr, ADD_OVF_UN | SUB_OVF_UN};

// Table III.7: Overflow Arithmetic Operations. The same shape as Table III.2, except
// that only the unsigned forms can touch a pointer - `number - &` included.
constexpr OperandTable OVERFLOW_ARITHMETIC = {
	/*                int32          int64   native int      F      &        O */
	/* int32 */ {I4_OVF, I8_OVF, NI_OVF, X, MP_ADD_NI_SUB_UN, X},
	/* int64 */ {I8_OVF, I8_OVF, NI_OVF, X, X, X},
	/* nint  */ {NI_OVF, NI_OVF, NI_OVF, X, MP_ADD_NI_SUB_UN, X},
	/* F     */ {X, X, X, X, X, X},
	/* &     */ {MP_ADD_SUB_UN, X, MP_ADD_SUB_UN, X, NI_SUB_UN, X},
	/* O     */ {X, X, X, X, X, X},
};

/// The operand table ECMA-335 gives for op.
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
	case BinaryOp::And:
	case BinaryOp::Or:
	case BinaryOp::Xor:
		return INTEGER;
	case BinaryOp::Shl:
	case BinaryOp::Shr:
	case BinaryOp::ShrUn:
		return SHIFT;
	case BinaryOp::Beq:
	case BinaryOp::Bge:
	case BinaryOp::Bgt:
	case BinaryOp::Ble:
	case BinaryOp::Blt:
	case BinaryOp::BneUn:
	case BinaryOp::BgeUn:
	case BinaryOp::BgtUn:
	case BinaryOp::BleUn:
	case BinaryOp::BltUn:
		return COMPARISON;
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
	case BinaryOp::And:
		return "and";
	case BinaryOp::Or:
		return "or";
	case BinaryOp::Xor:
		return "xor";
	case BinaryOp::Shl:
		return "shl";
	case BinaryOp::Shr:
		return "shr";
	case BinaryOp::ShrUn:
		return "shr.un";
	case BinaryOp::Beq:
		return "beq";
	case BinaryOp::Bge:
		return "bge";
	case BinaryOp::Bgt:
		return "bgt";
	case BinaryOp::Ble:
		return "ble";
	case BinaryOp::Blt:
		return "blt";
	case BinaryOp::BneUn:
		return "bne.un";
	case BinaryOp::BgeUn:
		return "bge.un";
	case BinaryOp::BgtUn:
		return "bgt.un";
	case BinaryOp::BleUn:
		return "ble.un";
	case BinaryOp::BltUn:
		return "blt.un";
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

} // namespace

/// The type a op b leaves on the evaluation stack, per whichever of the ECMA-335
/// III.1.5 operand tables governs op, or an InvalidProgramException for the
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

	switch ((cell.alt_ops & bit (op)) != 0 ? cell.alt_result : cell.result) {
	case Int32:
		return mono_get_int32_type ();
	case Int64:
		return m_class_get_byval_arg (mono_defaults.int64_class);
	case NativeInt:
		return mono_get_int_type ();
	case Float:
		// The CLI tracks a single float type, so keep the wider of the two.
		return is_r8 (lhs) || is_r8 (rhs)
		               ? m_class_get_byval_arg (mono_defaults.double_class)
		               : m_class_get_byval_arg (mono_defaults.single_class);
	case ManagedPtr:
		// Pointer arithmetic keeps pointing at whatever the pointer operand did.
		return a == ManagedPtr ? lhs : rhs;
	case ObjectRef:
		// Only Table III.4 has these, where the two are compared as addresses.
		return lhs;
	default:
		llvm::report_fatal_error ("binary_result: unreachable result type");
	}
}

/// Take op's two operands off the evaluation stack, with the result type from
/// whichever ECMA-335 III.1.5 table governs op. Fail if the stack cannot supply a
/// valid pair.
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
	return BinaryOperands{value1, value2, *result};
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
		// A managed pointer plus an integer stays a managed pointer, so this indexes
		// the pointer instead of doing the arithmetic on it. That keeps the result
		// something the GC can still recognize as pointing into its object.
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
		// When the result is a managed pointer, value1 is always that pointer. Only
		// `& - int` produces one, per the table above. `int - &` produces a number
		// instead. This indexes the pointer the same way add does, backwards.
		llvm::Value *index = coerce (builder, value2.value, native_int_type (builder));

		difference = builder.CreateGEP (builder.getInt8Ty (), value1.value,
		                                builder.CreateNeg (index));
	} else {
		llvm::Expected<llvm::Type *> type = convert_type (result);
		if (!type)
			return type.takeError ();

		// `& - &` and `int - &` land here: coerce turns a pointer into its address.
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

/// Branch around the divisor values an integer division has no answer for. Zero always
/// applies. When is_signed is true, the smallest negative number divided by -1 applies
/// too.
///
/// LLVM's own division instructions treat those divisors as undefined behavior, not a
/// defined trap. This function raises the exception ECMA-335 requires before the
/// division runs.
void
MethodLLVMEmitter::emit_division_guards (MonoIrBuilder &builder, llvm::Value *lhs, llvm::Value *rhs,
                                         bool is_signed)
{
	llvm::Type *type = lhs->getType ();
	llvm::Value *zero = llvm::ConstantInt::get (type, 0);

	emit_cond_exception (builder, builder.CreateICmpEQ (rhs, zero), "DivideByZeroException");

	if (!is_signed)
		return;

	// The division -2^(n-1) / -1 has no valid quotient: the true answer, 2^(n-1), is
	// one more than the type can hold.
	llvm::Value *minus_one = llvm::ConstantInt::getSigned (type, -1);
	llvm::Value *smallest = llvm::ConstantInt::get (
		type, llvm::APInt::getSignedMinValue (type->getIntegerBitWidth ()));
	llvm::Value *overflow = builder.CreateAnd (builder.CreateICmpEQ (lhs, smallest),
	                                           builder.CreateICmpEQ (rhs, minus_one));

	emit_cond_exception (builder, overflow, "OverflowException");
}

/// lhs op rhs under the given `llvm.*.with.overflow` intrinsic. If the result does not
/// fit, this throws OverflowException.
llvm::Value *
MethodLLVMEmitter::emit_checked (MonoIrBuilder &builder, llvm::Intrinsic::ID intrinsic,
                                 llvm::Value *lhs, llvm::Value *rhs)
{
	llvm::Value *checked = builder.CreateBinaryIntrinsic (intrinsic, lhs, rhs);
	llvm::Value *value = builder.CreateExtractValue (checked, 0);

	emit_cond_exception (builder, builder.CreateExtractValue (checked, 1), "OverflowException");
	return value;
}

/// base moved index bytes, forwards or backwards, with the address arithmetic checked
/// for wraparound.
///
/// The check runs on the address, but the result comes from a GEP, not from the
/// checked integer. If code passed that integer to inttoptr, the GC cannot tie the
/// pointer back to the object it points into. Only the unsigned forms have a pointer
/// cell in Table III.7, so the check is unsigned too.
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
		// sdiv, not udiv: div.un is a separate instruction with its own table.
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
		// frem is fmod, which truncates toward zero - not IEEERemainder.
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

	// No overflow case to guard: only signed division has a quotient that cannot fit.
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
		// As with sub, only `& - int` gives back a pointer, so value1 is the pointer.
		difference = emit_checked_pointer_offset (
			builder, value1.value,
			coerce (builder, value2.value, native_int_type (builder)), true);
	} else {
		llvm::Expected<llvm::Type *> type = convert_type (result);
		if (!type)
			return type.takeError ();

		// `& - &` and `int - &` land here: coerce turns a pointer into its address.
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

	// Table III.7 gives mul.ovf no pointer cell, so there is only the scalar case.
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

/*
 * III.3.50  neg - negate
 *
 *   Format   Assembly Format   Description
 *   65       neg               Negate value.
 *
 * Stack Transition:
 *
 *   ..., value -> ..., result
 *
 * Description:
 *
 *   The neg instruction negates value and pushes the result on top of the stack. The
 *   return type is the same as the operand type.
 *
 *   Negation of integral values is standard twos-complement negation. In particular,
 *   negating the most negative number (which does not have a positive counterpart)
 *   yields the most negative number. To detect this overflow use the sub.ovf
 *   instruction instead (i.e., subtract from 0).
 *
 *   Negating a floating-point number cannot overflow; negating NaN returns NaN.
 *
 *   The acceptable operand types and their corresponding result data types are
 *   encapsulated in Table 3: Unary Numeric Operations.
 *
 * Exceptions:
 *
 *   None.
 *
 * Correctness and Verifiability:
 *
 *   See Table 3: Unary Numeric Operations.
 */
llvm::Error
MethodLLVMEmitter::emit_neg (MonoIrBuilder &builder)
{
	if (stack.empty ())
		return unbalanced_stack (1);

	StackValue value = get_stack (0);
	StackType type = stack_type (value.type);
	MonoType *result;

	// Table III.3: Unary Numeric Operations - each numeric type maps to itself.
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
	case Float:
		// F keeps the width it already has, r4 or r8.
		result = value.type;
		break;
	default:
		return invalid_il (llvm::Twine ("neg is not defined for operand type ")
		                   + describe (value.type, type));
	}

	llvm::Expected<llvm::Type *> ltype = convert_type (result);
	if (!ltype)
		return ltype.takeError ();

	llvm::Value *coerced = coerce (builder, value.value, *ltype);
	llvm::Value *negated = (*ltype)->isFloatingPointTy () ? builder.CreateFNeg (coerced)
	                                                      : builder.CreateNeg (coerced);

	pop_stack (1);
	push_stack (negated, result);
	return llvm::Error::success ();
}

/*
 * III.3.24  ckfinite - check for a finite real number
 *
 *   Format   Assembly Format   Description
 *   C3       ckfinite          Throw ArithmeticException if value is not a finite
 *                              number.
 *
 * Stack Transition:
 *
 *   ..., value -> ..., value
 *
 * Description:
 *
 *   The ckfinite instruction throws ArithmeticException if value (a floating-point
 *   number) is either a "not a number" value (NaN) or +/- infinity value. ckfinite
 *   leaves the value on the stack if no exception is thrown. Execution behavior is
 *   unspecified if value is not a floating-point number.
 *
 * Exceptions:
 *
 *   System.ArithmeticException is thrown if value is a NaN or an infinity.
 *
 * Correctness:
 *
 *   Correct CIL guarantees that value is a floating-point number.
 *
 * Verifiability:
 *
 *   There are no additional verification requirements.
 */
llvm::Error
MethodLLVMEmitter::emit_ckfinite (MonoIrBuilder &builder)
{
	if (stack.empty ())
		return unbalanced_stack (1);

	StackValue value = get_stack (0);

	if (stack_type (value.type) != Float)
		return invalid_il (llvm::Twine ("ckfinite is not defined for operand type ")
		                   + describe (value.type, stack_type (value.type)));

	// UEQ (unordered or equal) against infinity is true for exactly the values
	// ckfinite must reject. NaN triggers it because any comparison with NaN is
	// unordered. Either infinity triggers it because its magnitude equals positive
	// infinity.
	llvm::Type *ftype = value.value->getType ();
	llvm::Value *magnitude = builder.CreateUnaryIntrinsic (llvm::Intrinsic::fabs, value.value);

	emit_cond_exception (
		builder, builder.CreateFCmpUEQ (magnitude, llvm::ConstantFP::getInfinity (ftype)),
		"ArithmeticException");
	return llvm::Error::success ();
}

} // namespace mono

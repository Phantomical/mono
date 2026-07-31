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

constexpr uint8_t
bit (BinaryNumericOp op)
{
	return 1u << static_cast<unsigned> (op);
}

constexpr uint8_t ADD = bit (BinaryNumericOp::Add);
constexpr uint8_t DIV = bit (BinaryNumericOp::Div);
constexpr uint8_t MUL = bit (BinaryNumericOp::Mul);
constexpr uint8_t REM = bit (BinaryNumericOp::Rem);
constexpr uint8_t SUB = bit (BinaryNumericOp::Sub);
constexpr uint8_t ALL = ADD | DIV | MUL | REM | SUB;

/// One cell of Table III.2: what A op B leaves on the stack, and which of the five
/// instructions the cell holds for. X is the table's invalid box.
struct Cell {
	StackType result = Invalid;
	uint8_t ops = 0;
};

constexpr Cell X = {};
constexpr Cell I4_ALL = { Int32, ALL };
constexpr Cell I8_ALL = { Int64, ALL };
constexpr Cell NI_ALL = { NativeInt, ALL };
constexpr Cell F_ALL = { Float, ALL };
constexpr Cell NI_SUB = { NativeInt, SUB };
constexpr Cell MP_ADD = { ManagedPtr, ADD };
constexpr Cell MP_ADD_SUB = { ManagedPtr, ADD | SUB };

/*
 * ECMA-335 III.1.5, Table III.2, transcribed. Indexed [A's type][B's type].
 *
 * The managed-pointer cells are the ones the spec shades as unverifiable; the JIT is
 * not a verifier, so they are accepted like any other.
 */
constexpr Cell TABLE[STACK_TYPE_COUNT][STACK_TYPE_COUNT] = {
	/*              int32       int64    native int     F        &      O */
	/* int32 */ { I4_ALL,     X,        NI_ALL,     X,      MP_ADD, X },
	/* int64 */ { X,          I8_ALL,   X,          X,      X,      X },
	/* nint  */ { NI_ALL,     X,        NI_ALL,     X,      MP_ADD, X },
	/* F     */ { X,          X,        X,          F_ALL,  X,      X },
	/* &     */ { MP_ADD_SUB, X,        MP_ADD_SUB, X,      NI_SUB, X },
	/* O     */ { X,          X,        X,          X,      X,      X },
};

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
op_name (BinaryNumericOp op)
{
	switch (op) {
	case BinaryNumericOp::Add:
		return "add";
	case BinaryNumericOp::Div:
		return "div";
	case BinaryNumericOp::Mul:
		return "mul";
	case BinaryNumericOp::Rem:
		return "rem";
	case BinaryNumericOp::Sub:
		return "sub";
	}

	llvm::report_fatal_error ("op_name: unknown binary numeric operation");
}

bool
is_r8 (MonoType *t)
{
	return mini_get_underlying_type (t)->type == MONO_TYPE_R8;
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

/// The type A op B leaves on the evaluation stack, per ECMA-335 III.1.5,
/// Table III.2, or an InvalidProgramException for the combinations that table
/// rules out.
llvm::Expected<MonoType *>
MethodLLVMEmitter::binary_numeric_result (BinaryNumericOp op, MonoType *lhs, MonoType *rhs)
{
	StackType a = stack_type (lhs);
	StackType b = stack_type (rhs);
	Cell cell = a == Invalid || b == Invalid ? Cell () : TABLE[a][b];

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
		llvm::report_fatal_error ("binary_numeric_result: unreachable result type");
	}
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
	if (stack.size () < 2)
		return unbalanced_stack (2);

	StackValue value1 = get_stack (1);
	StackValue value2 = get_stack (0);

	llvm::Expected<MonoType *> result =
		binary_numeric_result (BinaryNumericOp::Add, value1.type, value2.type);
	if (!result)
		return result.takeError ();

	llvm::Value *sum;

	if ((*result)->byref) {
		/*
		 * A managed pointer plus an integer stays a managed pointer, so index the
		 * pointer rather than doing the arithmetic on it - that keeps the result
		 * something the GC can still recognize as pointing into its object.
		 */
		bool value1_is_pointer = value1.type->byref;
		llvm::Value *base = value1_is_pointer ? value1.value : value2.value;
		llvm::Value *index = value1_is_pointer ? value2.value : value1.value;

		sum = builder.CreateGEP (
			builder.getInt8Ty (), base,
			coerce (builder, index, builder.getIntNTy (TARGET_SIZEOF_VOID_P * 8)));
	} else {
		llvm::Expected<llvm::Type *> type = convert_type (*result);
		if (!type)
			return type.takeError ();

		llvm::Value *lhs = coerce (builder, value1.value, *type);
		llvm::Value *rhs = coerce (builder, value2.value, *type);

		sum = (*type)->isFloatingPointTy () ? builder.CreateFAdd (lhs, rhs)
		                                    : builder.CreateAdd (lhs, rhs);
	}

	pop_stack (2);
	push_stack (sum, *result);
	return llvm::Error::success ();
}

} // namespace mono

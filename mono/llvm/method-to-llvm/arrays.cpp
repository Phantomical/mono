#include "method-to-llvm.hpp"
#include "operand-class.hpp"
#include "runtime-error.hpp"
#include "../passes/alloc-func.hpp"
#include "../passes/array-address.hpp"
#include "../passes/array-shape.hpp"
#include "../passes/vtable-func.hpp"
#include "../runtime/options.hpp"
#include "mono/metadata/abi-details.h"
#include "mono/metadata/class-init.h"
#include "mono/metadata/class-inlines.h"
#include "mono/metadata/class-internals.h"
#include "mono/metadata/debug-helpers.h"
#include "mono/metadata/metadata.h"
#include "mono/metadata/object-internals.h"
#include "mono/metadata/opcodes.h"
#include "mono/metadata/tokentype.h"

// gc-internals.h declares C functions but does not mark them extern "C"
// itself, so this include must supply the wrap.
extern "C" {
#include "mono/metadata/gc-internals.h"
}

#include <llvm/IR/Attributes.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Type.h>
#include <llvm/Support/ErrorHandling.h>

#include <cstdio>

namespace mono {

MonoType *
MethodLLVMEmitter::builtin_element_type (int opcode)
{
	switch (opcode) {
	case MONO_CEE_LDELEM_I1:
	case MONO_CEE_LDIND_I1:
		return m_class_get_byval_arg (mono_defaults.sbyte_class);
	case MONO_CEE_LDELEM_U1:
	case MONO_CEE_STELEM_I1:
	case MONO_CEE_LDIND_U1:
	case MONO_CEE_STIND_I1:
		return m_class_get_byval_arg (mono_defaults.byte_class);
	case MONO_CEE_LDELEM_I2:
	case MONO_CEE_LDIND_I2:
		return m_class_get_byval_arg (mono_defaults.int16_class);
	case MONO_CEE_LDELEM_U2:
	case MONO_CEE_STELEM_I2:
	case MONO_CEE_LDIND_U2:
	case MONO_CEE_STIND_I2:
		return m_class_get_byval_arg (mono_defaults.uint16_class);
	case MONO_CEE_LDELEM_I4:
	case MONO_CEE_STELEM_I4:
	case MONO_CEE_LDIND_I4:
	case MONO_CEE_STIND_I4:
		return mono_get_int32_type ();
	case MONO_CEE_LDELEM_U4:
	case MONO_CEE_LDIND_U4:
		return m_class_get_byval_arg (mono_defaults.uint32_class);
	case MONO_CEE_LDELEM_I8:
	case MONO_CEE_STELEM_I8:
	case MONO_CEE_LDIND_I8:
	case MONO_CEE_STIND_I8:
		return m_class_get_byval_arg (mono_defaults.int64_class);
	case MONO_CEE_LDELEM_I:
	case MONO_CEE_STELEM_I:
	case MONO_CEE_LDIND_I:
	case MONO_CEE_STIND_I:
		return mono_get_int_type ();
	case MONO_CEE_LDELEM_R4:
	case MONO_CEE_STELEM_R4:
	case MONO_CEE_LDIND_R4:
	case MONO_CEE_STIND_R4:
		return m_class_get_byval_arg (mono_defaults.single_class);
	case MONO_CEE_LDELEM_R8:
	case MONO_CEE_STELEM_R8:
	case MONO_CEE_LDIND_R8:
	case MONO_CEE_STIND_R8:
		return m_class_get_byval_arg (mono_defaults.double_class);
	case MONO_CEE_LDELEM_REF:
	case MONO_CEE_STELEM_REF:
	case MONO_CEE_LDIND_REF:
	case MONO_CEE_STIND_REF:
		return mono_get_object_type ();
	default:
		llvm::report_fatal_error ("builtin_element_type: not an element opcode");
	}
}

llvm::Expected<MonoClass *>
MethodLLVMEmitter::resolve_class (uint32_t token)
{
	ERROR_DECL (metadata_error);
	MonoGenericContext *context = mono_method_get_context (method);
	MonoClass *klass;

	if (in_wrapper ()) {
		klass = static_cast<MonoClass *> (wrapper_data (token));

		if (klass == nullptr)
			return invalid_il (llvm::Twine ("wrapper data slot ") + llvm::Twine (token)
			                   + " does not name a type");

		if (context != nullptr) {
			klass = mono_class_inflate_generic_class_checked (klass, context,
			                                                  metadata_error);
			if (klass == nullptr)
				return runtime_error (metadata_error);
		}
	} else {
		klass = mono_class_get_and_inflate_typespec_checked (
			m_class_get_image (method->klass), token, context, metadata_error);

		/*
		 * A token that names a missing type is a defect in the program's
		 * environment, not in its encoding. The failure travels on as the
		 * loader recorded it, instead of becoming invalid IL, and it
		 * already names the missing assembly and type.
		 */
		if (klass == nullptr)
			return runtime_error (metadata_error);
	}

	mono_class_init_internal (klass);

	return klass;
}

llvm::Expected<MonoType *>
MethodLLVMEmitter::element_type_from_token (uint32_t token)
{
	llvm::Expected<MonoClass *> resolved = resolve_class (token);

	if (!resolved)
		return resolved.takeError ();

	MonoClass *klass = *resolved;

	/*
	 * A class the runtime already gave up on still resolves. The failure is
	 * recorded on the class instead of replacing it, so naming that class in
	 * IL must be refused here. The failure is a type load, and recover ()
	 * turns it into a method that raises TypeLoadException when the method
	 * runs.
	 */
	if (mono_class_has_failure (klass)) {
		ERROR_DECL (load_error);

		mono_error_set_for_class_failure (load_error, klass);
		return runtime_error (load_error);
	}

	return m_class_get_byval_arg (klass);
}

llvm::Expected<llvm::Value *>
MethodLLVMEmitter::array_length (MonoIrBuilder &builder, StackValue array)
{
	if (stack_type (array.type) != ObjectRef)
		return invalid_il (llvm::Twine ("an array was expected, not operand type ")
		                   + describe (array.type, stack_type (array.type)));

	emit_null_check (builder, array.value);

	llvm::Value *slot =
		builder.CreateGEP (builder.getInt8Ty (), array.value,
	                           builder.getInt32 (MONO_STRUCT_OFFSET (MonoArray, max_length)));

	/* This is a scalar typedef, so its size alone is the layout. No ABI table is involved. */
	constexpr unsigned bytes = sizeof (mono_array_size_t);

	llvm::LoadInst *length =
		builder.CreateAlignedLoad (builder.getIntNTy (bytes * 8), slot, llvm::Align (bytes));

	mark_array_header_load (length);
	return length;
}

llvm::Expected<llvm::Value *>
MethodLLVMEmitter::element_address (MonoIrBuilder &builder, StackValue array, StackValue index,
                                    MonoType *element)
{
	StackType index_type = stack_type (index.type);

	if (index_type != Int32 && index_type != NativeInt)
		return invalid_il (llvm::Twine ("an array index cannot be operand type ")
		                   + describe (index.type, index_type));

	llvm::Expected<llvm::Value *> length = array_length (builder, array);
	if (!length)
		return length.takeError ();

	llvm::Type *native = builder.getIntNTy (TARGET_SIZEOF_VOID_P * 8);
	llvm::Value *at = index.value;

	if (at->getType ()->isPointerTy ())
		at = builder.CreatePtrToInt (at, native);
	// A negative index read as unsigned becomes enormous. Zero-extending it here
	// lets one `uge` check reject both a negative and an out-of-range index at once.
	at = builder.CreateZExtOrTrunc (at, native);

	emit_cond_exception (
		builder, builder.CreateICmpUGE (at, builder.CreateZExtOrTrunc (*length, native)),
		"IndexOutOfRangeException");

	MonoClass *klass = mono_class_from_mono_type_internal (element);
	int32_t size = mono_class_array_element_size (klass);
	llvm::Value *vector =
		builder.CreateGEP (builder.getInt8Ty (), array.value,
	                           builder.getInt32 (MONO_STRUCT_OFFSET (MonoArray, vector)));

	return builder.CreateGEP (builder.getInt8Ty (), vector,
	                          builder.CreateMul (at, llvm::ConstantInt::get (native, size)));
}

/// Throws ArrayTypeMismatchException unless array is exactly an instance of array_class.
///
/// The check compares vtables for exact equality, not by assignability.
/// Covariance lets a string[] arrive where an object[] is expected. An
/// address into it, typed at the wrong element, must be refused before
/// anything writes through it.
llvm::Error
MethodLLVMEmitter::emit_array_type_check (MonoIrBuilder &builder, llvm::Value *array,
                                          MonoClass *array_class)
{
	emit_null_check (builder, array);

	llvm::Value *vtable = load_vtable (builder, array);
	llvm::Expected<llvm::Value *> wanted =
		class_operand (builder, array_class, "mono_vtable_");

	if (!wanted)
		return wanted.takeError ();

	emit_cond_exception (builder, builder.CreateICmpNE (vtable, *wanted),
	                     "ArrayTypeMismatchException");
	return llvm::Error::success ();
}

/*
 * III.4.12  ldlen - load the length of an array
 *
 *   Format   Assembly Format   Description
 *   8E       ldlen             Push the length (of type native unsigned int) of array
 *                              on the stack.
 *
 * Stack Transition:
 *
 *   ..., array -> ..., length
 *
 * Description:
 *
 *   The ldlen instruction pushes the number of elements of array (a zero-based,
 *   one-dimensional array) on the stack.
 *
 *   Arrays are objects and hence represented by a value of type O. The return value is
 *   a native unsigned int.
 *
 * Exceptions:
 *
 *   System.NullReferenceException is thrown if array is null.
 */
llvm::Error
MethodLLVMEmitter::emit_ldlen (MonoIrBuilder &builder)
{
	if (stack.empty ())
		return unbalanced_stack (1);

	llvm::Expected<llvm::Value *> length = array_length (builder, get_stack (0));
	if (!length)
		return length.takeError ();

	llvm::Value *native =
		builder.CreateZExtOrTrunc (*length, builder.getIntNTy (TARGET_SIZEOF_VOID_P * 8));

	pop_stack (1);
	push_stack (native, mono_get_int_type ());
	return llvm::Error::success ();
}

/*
 * III.4.9  ldelema - load address of an element of an array
 *
 *   Format     Assembly Format    Description
 *   8F <T>     ldelema class      Load the address of element at index onto the top of
 *                                 the stack.
 *
 * Stack Transition:
 *
 *   ..., array, index -> ..., address
 *
 * Description:
 *
 *   The ldelema instruction loads the address of the element with index index (of type
 *   native int or int32) in the zero-based one-dimensional array array and places it
 *   on the top of the stack. Arrays are objects and hence represented by a value of
 *   type O. The return address is a managed pointer (type &).
 *
 * Exceptions:
 *
 *   System.NullReferenceException is thrown if array is null.
 *
 *   System.IndexOutOfRangeException is thrown if index is negative, or larger than the
 *   bound of array.
 *
 *   System.ArrayTypeMismatchException is thrown if array does not hold elements of the
 *   required type.
 */
llvm::Error
MethodLLVMEmitter::emit_ldelema (MonoIrBuilder &builder, uint32_t token)
{
	if (stack.size () < 2)
		return unbalanced_stack (2);

	llvm::Expected<MonoType *> element = element_type_from_token (token);
	if (!element)
		return element.takeError ();

	MonoClass *klass = mono_class_from_mono_type_internal (*element);

	/*
	 * Wrappers are deliberately lax about the element type they name.
	 */
	if (!m_class_is_valuetype (klass) && method->wrapper_type == MONO_WRAPPER_NONE
	    && !prefixes.readonly_)
		if (llvm::Error refused = emit_array_type_check (
			    builder, get_stack (1).value, mono_class_create_array (klass, 1)))
			return refused;

	llvm::Expected<llvm::Value *> address =
		element_address (builder, get_stack (1), get_stack (0), *element);
	if (!address)
		return address.takeError ();

	pop_stack (2);
	trusted_byrefs.insert (*address);
	push_stack (*address, m_class_get_this_arg (klass));
	return llvm::Error::success ();
}

/*
 * III.4.7  ldelem - load element from array
 * III.4.8  ldelem.<type> - load an element of an array
 *
 *   Format     Assembly Format    Description
 *   A3 <T>     ldelem typeTok     Load the element at index onto the top of the stack.
 *   90         ldelem.i1          Load the element with type int8 at index onto the top
 *                                 of the stack as an int32.
 *   92         ldelem.i2          Load the element with type int16 at index onto the
 *                                 top of the stack as an int32.
 *   94         ldelem.i4          Load the element with type int32 at index onto the
 *                                 top of the stack as an int32.
 *   96         ldelem.i8          Load the element with type int64 at index onto the
 *                                 top of the stack as an int64.
 *   91         ldelem.u1          Load the element with type unsigned int8 at index
 *                                 onto the top of the stack as an int32.
 *   93         ldelem.u2          Load the element with type unsigned int16 at index
 *                                 onto the top of the stack as an int32.
 *   95         ldelem.u4          Load the element with type unsigned int32 at index
 *                                 onto the top of the stack as an int32.
 *   97         ldelem.i           Load the element with type native int at index onto
 *                                 the top of the stack as a native int.
 *   98         ldelem.r4          Load the element with type float32 at index onto the
 *                                 top of the stack as an F.
 *   99         ldelem.r8          Load the element with type float64 at index onto the
 *                                 top of the stack as an F.
 *   9A         ldelem.ref         Load the element at index onto the top of the stack
 *                                 as an O. The type of the O is the same as the element
 *                                 type of the array pushed on the CIL stack.
 *
 * Stack Transition:
 *
 *   ..., array, index -> ..., value
 *
 * Description:
 *
 *   The ldelem instruction loads the value of the element with index index (of type
 *   native int or int32) in the zero-based one-dimensional array array and places it
 *   on the top of the stack. Arrays are objects and hence represented by a value of
 *   type O.
 *
 *   The return value is the value in the array element, converted to the representation
 *   of its intermediate type (§I.8.7) where required.
 *
 * Exceptions:
 *
 *   System.NullReferenceException is thrown if array is null.
 *
 *   System.IndexOutOfRangeException is thrown if index is negative, or larger than the
 *   bound of array.
 *
 *   System.ArrayTypeMismatchException is thrown if array does not hold elements of the
 *   required type.
 */
llvm::Error
MethodLLVMEmitter::emit_ldelem (MonoIrBuilder &builder, MonoType *element)
{
	if (stack.size () < 2)
		return unbalanced_stack (2);

	llvm::Expected<llvm::Value *> address =
		element_address (builder, get_stack (1), get_stack (0), element);
	if (!address)
		return address.takeError ();

	pop_stack (2);
	return push_from_location (builder, *address, element, /*native=*/false,
	                           ManagedAccess::of_element (element, 1));
}

namespace {

/// Whether nothing but element itself can be the element class of an array
/// that a variable of that array type holds.
///
/// This is the exclusion list of `get_virtual_stelemref_kind ()`
/// (`mono/metadata/marshal.c`), copied rather than rebuilt. The sealed flag
/// alone answers wrongly. An array class carries the flag and is covariant on
/// its element type all the same. A variant generic argument makes an
/// unrelated instantiation assignable. `string[]` is assignable to
/// `IComparable[]`, so an interface is out whatever flags it carries. A
/// marshal-by-ref class is out because only the runtime reasons about a
/// transparent proxy.
bool
element_class_is_final (MonoClass *element)
{
	if (m_class_get_rank (element) != 0 || mono_class_is_interface (element)
	    || mono_class_is_marshalbyref (element)
	    || mono_class_has_variant_generic_params (element))
		return false;

	return mono_class_is_sealed (element);
}

/// Prints the form a reference store into an array was given.
///
/// Counting these lines over a corpus is what says how its sites divide
/// between the three forms.
void
trace_stelem_check (MonoMethod *method, const char *form)
{
	if (!is_jit_trace_enabled ())
		return;

	char *name = mono_method_full_name (method, TRUE);

	MONO_LOCK (jit_trace_mutex ())
	{
		fprintf (stderr, "[llvm-jit] stelem.ref %s in %s\n", form, name);
	}
	g_free (name);
}

} // namespace

/// The class every reference the array holds is an instance of, or null where
/// covariance leaves that open until the store runs.
///
/// A variable of type `object[]` can hold a `string[]`. So the static type
/// answers only for an element class nothing derives from, or for an array
/// this body allocated itself.
MonoClass *
MethodLLVMEmitter::exact_element_class (const StackValue &array)
{
	if (stack_type (array.type) != ObjectRef || array.type->type == MONO_TYPE_VAR
	    || array.type->type == MONO_TYPE_MVAR)
		return nullptr;

	MonoClass *klass = mono_class_from_mono_type_internal (array.type);

	if (klass == nullptr || m_class_get_rank (klass) == 0 || depends_on_context (klass))
		return nullptr;

	MonoClass *element = m_class_get_element_class (klass);

	// newarr and an array newobj name the class they allocate, so this value
	// is an array of exactly that element class. A value that reached here
	// through a spill is not in the set, which costs a test and never
	// correctness.
	if (allocated_here.count (array.value) != 0)
		return element;

	return element_class_is_final (element) ? element : nullptr;
}

/// Whether every object this stack slot can hold is an instance of element.
///
/// A value's run-time class is assignable to the type the IL tracks it as, and
/// assignability is transitive. So a static type this says yes to leaves the
/// store nothing to ask about. The caller still has to know that the array
/// really holds element, which `exact_element_class ()` answers.
bool
MethodLLVMEmitter::is_always_an_instance_of (const StackValue &value, MonoClass *element)
{
	if (stack_type (value.type) != ObjectRef || value.type->type == MONO_TYPE_VAR
	    || value.type->type == MONO_TYPE_MVAR)
		return false;

	MonoClass *klass = mono_class_from_mono_type_internal (value.type);

	if (klass == nullptr || depends_on_context (klass))
		return false;

	return mono_class_is_assignable_from_internal (element, klass) != 0;
}

/// Emits what a reference store into an array has to test before it writes:
/// the ArrayTypeMismatchException that `stelem.ref` and `Set` both list.
///
/// value is the stack slot the store reads, and stored is that slot coerced to
/// the element type. The static type comes from the first and the reference
/// the test reads from the second. Leaves the builder on the block the store
/// goes in.
///
/// `mono_helper_stelem_ref_check ()` leaves the exception pending, and the
/// wrapper call around it is what throws it.
llvm::Error
MethodLLVMEmitter::emit_stelem_ref_check (MonoIrBuilder &builder, const StackValue &array,
                                          const StackValue &value, llvm::Value *stored)
{
	MonoClass *element = exact_element_class (array);

	// Storing null always succeeds, whatever the array turns out to hold.
	if (llvm::isa<llvm::ConstantPointerNull> (stored)
	    || (element != nullptr && is_always_an_instance_of (value, element))) {
		trace_stelem_check (method, "needs no check");
		return llvm::Error::success ();
	}

	llvm::Expected<llvm::Function *> check =
		icall_wrapper_decl (MONO_JIT_ICALL_mono_helper_stelem_ref_check);

	if (!check)
		return check.takeError ();

	llvm::Type *ptr = llvm::PointerType::get (context (), 0);
	llvm::BasicBlock *test = llvm::BasicBlock::Create (context (), "stelem_test", function);
	llvm::BasicBlock *ask = create_cold_block ("stelem_ask");
	llvm::BasicBlock *done = llvm::BasicBlock::Create (context (), "stelem_ok", function);

	// The test below reads the value's vtable, so the null goes in front of it.
	builder.CreateCondBr (builder.CreateIsNull (stored), done, test);
	builder.SetInsertPoint (test);

	llvm::Value *wanted = nullptr;

	if (element != nullptr) {
		llvm::Expected<llvm::Value *> constant =
			class_operand (builder, element, "mono_class_");

		if (!constant)
			return constant.takeError ();

		wanted = *constant;
		trace_stelem_check (method, "tests a constant element class");
	} else {
		emit_null_check (builder, array.value);

		llvm::Value *array_vtable = load_vtable (builder, array.value, "array_vtable");
		llvm::Value *array_class = builder.CreateCall (vtable_klass_decl (*module),
		                                               { array_vtable }, "array_class");

		// LLVM CSEs this load across the stores of an initializer. A
		// call through the array's stelemref vtable slot, which is how
		// mini answers this, is opaque to it.
		wanted = builder.CreateAlignedLoad (
			ptr,
			builder.CreateGEP (builder.getInt8Ty (), array_class,
		                           builder.getInt32 (MONO_STRUCT_OFFSET (MonoClass, element_class))),
			llvm::Align (TARGET_SIZEOF_VOID_P), "element_class");
		trace_stelem_check (method, "tests the array's element class");
	}

	llvm::Value *value_vtable = load_vtable (builder, stored, "value_vtable");
	llvm::Value *value_class = builder.CreateCall (vtable_klass_decl (*module),
	                                               { value_vtable }, "value_class");

	// A value of exactly the element class is the general answer, so it goes
	// first. A store the compare below catches pays this one as well.
	llvm::BasicBlock *inexact = ask;

	if (element == nullptr)
		inexact = llvm::BasicBlock::Create (context (), "stelem_inexact", function);

	builder.CreateCondBr (builder.CreateICmpEQ (value_class, wanted), done, inexact);

	if (element == nullptr) {
		/*
		 * Every object is an instance of System.Object, so an array that
		 * really holds object[] takes any reference. Only the array
		 * answers this: covariance leaves the static type saying object[]
		 * for a string[] as well.
		 *
		 * It earns its compare on the containers. ArrayList, List<object>
		 * and a params array all store into object[], and a string or a
		 * boxed int misses the exact compare above every time.
		 */
		builder.SetInsertPoint (inexact);

		llvm::Expected<llvm::Value *> any =
			class_operand (builder, mono_defaults.object_class, "mono_class_");

		if (!any)
			return any.takeError ();

		builder.CreateCondBr (builder.CreateICmpEQ (wanted, *any), done, ask);
	}

	// Both tests are exact, so a legal store still arrives here. A derived
	// class, a class implementing an interface element class, and a
	// transparent proxy standing in for either are all legal and all miss.
	builder.SetInsertPoint (ask);
	emit_protected_call (builder, *check,
	                     adapt_to_callee (builder, *check, { array.value, stored }));
	builder.CreateBr (done);
	builder.SetInsertPoint (done);
	return llvm::Error::success ();
}

/*
 * III.4.29  stelem - store element to array
 * III.4.30  stelem.<type> - store an element of an array
 *
 *   Format     Assembly Format    Description
 *   A4 <T>     stelem typeTok     Replace array element at index with the value on the
 *                                 stack.
 *   9C         stelem.i1          Replace array element at index with the int8 value on
 *                                 the stack.
 *   9D         stelem.i2          Replace array element at index with the int16 value
 *                                 on the stack.
 *   9E         stelem.i4          Replace array element at index with the int32 value
 *                                 on the stack.
 *   9F         stelem.i8          Replace array element at index with the int64 value
 *                                 on the stack.
 *   9B         stelem.i           Replace array element at index with the i value on
 *                                 the stack.
 *   A0         stelem.r4          Replace array element at index with the float32 value
 *                                 on the stack.
 *   A1         stelem.r8          Replace array element at index with the float64 value
 *                                 on the stack.
 *   A2         stelem.ref         Replace array element at index with the ref value on
 *                                 the stack.
 *
 * Stack Transition:
 *
 *   ..., array, index, value -> ...,
 *
 * Description:
 *
 *   The stelem instruction replaces the value of the element with index index (of type
 *   native int or int32) in the zero-based one-dimensional array array with value.
 *   Arrays are objects and hence represented by a value of type O.
 *
 *   Storing into arrays that hold values smaller than 4 bytes whose intermediate type
 *   is int32 truncates the value as it moves from the stack to the array. Floating-point
 *   values are rounded from their native size (type F) to the size associated with the
 *   array.
 *
 * Exceptions:
 *
 *   System.NullReferenceException is thrown if array is null.
 *
 *   System.IndexOutOfRangeException is thrown if index is negative, or larger than the
 *   bound of array.
 *
 *   System.ArrayTypeMismatchException is thrown if array does not hold elements of the
 *   required type.
 */
llvm::Error
MethodLLVMEmitter::emit_stelem (MonoIrBuilder &builder, MonoType *element)
{
	if (stack.size () < 3)
		return unbalanced_stack (3);

	StackValue array = get_stack (2);
	llvm::Expected<llvm::Value *> value = coerce_to_location (builder, get_stack (0), element);
	if (!value)
		return value.takeError ();

	// The element type here is the opcode's, and what the array really holds
	// can be narrower than that.
	if (mini_type_is_reference (element))
		if (llvm::Error refused =
			    emit_stelem_ref_check (builder, array, get_stack (0), *value))
			return refused;

	llvm::Expected<llvm::Value *> address =
		element_address (builder, array, get_stack (1), element);
	if (!address)
		return address.takeError ();

	pop_stack (3);
	if (llvm::Error stored = emit_memory_store (builder, *value, *address, element,
	                                            ManagedAccess::of_element (element, 1)))
		return stored;
	return llvm::Error::success ();
}

/// The symbolic element-address call that lower_array_addresses () expands.
///
/// The expansion turns (array, idx...) into a pointer at the element, and
/// throws the exception whose token trails the indices when one of them misses
/// its dimension. The pass reads MonoArray itself; what the declaration carries
/// is the rank, the element size and whether the array is bounded.
llvm::Expected<llvm::Value *>
MethodLLVMEmitter::array_accessor_address (MonoIrBuilder &builder, MonoClass *klass,
                                           llvm::Value *array,
                                           llvm::ArrayRef<llvm::Value *> indices)
{
	MonoClass *eclass = m_class_get_element_class (klass);
	int32_t size = mono_class_array_element_size (eclass);
	bool bounded = m_class_get_byval_arg (klass)->type == MONO_TYPE_ARRAY;
	std::string name = (llvm::Twine (array_address_prefix) + "r"
	                    + llvm::Twine (indices.size ()) + ".s" + llvm::Twine (size)
	                    + (bounded ? ".b" : ""))
	                           .str ();
	llvm::Function *decl = module->getFunction (name);

	if (decl == nullptr) {
		llvm::Type *ptr = llvm::PointerType::get (context (), 0);
		// The array, one index for each dimension, and the exception token.
		std::vector<llvm::Type *> params (2 + indices.size (), builder.getInt32Ty ());

		params[0] = ptr;
		decl = llvm::Function::Create (llvm::FunctionType::get (ptr, params, false),
		                               llvm::GlobalValue::ExternalLinkage, name,
		                               module);

		char spec[64];

		snprintf (spec, sizeof (spec), "rank=%zu,size=%d,bounded=%d", indices.size (),
		          size, bounded ? 1 : 0);
		decl->addFnAttr (llvm::Attribute::get (context (), array_address_attribute,
		                                       spec));
	}

	MonoClass *ioor = mono_class_load_from_name (mono_get_corlib (), "System",
	                                             "IndexOutOfRangeException");
	std::vector<llvm::Value *> args;

	args.reserve (2 + indices.size ());
	args.push_back (array);
	args.insert (args.end (), indices.begin (), indices.end ());
	args.push_back (
		builder.getInt32 (m_class_get_type_token (ioor) - MONO_TOKEN_TYPE_DEF));
	return emit_protected_call (builder, decl, args);
}

/// A call to Get, Set or Address on an array class.
///
/// These accessors have no IL body. The runtime resolves them per call site,
/// and the marshal wrapper it offers instead calls the accessor again, so
/// the site lowers here. The address comes from the symbolic call above,
/// and the load or store around it is shaped like ldelem or stelem.
llvm::Error
MethodLLVMEmitter::emit_array_accessor_call (MonoIrBuilder &builder, MonoMethod *accessor,
                                             MonoMethodSignature *sig)
{
	std::string_view what = accessor->name;
	bool is_set = what == "Set";

	if (!is_set && what != "Get" && what != "Address")
		return unsupported_il (llvm::Twine ("array runtime method ") + accessor->name);

	uint32_t rank = sig->param_count - (is_set ? 1 : 0);
	size_t depth = 1 + sig->param_count;

	if (stack.size () < depth)
		return unbalanced_stack (depth);

	MonoClass *eclass = m_class_get_element_class (accessor->klass);
	MonoType *element = m_class_get_byval_arg (eclass);
	StackValue array = get_stack (sig->param_count);

	if (stack_type (array.type) != ObjectRef)
		return invalid_il (llvm::Twine ("an array was expected, not operand type ")
		                   + describe (array.type, stack_type (array.type)));

	llvm::Value *value = nullptr;

	if (is_set) {
		llvm::Expected<llvm::Value *> coerced =
			coerce_to_location (builder, get_stack (0), element);

		if (!coerced)
			return coerced.takeError ();
		value = *coerced;

		if (mini_type_is_reference (element))
			if (llvm::Error refused = emit_stelem_ref_check (builder, array,
			                                                 get_stack (0), value))
				return refused;
	}

	if (what == "Address" && !m_class_is_valuetype (eclass) && !prefixes.readonly_)
		if (llvm::Error refused =
			    emit_array_type_check (builder, array.value, accessor->klass))
			return refused;

	emit_null_check (builder, array.value);

	std::vector<llvm::Value *> indices;

	indices.reserve (rank);
	for (uint32_t i = 0; i < rank; ++i) {
		StackValue index = get_stack ((is_set ? 1 : 0) + (rank - 1 - i));
		StackType type = stack_type (index.type);

		if (type != Int32 && type != NativeInt)
			return invalid_il (llvm::Twine ("an array index cannot be operand type ")
			                   + describe (index.type, type));

		llvm::Value *raw = index.value;

		if (raw->getType ()->isPointerTy ())
			raw = builder.CreatePtrToInt (
				raw, builder.getIntNTy (TARGET_SIZEOF_VOID_P * 8));
		indices.push_back (builder.CreateZExtOrTrunc (raw, builder.getInt32Ty ()));
	}

	llvm::Expected<llvm::Value *> address =
		array_accessor_address (builder, accessor->klass, array.value, indices);

	if (!address)
		return address.takeError ();

	pop_stack (depth);

	if (is_set) {
		if (llvm::Error stored = emit_memory_store (
			    builder, value, *address, element,
			    ManagedAccess::of_element (element, m_class_get_rank (accessor->klass))))
			return stored;
	} else if (what == "Get") {
		return push_from_location (
			builder, *address, element, /*native=*/false,
			ManagedAccess::of_element (element, m_class_get_rank (accessor->klass)));
	} else {
		trusted_byrefs.insert (*address);
		push_stack (*address, m_class_get_this_arg (eclass));
	}
	return llvm::Error::success ();
}

/// A call to Array.GetGenericValueImpl<T> or Array.SetGenericValueImpl<T>.
///
/// Each moves one element between the array and a location its caller names.
/// The element type comes from the call site rather than from the array. That
/// is the same width, because an array implements a generic collection
/// interface at its own element type and at the reference types that element
/// converts to.
///
/// Each caller in corlib tests the index first. This emits the test again, and
/// an inliner that folds the caller in takes one of the two back out.
llvm::Error
MethodLLVMEmitter::emit_array_generic_access (MonoIrBuilder &builder,
                                              MonoMethodSignature *sig,
                                              ArrayGenericAccess what)
{
	constexpr size_t depth = 3;     // the array, the index, the caller's location

	if (stack.size () < depth)
		return unbalanced_stack (depth);

	MonoType *element =
		m_class_get_byval_arg (mono_class_from_mono_type_internal (sig->params[1]));
	llvm::Expected<llvm::Value *> address =
		element_address (builder, get_stack (2), get_stack (1), element);

	if (!address)
		return address.takeError ();

	llvm::Expected<llvm::Value *> slot = indirect_address (builder, get_stack (0));

	if (!slot)
		return slot.takeError ();

	pop_stack (depth);

	bool is_set = what == ArrayGenericAccess::set;
	ManagedAccess access = ManagedAccess::of_element (element, 1);
	llvm::Value *source = is_set ? *slot : *address;
	llvm::Value *value = source;

	// emit_memory_store () copies a value class out of the address it gets, and
	// takes anything else as the value itself.
	if (!held_in_memory (element)) {
		llvm::Expected<llvm::Type *> type = convert_type (element);

		if (!type)
			return type.takeError ();

		value = emit_memory_load (builder, *type, source, element,
		                          is_set ? ManagedAccess::untagged () : access);
	}

	if (is_set)
		return emit_memory_store (builder, value, *address, element, access);

	/*
	 * A store into this frame needs no card, because the collector scans a
	 * thread's stack conservatively. Each caller in corlib names a local, so
	 * this is the path a foreach over an array takes, and the plain store is
	 * what lets SROA promote that local away.
	 */
	if (llvm::isa<llvm::AllocaInst> ((*slot)->stripPointerCasts ())) {
		if (held_in_memory (element))
			copy_vtype (builder, *slot, value, element, /*native=*/false);
		else
			builder.CreateAlignedStore (value, *slot, access_alignment (element));
		return llvm::Error::success ();
	}

	return emit_memory_store (builder, value, *slot, element);
}

/// Whether UnsafeMov can reinterpret from as to. Both must be reference
/// types, blittable value types of equal size, or scalars in the same
/// register class.
static bool
unsafe_mov_compatible (MonoClass *from, MonoClass *to)
{
	if (!m_class_is_valuetype (from) && !m_class_is_valuetype (to))
		return true;
	if (!m_class_is_valuetype (from) || !m_class_is_valuetype (to))
		return false;
	if (m_class_has_references (from) || m_class_has_references (to))
		return false;

	MonoType *ftype = m_class_get_byval_arg (from);
	MonoType *ttype = m_class_get_byval_arg (to);

	if (MONO_TYPE_ISSTRUCT (ftype) != MONO_TYPE_ISSTRUCT (ttype))
		return false;
	if (ftype->type == MONO_TYPE_R4 || ftype->type == MONO_TYPE_R8
	    || ttype->type == MONO_TYPE_R4 || ttype->type == MONO_TYPE_R8)
		return false;

	int32_t from_size = mono_class_value_size (from, nullptr);
	int32_t to_size = mono_class_value_size (to, nullptr);

	return from_size == to_size
	       || (!MONO_TYPE_ISSTRUCT (ftype) && from_size <= 4 && to_size <= 4);
}

/// The IL body of Array.UnsafeMov<S,R> (S) boxes S and unboxes it as R. The unbox type
/// check rightly refuses that for pairs like an enum and its unsigned
/// underlying type. The helper exists to skip that check.
llvm::Error
MethodLLVMEmitter::emit_unsafe_mov (MonoIrBuilder &builder, MonoMethodSignature *sig)
{
	if (stack.empty ())
		return unbalanced_stack (1);

	MonoClass *from = mono_class_from_mono_type_internal (sig->params[0]);
	MonoClass *to = mono_class_from_mono_type_internal (sig->ret);

	if (!unsafe_mov_compatible (from, to))
		return unsupported_il ("UnsafeMov between incompatible types");

	llvm::Expected<llvm::Type *> type = convert_type (sig->ret);
	if (!type)
		return type.takeError ();

	StackValue value = get_stack (0);
	llvm::Value *raw = value.value;

	/*
	 * The two types are the same bytes, by the compatibility check above. A
	 * result that lives in memory is those bytes copied into a slot of its
	 * own type.
	 */
	if (held_in_memory (sig->ret)) {
		llvm::Value *home = spill_to_temporary (builder, value.type);

		pop_stack (1);
		return push_from_location (builder, home, sig->ret);
	}

	llvm::Value *result;

	if (raw->getType () == *type) {
		result = raw;
	} else if (raw->getType ()->isIntegerTy () && (*type)->isIntegerTy ()) {
		result = builder.CreateSExtOrTrunc (raw, *type);
	} else {
		llvm::Value *home = spill_to_temporary (builder, value.type);

		result = builder.CreateAlignedLoad (*type, home, type_alignment (sig->ret));
	}

	pop_stack (1);
	return push_produced (builder, result, sig->ret);
}

namespace {

/**
 * Builds the return attribute stating how many bytes a caller can read at a
 * fresh \p array of \p length elements.
 *
 * LLVM reads `dereferenceable` as a promise the pointer is not null.
 * \p answers_null picks the `_or_null` form for an allocator that can answer
 * null.
 *
 * `emit_object_alloc ()`'s allocator carries `allocsize` instead. Both of its
 * operands are parameter numbers. The size of an element is a constant here,
 * not an argument, so there is no parameter to name.
 */
llvm::Attribute
array_extent (bool answers_null, MonoClass *array, llvm::Value *length)
{
	llvm::LLVMContext &c = length->getContext ();

	// The header sits in front of the elements, so it is there whatever the
	// length is. A constant length sizes the elements behind it as well.
	uint64_t bytes = MONO_SIZEOF_MONO_ARRAY;
	auto *count = llvm::dyn_cast<llvm::ConstantInt> (length);
	int32_t element = mono_array_element_size (array);

	// The elements are added only for a constant length that is in range. Any
	// other length leaves the extent at the header alone, a safe floor whether
	// the true array is larger or the call never returns. The same range check
	// also keeps the multiply below from overflowing.
	if (count != nullptr && element > 0 && count->getValue ().isNonNegative ()
	    && count->getValue ().getActiveBits () <= 32)
		bytes += count->getValue ().getZExtValue () * (uint64_t) element;

	// The collector rounds the request up to its own alignment, so this is a
	// floor rather than the size of the block.
	return answers_null ? llvm::Attribute::getWithDereferenceableOrNullBytes (c, bytes)
	                    : llvm::Attribute::getWithDereferenceableBytes (c, bytes);
}

} // namespace

/// Allocates an array of length elements and answers it. array must be a
/// szarray class, and length an integer of any width.
///
/// The collector's array allocator serves the call where the collector has one,
/// and the runtime's array-new icall where it does not.
///
/// A negative length raises OverflowException, which this emits rather than
/// leaving to either allocator. The two disagree above MONO_ARRAY_MAX_INDEX:
/// the allocator refuses with OutOfMemoryException and the icall with
/// OverflowException. A caller that can present such a length must test it
/// first.
llvm::Expected<llvm::Value *>
MethodLLVMEmitter::emit_vector_alloc (MonoIrBuilder &builder, MonoClass *array,
                                      llvm::Value *length)
{
	// mono_gc_get_managed_array_allocator () tests the rank alone. AllocVector
	// sizes the object as a header plus the elements, so a rank-1 array that
	// carries bounds comes out short.
	if (m_class_get_byval_arg (array)->type != MONO_TYPE_SZARRAY)
		llvm::reportFatalInternalError ("emit_vector_alloc needs a szarray class");

	llvm::Expected<llvm::Value *> vtable = class_operand (builder, array, "mono_vtable_");

	if (!vtable)
		return vtable.takeError ();

	/*
	 * The allocator raises this itself, and that is not enough. The `allockind`
	 * on `mono.alloc.vector` lets LLVM erase an allocation nothing reads, which
	 * takes the raise away with it: `newobj int32[]::.ctor(-1)` under a `pop`
	 * then answers an array instead of OverflowException. The test has to sit
	 * outside the call that the attribute makes erasable.
	 */
	emit_cond_exception (
		builder,
		builder.CreateICmpSLT (length, llvm::ConstantInt::get (length->getType (), 0)),
		"OverflowException");

	MonoMethod *allocator = mono_gc_get_managed_array_allocator (array);
	llvm::Function *serves = nullptr;

	if (allocator != nullptr) {
		llvm::Expected<llvm::Function *> fast = create_method_decl (allocator);

		if (!fast)
			return fast.takeError ();

		(*fast)->addRetAttr (llvm::Attribute::NoAlias);
		// The allocator raises OutOfMemoryException instead of answering null.
		(*fast)->addRetAttr (llvm::Attribute::NonNull);
		serves = *fast;
	} else {
		llvm::Expected<llvm::Function *> slow =
			icall_wrapper_decl (MONO_JIT_ICALL_ves_icall_array_new_specific);

		if (!slow)
			return slow.takeError ();

		(*slow)->addRetAttr (llvm::Attribute::NoAlias);
		serves = *slow;
	}

	// The count is a signed native int: the allocator tells a negative length
	// from an oversized one by its sign. The icall behind it takes an int32,
	// and the lowering narrows the operand for that one.
	llvm::Type *native = builder.getIntNTy (TARGET_SIZEOF_VOID_P * 8);
	llvm::Value *created = emit_protected_call (
		builder,
		alloc_func_decl (*module, AllocShape::vector, !allocation_is_observable (array)),
		{*vtable, builder.CreateSExtOrTrunc (length, native), serves});

	/*
	 * Both branches make an array of this class and no other. A proxy stands in
	 * for no array, so this needs none of the guard emit_object_alloc () keeps.
	 *
	 * The mark is what lets fold_type_tests () answer a type test on a fresh
	 * array. fold_object_vtables () reads it as well, which takes an interface
	 * dispatch on one down to a direct call.
	 */
	if (auto *made = llvm::dyn_cast<llvm::Instruction> (created))
		mark_exact_class (*made, array);

	// Only the fast branch raises rather than answering null, so the icall's
	// answer needs the `_or_null` form.
	if (auto *site = llvm::dyn_cast<llvm::CallBase> (created))
		site->addRetAttr (array_extent (allocator == nullptr, array, length));

	return created;
}

/*
 * III.4.20  newarr - create a zero-based, one-dimensional array
 *
 *   Format     Assembly Format   Description
 *   8D <T>     newarr etype      Create a new array with elements of type etype.
 *
 * Stack Transition:
 *
 *   ..., numElems -> ..., array
 *
 * Description:
 *
 *   The newarr instruction pushes a reference to a new zero-based, one-dimensional
 *   array whose elements are of type etype, a metadata token (a typeref, typedef or
 *   typespec; see Partition II). numElems (of type native int or int32) specifies the
 *   number of elements in the array. Valid array indexes are 0 <= index < numElems. The
 *   elements of an array can be any type, including value types.
 *
 *   Zero-based, one-dimensional arrays of numbers are created using a metadata token
 *   referencing the appropriate value type (System.Int32, etc.). Elements of the array
 *   are initialized to 0 of the appropriate type.
 *
 *   One-dimensional arrays that aren't zero-based and multidimensional arrays are
 *   created using newobj rather than newarr. More commonly, they are created using the
 *   methods of System.Array class in the Base Framework.
 *
 * Exceptions:
 *
 *   System.OutOfMemoryException is thrown if there is insufficient memory to satisfy
 *   the request.
 *
 *   System.OverflowException is thrown if numElems is < 0.
 */
llvm::Error
MethodLLVMEmitter::emit_newarr (MonoIrBuilder &builder, uint32_t token)
{
	if (stack.empty ())
		return unbalanced_stack (1);

	llvm::Expected<MonoType *> element = element_type_from_token (token);
	if (!element)
		return element.takeError ();

	StackValue count = get_stack (0);
	StackType count_type = stack_type (count.type);

	/* int64 is mini's 64-bit leniency. The narrowing below checks that it fits. */
	if (count_type != Int32 && count_type != NativeInt && count_type != Int64)
		return invalid_il (llvm::Twine ("an array length cannot be operand type ")
		                   + describe (count.type, count_type));

	llvm::Type *native = builder.getIntNTy (TARGET_SIZEOF_VOID_P * 8);
	llvm::Value *length = count.value;

	if (length->getType ()->isPointerTy ())
		length = builder.CreatePtrToInt (length, native);

	/*
	 * The two allocators disagree about which exception an oversized length
	 * draws, so this settles the answer here. It also keeps a native-int length
	 * off the icall's int32 count, where 2^32 truncates to a legal zero and
	 * allocates an empty array instead of raising anything.
	 *
	 * emit_vector_alloc () raises the negative-length answer.
	 */
	if (length->getType ()->getIntegerBitWidth () > 32) {
		llvm::Value *narrowed = builder.CreateTrunc (length, builder.getInt32Ty ());

		emit_cond_exception (
			builder,
			builder.CreateICmpNE (builder.CreateSExt (narrowed,
		                                                  length->getType ()),
		                              length),
			"OverflowException");
	}

	MonoClass *array =
		mono_class_create_array (mono_class_from_mono_type_internal (*element), 1);
	llvm::Expected<llvm::Value *> created = emit_vector_alloc (builder, array, length);

	if (!created)
		return created.takeError ();

	pop_stack (1);

	// The array holds the element class this opcode named, so a store into it
	// while it is still this value needs no covariance test.
	allocated_here.insert (*created);
	push_stack (*created, m_class_get_byval_arg (array));
	return llvm::Error::success ();
}

/*
 * The three emitters below answer System.Array's shape accessors from the
 * object instead of from an icall. Array.Copy reads the rank, the lower bound
 * and the length of both arrays before it moves one element, which is six of
 * these sites in one method.
 *
 * The first two read a field of the receiver, so each one raises
 * NullReferenceException on a null receiver. The third leaves the read to
 * the array shape lowering, which owes the same exception.
 */

/// Reads the rank out of the array's vtable, which is what Array.Rank answers.
llvm::Error
MethodLLVMEmitter::emit_array_rank (MonoIrBuilder &builder)
{
	if (stack.empty ())
		return unbalanced_stack (1);

	StackValue array = get_stack (0);

	if (stack_type (array.type) != ObjectRef)
		return invalid_il (llvm::Twine ("an array was expected, not operand type ")
		                   + describe (array.type, stack_type (array.type)));

	emit_null_check (builder, array.value);

	llvm::Value *rank = builder.CreateCall (
		vtable_rank_decl (*module), { load_vtable (builder, array.value) }, "rank");

	pop_stack (1);
	push_stack (builder.CreateZExt (rank, builder.getInt32Ty ()), mono_get_int32_type ());
	return llvm::Error::success ();
}

/// Reads MonoArray.max_length, which is what Array.Length answers.
///
/// That field holds the element count over every dimension, so one load
/// answers for an array of any shape.
llvm::Error
MethodLLVMEmitter::emit_array_total_length (MonoIrBuilder &builder)
{
	if (stack.empty ())
		return unbalanced_stack (1);

	llvm::Expected<llvm::Value *> length = array_length (builder, get_stack (0));

	if (!length)
		return length.takeError ();

	pop_stack (1);
	push_stack (builder.CreateZExtOrTrunc (*length, builder.getInt32Ty ()),
	            mono_get_int32_type ());
	return llvm::Error::success ();
}

/// The symbolic dimension call the array shape passes expand, for a site that asks
/// Array.GetLength () or Array.GetLowerBound () for one dimension.
///
/// The expansion answers dimension zero from the object and leaves any other
/// dimension on the accessor. Which one a site is, is read in the pass rather
/// than here, because a caller's dimension can arrive through a forwarded
/// parameter. Array.GetUpperBound () is one such forwarder, and the constant
/// reaches the accessor's body only once an inliner has folded it in.
///
/// The pass reads MonoArray itself. What the declaration carries is which
/// accessor this is and the method a declined site falls back to, and the site
/// carries the token of the exception a null array raises.
llvm::Error
MethodLLVMEmitter::emit_array_dimension (MonoIrBuilder &builder, MonoMethod *accessor,
                                         bool lower_bound)
{
	if (stack.size () < 2)
		return unbalanced_stack (2);

	StackValue array = get_stack (1);

	if (stack_type (array.type) != ObjectRef)
		return invalid_il (llvm::Twine ("an array was expected, not operand type ")
		                   + describe (array.type, stack_type (array.type)));

	// The accessor is an internal call, so what a declined site calls is the
	// marshalling wrapper the runtime publishes it as.
	MonoMethod *wrapper = icall_wrapper_target (accessor);
	llvm::Expected<llvm::Function *> target = create_method_decl (wrapper);

	if (!target)
		return target.takeError ();

	llvm::Expected<std::vector<llvm::Value *>> args =
		pop_call_arguments (builder, mono_method_signature_internal (wrapper));

	if (!args)
		return args.takeError ();

	llvm::StringRef kind = lower_bound ? array_shape_lower_bound : array_shape_length;
	// One declaration per accessor in the module, the way create_method_decl ()
	// keeps one per method.
	std::string name =
		(llvm::Twine (array_shape_prefix) + kind + "." + (*target)->getName ()).str ();
	llvm::Function *decl = module->getFunction (name);

	if (decl == nullptr) {
		llvm::FunctionType *shape = (*target)->getFunctionType ();
		std::vector<llvm::Type *> params (shape->param_begin (), shape->param_end ());

		// The accessor's own arguments, and the exception token behind them.
		params.push_back (builder.getInt32Ty ());
		decl = llvm::Function::Create (
			llvm::FunctionType::get (shape->getReturnType (), params, false),
			llvm::GlobalValue::ExternalLinkage, name, module);
		decl->addFnAttr (llvm::Attribute::get (context (), array_shape_attribute, kind));
		decl->addFnAttr (llvm::Attribute::get (context (), array_shape_target_attribute,
		                                       (*target)->getName ()));
	}

	MonoClass *nre = mono_class_load_from_name (mono_get_corlib (), "System",
	                                            "NullReferenceException");

	args->push_back (
		builder.getInt32 (m_class_get_type_token (nre) - MONO_TOKEN_TYPE_DEF));

	llvm::Value *result = emit_protected_call (builder, decl, *args);

	pop_stack (2);
	push_stack (result, mono_get_int32_type ());
	return llvm::Error::success ();
}

} // namespace mono

#include "method-to-llvm.hpp"
#include "runtime-error.hpp"
#include "mono/metadata/abi-details.h"
#include "mono/metadata/class-init.h"
#include "mono/metadata/class-internals.h"
#include "mono/metadata/metadata.h"
#include "mono/metadata/object-internals.h"
#include "mono/metadata/opcodes.h"
#include <llvm/IR/Attributes.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Type.h>

namespace mono {

/// The built-in type the suffixed forms of ldelem, stelem, ldind and stind carry in
/// the opcode.
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

/// The element type TOKEN names, for the two forms that carry one.
llvm::Expected<MonoType *>
MethodLLVMEmitter::element_type_from_token (uint32_t token)
{
	MonoClass *klass = mini_get_class (method, token, mono_method_get_context (method));

	if (klass == nullptr)
		return invalid_il (llvm::Twine ("token 0x") + llvm::Twine::utohexstr (token)
		                   + " does not name a type");

	return m_class_get_byval_arg (klass);
}

/// The number of elements in the array on top of the stack.
///
/// A length is read as a native unsigned int, which is what ldlen pushes and what the
/// bounds check below compares against.
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

	/* A scalar typedef, so its own size is the layout - no ABI table involved. */
	constexpr unsigned bytes = sizeof (mono_array_size_t);

	return builder.CreateAlignedLoad (builder.getIntNTy (bytes * 8), slot, llvm::Align (bytes));
}

/// Where element INDEX of ARRAY lives, having established that it is there.
///
/// The bounds test is a single unsigned comparison: a negative index read as unsigned
/// is enormous, so one `uge` rejects both ends at once, which is why the index is
/// zero-extended here rather than sign-extended.
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

	llvm::Expected<llvm::Value *> address =
		element_address (builder, get_stack (1), get_stack (0), *element);
	if (!address)
		return address.takeError ();

	MonoClass *klass = mono_class_from_mono_type_internal (*element);

	pop_stack (2);
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

	llvm::Expected<llvm::Type *> type = convert_type (element);
	if (!type)
		return type.takeError ();

	llvm::Expected<llvm::Value *> address =
		element_address (builder, get_stack (1), get_stack (0), element);
	if (!address)
		return address.takeError ();

	llvm::Value *value = builder.CreateAlignedLoad (*type, *address, type_alignment (element));

	pop_stack (2);
	push_stack (widen_to_stack (builder, value, element), stack_slot_type (element));
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

	/*
	 * The element type is the opcode's, but what the array actually holds is only
	 * known at run time, so storing a reference has to ask before it writes - that
	 * is the ArrayTypeMismatchException the spec lists. The check leaves it
	 * pending, so it goes through the wrapper whose check throws it.
	 */
	if (mini_type_is_reference (element)) {
		llvm::Expected<llvm::Function *> check =
			icall_wrapper_decl (MONO_JIT_ICALL_mono_helper_stelem_ref_check);

		if (!check)
			return check.takeError ();
		emit_protected_call (builder, *check,
		                     adapt_to_callee (builder, *check,
		                                      {array.value, *value}));
	}

	llvm::Expected<llvm::Value *> address =
		element_address (builder, array, get_stack (1), element);
	if (!address)
		return address.takeError ();

	pop_stack (3);
	emit_memory_store (builder, *value, *address, element);
	return llvm::Error::success ();
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

	if (count_type != Int32 && count_type != NativeInt)
		return invalid_il (llvm::Twine ("an array length cannot be operand type ")
		                   + describe (count.type, count_type));

	llvm::Type *native = builder.getIntNTy (TARGET_SIZEOF_VOID_P * 8);
	llvm::Value *length = count.value;

	if (length->getType ()->isPointerTy ())
		length = builder.CreatePtrToInt (length, native);

	/*
	 * The allocator's count is unsigned, so a negative length would reach it as an
	 * enormous one and come back as OutOfMemory. The spec asks for OverflowException,
	 * which means asking before the sign is lost - and the allocator takes an int32,
	 * so a native-int length that does not survive the narrowing overflowed too.
	 */
	emit_cond_exception (
		builder,
		builder.CreateICmpSLT (length, llvm::ConstantInt::get (length->getType (), 0)),
		"OverflowException");

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

	/*
	 * Through the allocator's wrapper: it reports a failed allocation as a
	 * pending OutOfMemoryException, which only the wrapper's check throws. The
	 * result aliases nothing older than the call and arrives zeroed, and
	 * allockind lets an array nothing observes be elided outright - that is a
	 * real choice: an allocation whose failure nothing could observe did not
	 * need to happen. Deliberately not nounwind.
	 */
	llvm::Expected<llvm::Function *> allocate =
		icall_wrapper_decl (MONO_JIT_ICALL_ves_icall_array_new_specific);

	if (!allocate)
		return allocate.takeError ();

	{
		llvm::AttrBuilder allocator (context ());

		allocator.addAllocKindAttr (llvm::AllocFnKind::Alloc
		                            | llvm::AllocFnKind::Zeroed);
		allocator.addAttribute ("alloc-family", "mono_gc");
		(*allocate)->addRetAttr (llvm::Attribute::NoAlias);
		(*allocate)->addFnAttrs (allocator);
	}

	llvm::Value *created = emit_protected_call (
		builder, *allocate,
		adapt_to_callee (builder, *allocate,
	                         {class_symbol (array, "mono_vtable_"),
	                          builder.CreateSExtOrTrunc (length, builder.getInt32Ty ())}));

	pop_stack (1);
	push_stack (created, m_class_get_byval_arg (array));
	return llvm::Error::success ();
}

} // namespace mono

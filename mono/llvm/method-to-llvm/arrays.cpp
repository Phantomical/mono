#include "method-to-llvm.hpp"
#include "runtime-error.hpp"
#include "../passes/array-address.hpp"
#include "mono/metadata/abi-details.h"
#include "mono/metadata/class-init.h"
#include "mono/metadata/class-internals.h"
#include "mono/metadata/metadata.h"
#include "mono/metadata/object-internals.h"
#include "mono/metadata/opcodes.h"
#include "mono/metadata/tokentype.h"
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

/// The class the token names, resolved against this method's generic context.
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

/// The element type the token names.
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

/// The number of elements in the array on top of the stack.
///
/// The length is read as a native unsigned int. ldlen pushes that width, and
/// the bounds check below compares against it.
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

	return builder.CreateAlignedLoad (builder.getIntNTy (bytes * 8), slot, llvm::Align (bytes));
}

/// Where element index of array lives, after checking that the element exists.
///
/// The bounds test is a single unsigned comparison. A negative index read as
/// unsigned becomes enormous, so one `uge` check rejects both ends at once.
/// That is why the index is zero-extended here instead of sign-extended.
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

/// Throw ArrayTypeMismatchException unless array is exactly an instance of array_class.
///
/// The check compares vtables for exact equality, not by assignability.
/// Covariance lets a string[] arrive where an object[] is expected. An
/// address into it, typed at the wrong element, must be refused before
/// anything writes through it.
void
MethodLLVMEmitter::emit_array_type_check (MonoIrBuilder &builder, llvm::Value *array,
                                          MonoClass *array_class)
{
	emit_null_check (builder, array);

	llvm::Value *slot = builder.CreateGEP (
		builder.getInt8Ty (), array,
		builder.getInt32 (MONO_STRUCT_OFFSET (MonoObject, vtable)));
	llvm::Value *vtable = builder.CreateAlignedLoad (
		llvm::PointerType::get (context (), 0), slot,
		llvm::Align (TARGET_SIZEOF_VOID_P));

	emit_cond_exception (
		builder,
		builder.CreateICmpNE (vtable, class_symbol (array_class, "mono_vtable_")),
		"ArrayTypeMismatchException");
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
	 * Wrappers are deliberately lax about the element type they name. Mini's
	 * rule limits this exactness check to ordinary IL.
	 */
	if (!m_class_is_valuetype (klass) && method->wrapper_type == MONO_WRAPPER_NONE
	    && !prefixes.readonly_)
		emit_array_type_check (builder, get_stack (1).value,
		                       mono_class_create_array (klass, 1));

	llvm::Expected<llvm::Value *> address =
		element_address (builder, get_stack (1), get_stack (0), *element);
	if (!address)
		return address.takeError ();

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

	llvm::Expected<llvm::Value *> address =
		element_address (builder, get_stack (1), get_stack (0), element);
	if (!address)
		return address.takeError ();

	pop_stack (2);
	return push_from_location (builder, *address, element);
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
	 * The element type here is the opcode's, but what the array holds is
	 * known only at run time. Storing a reference must ask before
	 * it writes - that check is the ArrayTypeMismatchException the spec
	 * lists. The check leaves the exception pending, and the wrapper call
	 * that follows is what throws it.
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

/// The symbolic element-address call that ArrayAddressPass expands.
///
/// The expansion turns (array, idx...) into a pointer at the element, and
/// throws IndexOutOfRangeException when an index misses its dimension. Every
/// number the expansion needs travels on the declaration's attribute, which
/// keeps mono's layouts out of the pass.
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
		std::vector<llvm::Type *> params (1 + indices.size (), builder.getInt32Ty ());

		params[0] = ptr;
		decl = llvm::Function::Create (llvm::FunctionType::get (ptr, params, false),
		                               llvm::GlobalValue::ExternalLinkage, name,
		                               module);

		MonoClass *ioor = mono_class_load_from_name (mono_get_corlib (), "System",
		                                             "IndexOutOfRangeException");
		char spec[256];

		snprintf (spec, sizeof (spec),
		          "rank=%zu,size=%d,bounded=%d,token=%u,bounds=%d,maxlen=%d,"
		          "maxlen_bytes=%zu,vector=%d,stride=%zu,blen=%d,blen_bytes=%zu,"
		          "blb=%d,blb_bytes=%zu",
		          indices.size (), size, bounded ? 1 : 0,
		          m_class_get_type_token (ioor) - MONO_TOKEN_TYPE_DEF,
		          (int) MONO_STRUCT_OFFSET (MonoArray, bounds),
		          (int) MONO_STRUCT_OFFSET (MonoArray, max_length),
		          sizeof (mono_array_size_t),
		          (int) MONO_STRUCT_OFFSET (MonoArray, vector),
		          sizeof (MonoArrayBounds),
		          (int) MONO_STRUCT_OFFSET (MonoArrayBounds, length),
		          sizeof (mono_array_size_t),
		          (int) MONO_STRUCT_OFFSET (MonoArrayBounds, lower_bound),
		          sizeof (mono_array_lower_bound_t));
		decl->addFnAttr (llvm::Attribute::get (context (), array_address_attribute,
		                                       spec));
	}

	std::vector<llvm::Value *> args;

	args.reserve (1 + indices.size ());
	args.push_back (array);
	args.insert (args.end (), indices.begin (), indices.end ());
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

		/* The covariance question stelem asks before it writes. */
		if (mini_type_is_reference (element)) {
			llvm::Expected<llvm::Function *> check =
				icall_wrapper_decl (MONO_JIT_ICALL_mono_helper_stelem_ref_check);

			if (!check)
				return check.takeError ();
			emit_protected_call (builder, *check,
			                     adapt_to_callee (builder, *check,
			                                      {array.value, value}));
		}
	}

	if (what == "Address" && !m_class_is_valuetype (eclass) && !prefixes.readonly_)
		emit_array_type_check (builder, array.value, accessor->klass);

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
		emit_memory_store (builder, value, *address, element);
	} else if (what == "Get") {
		return push_from_location (builder, *address, element);
	} else {
		push_stack (*address, m_class_get_this_arg (eclass));
	}
	return llvm::Error::success ();
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

/// R Array.UnsafeMov<S,R> (S): the reinterpret mini performs as a plain move.
///
/// The IL body boxes S and unboxes it as R. The unbox type
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
	 * The allocator's count is unsigned. Without this check, a negative
	 * length reaches it as an enormous one and comes back as OutOfMemory.
	 * The spec asks for
	 * OverflowException instead, so the check must run before the sign is
	 * lost. The allocator also takes an int32, so a native-int length that
	 * does not survive the narrowing overflowed too.
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
	 * This call goes through the allocator's wrapper. A failed allocation
	 * reports a pending OutOfMemoryException, and only the wrapper's check
	 * throws it, so the call unwinds like any other.
	 *
	 * NoAlias is the whole claim on the return value: the result aliases
	 * nothing older than the call. The header - vtable and length - comes
	 * back already initialized rather than zeroed.
	 */
	llvm::Expected<llvm::Function *> allocate =
		icall_wrapper_decl (MONO_JIT_ICALL_ves_icall_array_new_specific);

	if (!allocate)
		return allocate.takeError ();

	(*allocate)->addRetAttr (llvm::Attribute::NoAlias);

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

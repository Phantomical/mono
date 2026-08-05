#include "method-to-llvm.hpp"
#include "runtime-error.hpp"
#include "mono/metadata/abi-details.h"
#include "mono/metadata/class-abi-details.h"
#include "mono/metadata/class-internals.h"
#include "mono/metadata/exception-internals.h"
#include "mono/metadata/metadata.h"
#include "mono/metadata/object-internals.h"
#include <llvm/IR/Attributes.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Type.h>

namespace mono {

/// The runtime's allocator for a plain object of a known class, by its vtable -
/// through its wrapper, whose check turns a pending OutOfMemoryException into a
/// throw.
///
/// The allocation attributes: NoAlias and nothing more - an allockind would
/// let LLVM delete an unused allocation whose failure is still a catchable
/// OutOfMemoryException, and a Zeroed claim would fold loads of the
/// initialized header to zero. Deliberately not nounwind - it throws.
llvm::Expected<llvm::Function *>
MethodLLVMEmitter::object_new_decl ()
{
	llvm::Expected<llvm::Function *> wrapper =
		icall_wrapper_decl (MONO_JIT_ICALL_ves_icall_object_new_specific);

	if (!wrapper)
		return wrapper;

	(*wrapper)->addRetAttr (llvm::Attribute::NoAlias);
	return wrapper;
}

/// The address of the value held inside OBJ, after throwing InvalidCastException
/// unless OBJ is a boxed KLASS.
///
/// The test mirrors mini's. The object must not be an array - an int[]'s element class
/// is int, so the rank byte is what tells an array of T from a boxed T - and its
/// class's element class must be KLASS's. Element class rather than the class itself
/// so that a boxed enum unboxes as its underlying type and the other way around.
llvm::Value *
MethodLLVMEmitter::unbox_payload (MonoIrBuilder &builder, llvm::Value *obj, MonoClass *klass)
{
	llvm::Type *ptr = llvm::PointerType::get (context (), 0);

	emit_null_check (builder, obj);

	llvm::Value *vtable = builder.CreateAlignedLoad (
		ptr,
		builder.CreateGEP (builder.getInt8Ty (), obj,
	                           builder.getInt32 (MONO_STRUCT_OFFSET (MonoObject, vtable))),
		llvm::Align (TARGET_SIZEOF_VOID_P));
	llvm::Value *rank = builder.CreateLoad (
		builder.getInt8Ty (),
		builder.CreateGEP (builder.getInt8Ty (), vtable,
	                           builder.getInt32 (MONO_STRUCT_OFFSET (MonoVTable, rank))));

	emit_cond_exception (builder, builder.CreateICmpNE (rank, builder.getInt8 (0)),
	                     "InvalidCastException");

	llvm::Value *cls = builder.CreateAlignedLoad (
		ptr,
		builder.CreateGEP (builder.getInt8Ty (), vtable,
	                           builder.getInt32 (MONO_STRUCT_OFFSET (MonoVTable, klass))),
		llvm::Align (TARGET_SIZEOF_VOID_P));
	llvm::Value *element = builder.CreateAlignedLoad (
		ptr,
		builder.CreateGEP (builder.getInt8Ty (), cls,
	                           builder.getInt32 (static_cast<int32_t> (
					   m_class_offsetof_element_class ()))),
		llvm::Align (TARGET_SIZEOF_VOID_P));

	emit_cond_exception (
		builder,
		builder.CreateICmpNE (
			element, class_symbol (m_class_get_element_class (klass), "mono_class_")),
		"InvalidCastException");

	return builder.CreateGEP (builder.getInt8Ty (), obj,
	                          builder.getInt32 (MONO_ABI_SIZEOF (MonoObject)));
}

/// The corlib helper that unboxes an object into a Nullable<T>.
///
/// UnboxExact when T is an enum: Unbox's (T) cast also accepts a boxed underlying
/// type - enum and underlying interconvert under unbox.any - and a Nullable<enum>
/// must only accept a boxed enum.
static const char *
nullable_unbox_helper (MonoClass *klass)
{
	bool exact = m_class_is_enumtype (mono_class_get_nullable_param_internal (klass));
	return exact ? "UnboxExact" : "Unbox";
}

/// Emit a call to the one-argument Nullable<T> helper NAME - Box, Unbox or
/// UnboxExact - consuming its argument from the stack and pushing its result.
///
/// Boxing and unboxing a nullable branch on HasValue against a null reference, and
/// the corlib helpers are that branch, written once as ordinary IL.
llvm::Error
MethodLLVMEmitter::call_nullable_helper (MonoIrBuilder &builder, MonoClass *klass,
                                         const char *name)
{
	ERROR_DECL (find_error);
	MonoMethod *helper =
		mono_class_get_method_from_name_checked (klass, name, 1, 0, find_error);

	if (!is_ok (find_error))
		return runtime_error (find_error);
	if (helper == nullptr)
		return invalid_il (llvm::Twine ("Nullable has no ") + name + " helper");

	llvm::Expected<llvm::Function *> declaration = create_method_decl (helper);
	if (!declaration)
		return declaration.takeError ();

	MonoMethodSignature *sig = mono_method_signature_internal (helper);
	llvm::Expected<std::vector<llvm::Value *>> args = pop_call_arguments (builder, sig);
	if (!args)
		return args.takeError ();

	llvm::Value *result = emit_protected_call (builder, *declaration, *args);

	pop_stack (1);
	push_stack (widen_to_stack (builder, result, sig->ret), stack_slot_type (sig->ret));
	return llvm::Error::success ();
}

/// Box VALUE, a Nullable<T>, into null or a boxed T.
///
/// Unlike every other value type, the boxed form of a Nullable<T> is not a boxed
/// Nullable<T>: it is a boxed T, or a null reference when the value has none
/// (III.4.1). Nullable<T>:Box is that branch, written once as ordinary IL, and
/// every site that boxes a nullable has to go through it - the plain allocate-and-copy
/// would manufacture an object whose type the program can observe and never expects.
llvm::Expected<llvm::Value *>
MethodLLVMEmitter::box_nullable (MonoIrBuilder &builder, MonoClass *klass, StackValue value)
{
	ERROR_DECL (find_error);
	MonoMethod *helper =
		mono_class_get_method_from_name_checked (klass, "Box", 1, 0, find_error);

	if (!is_ok (find_error))
		return runtime_error (find_error);
	if (helper == nullptr)
		return invalid_il ("Nullable has no Box helper");

	llvm::Expected<llvm::Function *> declaration = create_method_decl (helper);
	if (!declaration)
		return declaration.takeError ();

	MonoMethodSignature *sig = mono_method_signature_internal (helper);
	llvm::Expected<llvm::Value *> argument =
		coerce_to_argument (builder, value, sig->params[0]);

	if (!argument)
		return argument.takeError ();

	return emit_protected_call (builder, *declaration, {*argument});
}

/*
 * III.4.1  box - convert a boxable value to its boxed form
 *
 *   Format     Assembly Format   Description
 *   8C <T>     box typeTok       Convert a boxable value to its boxed form
 *
 * Stack Transition:
 *
 *   ..., val -> ..., obj
 *
 * Description:
 *
 *   If typeTok is a value type, the box instruction converts val to its boxed form.
 *   When typeTok is a non-nullable type (§I.8.2.4), this is done by creating a new
 *   object and copying the data from val into the newly allocated object. If it is a
 *   nullable type, this is done by inspecting val's HasValue property; if it is false,
 *   a null reference is pushed onto the stack; otherwise, the result of boxing val's
 *   Value property is pushed onto the stack.
 *
 *   If typeTok is a reference type, the box instruction does returns val unchanged as
 *   obj.
 *
 *   If typeTok is a generic parameter, the behavior of box instruction depends on the
 *   actual type at runtime. If this type is a value type it is boxed as above, if it
 *   is a reference type then val is not changed. However the type tracked by
 *   verification is always "boxed" typeTok for generic parameters, regardless of
 *   whether the actual type at runtime is a value or reference type.
 *
 *   typeTok is a metadata token (a typedef, typeref, or typespec) indicating the type
 *   of val. typeTok can represent a value type, a reference type, or a generic
 *   parameter.
 *
 * Exceptions:
 *
 *   System.OutOfMemoryException is thrown if there is insufficient memory to satisfy
 *   the request.
 *
 *   System.TypeLoadException is thrown if typeTok cannot be found. (This is typically
 *   detected when CIL is converted to native code rather than at runtime.)
 *
 * Correctness:
 *
 *   typeTok shall be a valid typedef, typeref, or typespec metadata token. The type
 *   operand typeTok shall represent a boxable type (§I.8.2.4).
 *
 * Verifiability:
 *
 *   The top-of-stack shall be verifier-assignable-to the type represented by typeTok.
 *   When typeTok represents a non-nullable value type or a generic parameter, the
 *   resulting type is "boxed" typeTok; when typeTok is Nullable<T>, the resulting
 *   type is "boxed" T.
 */
llvm::Error
MethodLLVMEmitter::emit_box (MonoIrBuilder &builder, uint32_t token)
{
	llvm::Expected<MonoType *> type = element_type_from_token (token);
	if (!type)
		return type.takeError ();

	MonoClass *klass = mono_class_from_mono_type_internal (*type);

	if (stack.empty ())
		return unbalanced_stack (1);

	if (mono_class_is_nullable (klass)) {
		llvm::Expected<llvm::Value *> obj = box_nullable (builder, klass, get_stack (0));

		if (!obj)
			return obj.takeError ();

		pop_stack (1);
		push_stack (*obj, mono_get_object_type ());
		return llvm::Error::success ();
	}

	/* A reference type's boxed form is itself, so there is nothing to do. */
	if (!m_class_is_valuetype (klass))
		return llvm::Error::success ();

	llvm::Expected<llvm::Value *> value = coerce_to_location (builder, get_stack (0), *type);
	if (!value)
		return value.takeError ();

	llvm::Expected<llvm::Value *> obj = box_value (builder, klass, *type, *value);

	if (!obj)
		return obj.takeError ();

	pop_stack (1);
	push_stack (*obj, mono_get_object_type ());
	return llvm::Error::success ();
}

/// Allocate KLASS's box and copy VALUE, already in its location type, into the
/// payload, returning the new object.
llvm::Expected<llvm::Value *>
MethodLLVMEmitter::box_value (MonoIrBuilder &builder, MonoClass *klass, MonoType *type,
                              llvm::Value *value)
{
	/*
	 * A byreflike value may hold managed pointers into a stack frame, so a box
	 * of one would outlive what it points at. Metadata refuses the type as a
	 * generic argument; the box opcode is the remaining way to smuggle one onto
	 * the heap, and it is refused here.
	 */
	if (m_class_is_byreflike (klass)) {
		ERROR_DECL (error);

		mono_error_set_bad_image (error, m_class_get_image (method->klass),
		                          "Cannot box IsByRefLike type '%s.%s'",
		                          m_class_get_name_space (klass), m_class_get_name (klass));
		return runtime_error (error);
	}

	llvm::Expected<llvm::Function *> allocate = object_new_decl ();

	if (!allocate)
		return allocate.takeError ();

	llvm::Value *obj = emit_protected_call (
		builder, *allocate,
		adapt_to_callee (builder, *allocate,
	                         {class_symbol (klass, "mono_vtable_")}));
	llvm::Value *payload = builder.CreateGEP (builder.getInt8Ty (), obj,
	                                          builder.getInt32 (MONO_ABI_SIZEOF (MonoObject)));

	/* The same copy stobj makes: through the collector if references are inside. */
	if (m_class_has_references (klass)) {
		MonoIrBuilder entry (entry_block, entry_block->begin ());
		llvm::AllocaInst *temp = entry.CreateAlloca (value->getType ());

		temp->setAlignment (type_alignment (type));
		builder.CreateAlignedStore (value, temp, temp->getAlign ());
		builder.CreateCall (value_copy_decl (), {payload, temp, builder.getInt32 (1),
		                                         class_symbol (klass, "mono_class_")});
	} else {
		builder.CreateAlignedStore (value, payload, type_alignment (type));
	}

	return obj;
}

/*
 * III.4.32  unbox - convert boxed value type to its raw form
 *
 *   Format     Assembly Format   Description
 *   79 <T>     unbox valuetype   Extract a value-type from obj, its boxed
 *                                representation.
 *
 * Stack Transition:
 *
 *   ..., obj -> ..., valueTypePtr
 *
 * Description:
 *
 *   A value type has two separate representations (see Partition I) within the CLI:
 *
 *     - A 'raw' form used when a value type is embedded within another object.
 *
 *     - A 'boxed' form, where the data in the value type is wrapped (boxed) into an
 *       object, so it can exist as an independent entity.
 *
 *   The unbox instruction converts obj (of type O), the boxed representation of a
 *   value type, to valueTypePtr (a controlled-mutability managed pointer
 *   (§III.1.8.1.2.2), type &), its unboxed form. valuetype is a metadata token (a
 *   typeref, typedef or typespec). The type of valuetype contained within obj must be
 *   verifier-assignable-to valuetype.
 *
 *   Unlike box, which is required to make a copy of a value type for use in the
 *   object, unbox is not required to copy the value type from the object. Typically
 *   it simply computes the address of the value type that is already present inside
 *   of the boxed object.
 *
 * Exceptions:
 *
 *   System.InvalidCastException is thrown if obj is not a boxed value type, valuetype
 *   is a Nullable<T> and obj is not a boxed T, or if the type of the value contained
 *   in obj is not verifier-assignable-to (§III.1.8.1.2.3) valuetype.
 *
 *   System.NullReferenceException is thrown if obj is null and valuetype is a
 *   non-nullable value type (Partition I.8.2.4).
 *
 *   System.TypeLoadException is thrown if the class cannot be found. (This is
 *   typically detected when CIL is converted to native code rather than at runtime.)
 *
 * Correctness:
 *
 *   Correct CIL ensures that valueType is a typeref, typedef or typespec metadata
 *   token for some boxable value type, and that obj is always an object reference
 *   (i.e., of type O). If valuetype is the type Nullable<T>, the boxed instance shall
 *   be of type T.
 *
 * Verifiability:
 *
 *   Verification requires that the type of valuetype contained within obj must be
 *   verifier-assignable-to valuetype
 */
llvm::Error
MethodLLVMEmitter::emit_unbox (MonoIrBuilder &builder, uint32_t token)
{
	llvm::Expected<MonoType *> type = element_type_from_token (token);
	if (!type)
		return type.takeError ();

	MonoClass *klass = mono_class_from_mono_type_internal (*type);

	/*
	 * A nullable has no interior pointer to hand out - the boxed form is a plain T
	 * or null - so the Nullable<T> the helper manufactures is spilled and the
	 * pointer pushed is to the spill.
	 */
	if (mono_class_is_nullable (klass)) {
		if (llvm::Error error = call_nullable_helper (builder, klass,
		                                              nullable_unbox_helper (klass)))
			return error;

		StackValue value = get_stack (0);
		MonoIrBuilder entry (entry_block, entry_block->begin ());
		llvm::AllocaInst *temp = entry.CreateAlloca (value.value->getType ());

		temp->setAlignment (type_alignment (*type));
		builder.CreateAlignedStore (value.value, temp, temp->getAlign ());
		pop_stack (1);
		push_stack (temp, m_class_get_this_arg (klass));
		return llvm::Error::success ();
	}

	if (!m_class_is_valuetype (klass))
		return invalid_il ("unbox needs a value type");
	if (stack.empty ())
		return unbalanced_stack (1);

	StackValue obj = get_stack (0);
	StackType obj_type = stack_type (obj.type);

	if (obj_type != ObjectRef)
		return invalid_il (llvm::Twine ("unbox is not defined for operand type ")
		                   + describe (obj.type, obj_type));

	llvm::Value *payload = unbox_payload (builder, obj.value, klass);

	pop_stack (1);
	push_stack (payload, m_class_get_this_arg (klass));
	return llvm::Error::success ();
}

/*
 * III.4.33  unbox.any - convert boxed type to value
 *
 *   Format     Assembly Format     Description
 *   A5 <T>     unbox.any typeTok   Extract a value-type from obj, its boxed
 *                                  representation
 *
 * Stack Transition:
 *
 *   ..., obj -> ..., value or obj
 *
 * Description:
 *
 *   When applied to the boxed form of a value type, the unbox.any instruction
 *   extracts the value contained within obj (of type O). (It is equivalent to unbox
 *   followed by ldobj.) When applied to a reference type, the unbox.any instruction
 *   has the same effect as castclass typeTok.
 *
 *   If typeTok is a GenericParam, the runtime behavior is determined by the actual
 *   instantiation of that parameter.
 *
 * Exceptions:
 *
 *   System.InvalidCastException is thrown if obj is not a boxed value type or a
 *   reference type, typeTok is Nullable<T> and obj is not a boxed T, or if the type
 *   of the value contained in obj is not verifier-assignable-to (§III.1.8.1.2.3)
 *   typeTok.
 *
 *   System.NullReferenceException is thrown if obj is null and typeTok is a
 *   non-nullable value type (Partition I.8.2.4).
 *
 * Correctness:
 *
 *   obj shall be of reference type and typeTok shall be a boxable type.
 *
 * Verifiability:
 *
 *   Verification tracks the type of value or obj as the intermediate type of typeTok.
 */
llvm::Error
MethodLLVMEmitter::emit_unbox_any (MonoIrBuilder &builder, uint32_t token)
{
	llvm::Expected<MonoType *> type = element_type_from_token (token);
	if (!type)
		return type.takeError ();

	MonoClass *klass = mono_class_from_mono_type_internal (*type);

	if (mono_class_is_nullable (klass))
		return call_nullable_helper (builder, klass, nullable_unbox_helper (klass));
	/* The spec's reading for a reference type: this instruction is castclass. */
	if (!m_class_is_valuetype (klass))
		return emit_castclass (builder, token);
	if (stack.empty ())
		return unbalanced_stack (1);

	StackValue obj = get_stack (0);
	StackType obj_type = stack_type (obj.type);

	if (obj_type != ObjectRef)
		return invalid_il (llvm::Twine ("unbox.any is not defined for operand type ")
		                   + describe (obj.type, obj_type));

	llvm::Value *payload = unbox_payload (builder, obj.value, klass);

	/* The spec's own reading: unbox, then load through the pointer it pushed. */
	pop_stack (1);
	push_stack (payload, m_class_get_this_arg (klass));
	return emit_ldind (builder, *type);
}

} // namespace mono

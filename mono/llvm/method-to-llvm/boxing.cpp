#include "method-to-llvm.hpp"
#include "operand-class.hpp"
#include "runtime-error.hpp"
#include "../passes/vtable-func.hpp"
#include "mono/metadata/abi-details.h"
#include "mono/metadata/class-abi-details.h"
#include "mono/metadata/class-internals.h"
#include "mono/metadata/exception-internals.h"
#include "mono/metadata/metadata.h"
#include "mono/metadata/object-internals.h"

// gc-internals.h declares C functions but does not mark them extern "C"
// itself, so this include must supply the wrap.
extern "C" {
#include "mono/metadata/gc-internals.h"
}

#include <llvm/IR/Attributes.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Type.h>
#include <llvm/Support/ErrorHandling.h>

#include <optional>

namespace mono {

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

llvm::Expected<llvm::Value *>
MethodLLVMEmitter::emit_object_alloc (MonoIrBuilder &builder, MonoClass *klass, bool for_box)
{
	llvm::Expected<llvm::Value *> vtable = class_operand (builder, klass, "mono_vtable_");

	if (!vtable)
		return vtable.takeError ();

	int32_t size = mono_class_instance_size (klass);
	MonoMethod *allocator = nullptr;
	llvm::Value *object = nullptr;

	// The caller handles a string constructor before it gets here.
	if (m_class_get_byval_arg (klass)->type == MONO_TYPE_STRING)
		llvm::reportFatalInternalError ("emit_object_alloc does not support strings");

	if (size != 0 && size < MONO_ABI_SIZEOF (MonoObject))
		llvm::reportFatalInternalError (
			"instance size is smaller than MonoObject");

	// size == 0 means that the class has no known layout. Managed allocators
	// need an actual size so we skip in that case.
	if (size != 0)
		allocator = mono_gc_get_managed_allocator (klass, for_box, TRUE);

	if (allocator != nullptr) {
		llvm::Expected<llvm::Function *> fast = create_method_decl (allocator);

		if (!fast)
			return fast.takeError ();

		(*fast)->addRetAttr (llvm::Attribute::NoAlias);
		// allockind lets LLVM erase an allocation that nothing uses, which is
		// what SROA leaves behind once it scalarizes a temporary object.
		// mono_gc_get_managed_allocator () keeps the classes an erased
		// allocation is observable on out of this branch: a finalizer,
		// MarshalByRefObject, weak fields and collect-before-allocs all send it
		// to object_new_decl () below.
		(*fast)->addFnAttr (
			llvm::Attribute::getWithAllocKind (context (), llvm::AllocFnKind::Alloc));
		// Argument 0 is the vtable. An allocator that takes a second argument
		// takes the instance size there.
		if ((*fast)->arg_size () == 2)
			(*fast)->addFnAttr (llvm::Attribute::getWithAllocSizeArgs (
				context (), 1, std::nullopt));
		// The allocator raises OutOfMemoryException instead of answering null.
		(*fast)->addRetAttr (llvm::Attribute::NonNull);
		object = emit_protected_call (
			builder, *fast,
			adapt_to_callee (
				builder, *fast,
				{*vtable, builder.getIntN (TARGET_SIZEOF_VOID_P * 8, size)}));
	} else {
		llvm::Expected<llvm::Function *> slow = object_new_decl ();

		if (!slow)
			return slow.takeError ();

		object = emit_protected_call (builder, *slow,
		                              adapt_to_callee (builder, *slow, {*vtable}));
	}

	/*
	 * The allocator wrote this word already, so this store is redundant. What it
	 * adds is the class of a fresh object, stated in the IR. Without it the
	 * optimizer sees an opaque pointer. A dispatch site then keeps its lookup,
	 * even where the allocation is in the same block.
	 *
	 * A class whose allocation can answer with a proxy gets no store, because
	 * what comes back then carries the proxy's vtable rather than this one.
	 */
	if (!allocation_can_be_a_proxy (klass)) {
		builder.CreateAlignedStore (
			*vtable,
			builder.CreateGEP (
				builder.getInt8Ty (), object,
				builder.getInt32 (MONO_STRUCT_OFFSET (MonoObject, vtable))),
			llvm::Align (TARGET_SIZEOF_VOID_P));

		// The same fact for a reader that has the object rather than its vtable,
		// which is what a type test on a fresh object has.
		if (auto *made = llvm::dyn_cast<llvm::Instruction> (object))
			mark_allocated_class (*made, klass);
	}

	return object;
}

/// Returns the address of the value held inside obj. Throws
/// InvalidCastException when obj is not a boxed klass.
///
/// A boxed enum unboxes as its underlying type, and a boxed underlying value
/// unboxes as the enum.
llvm::Value *
MethodLLVMEmitter::unbox_payload (MonoIrBuilder &builder, llvm::Value *obj, MonoClass *klass)
{
	llvm::Type *ptr = llvm::PointerType::get (context (), 0);

	emit_null_check (builder, obj);

	llvm::Value *vtable = load_vtable (builder, obj);
	// An array of T and a boxed T have the same element class, so the rank is
	// what tells them apart.
	llvm::Value *rank = builder.CreateCall (vtable_rank_decl (*module), { vtable });

	emit_cond_exception (builder, builder.CreateICmpNE (rank, builder.getInt8 (0)),
	                     "InvalidCastException");

	llvm::Value *cls = builder.CreateCall (vtable_klass_decl (*module), { vtable });
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

/// Names the corlib helper that unboxes an object into a Nullable<T>.
///
/// UnboxExact applies when T is an enum. Unbox's cast to T also accepts a
/// boxed underlying type, because enum and underlying type interconvert
/// under unbox.any. A Nullable<enum> must accept only a boxed enum.
static const char *
nullable_unbox_helper (MonoClass *klass)
{
	bool exact = m_class_is_enumtype (mono_class_get_nullable_param_internal (klass));
	return exact ? "UnboxExact" : "Unbox";
}

/// Boxing and unboxing a nullable both branch on HasValue against a null
/// reference. The corlib helpers are that branch, written once as ordinary
/// IL.
llvm::Error
MethodLLVMEmitter::call_nullable_helper (MonoIrBuilder &builder, MonoClass *klass, const char *name)
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
	return push_produced (builder, result, sig->ret);
}

/// Unlike every other value type, the boxed form of a Nullable<T> is not a
/// boxed Nullable<T> (III.4.1).
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

/// \param value  what coerce_to_location produced for a location of type.
llvm::Expected<llvm::Value *>
MethodLLVMEmitter::box_value (MonoIrBuilder &builder, MonoClass *klass, MonoType *type,
                              llvm::Value *value)
{
	// A byreflike value can hold managed pointers into a stack frame, so a box
	// of one can outlive what it points at. Metadata already refuses byreflike
	// as a generic argument. The box opcode is the remaining way to put one on
	// the heap, and this refuses it too.
	if (m_class_is_byreflike (klass)) {
		ERROR_DECL (error);

		mono_error_set_bad_image (error, m_class_get_image (method->klass),
		                          "Cannot box IsByRefLike type '%s.%s'",
		                          m_class_get_name_space (klass), m_class_get_name (klass));
		return runtime_error (error);
	}

	llvm::Expected<llvm::Value *> allocated = emit_object_alloc (builder, klass, true);

	if (!allocated)
		return allocated.takeError ();

	llvm::Value *obj = *allocated;
	llvm::Value *payload = builder.CreateGEP (builder.getInt8Ty (), obj,
	                                          builder.getInt32 (MONO_ABI_SIZEOF (MonoObject)));

	// This matches the copy stobj makes: through the collector when klass has
	// reference fields, and a plain copy otherwise.
	if (!held_in_memory (type)) {
		builder.CreateAlignedStore (value, payload, type_alignment (type));
	} else if (m_class_has_references (klass)) {
		llvm::Expected<llvm::Value *> cls = class_operand (builder, klass, "mono_class_");

		if (!cls)
			return cls.takeError ();

		builder.CreateCall (value_copy_decl (),
		                    {payload, value, builder.getInt32 (1), *cls});
	} else {
		copy_vtype (builder, payload, value, type, /*native=*/false);
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

	// A boxed nullable is a plain T, or null, so there is no Nullable<T>
	// inside the object whose address this can compute. The helper builds the
	// value instead, and spill_to_temporary () hands back its address either
	// way. III.4.32 does not require unbox to point into the object.
	if (mono_class_is_nullable (klass)) {
		if (llvm::Error error =
		            call_nullable_helper (builder, klass, nullable_unbox_helper (klass)))
			return error;

		llvm::Value *home = spill_to_temporary (builder, *type);

		pop_stack (1);
		push_stack (home, m_class_get_this_arg (klass));
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

	pop_stack (1);
	push_stack (payload, m_class_get_this_arg (klass));
	return emit_ldind (builder, *type);
}

} // namespace mono

#include "method-to-llvm.hpp"
#include "runtime-error.hpp"
#include "mono/metadata/abi-details.h"
#include "mono/metadata/class-internals.h"
#include "mono/metadata/debug-helpers.h"
#include "mono/metadata/loader.h"
#include "mono/metadata/metadata.h"
#include "mono/metadata/object-internals.h"
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Type.h>

namespace mono {

namespace {

/// The collector's hook for storing a reference through a pointer it does not otherwise
/// know about, which is the general form that also covers a field reached through the
/// interior of an object.
llvm::FunctionCallee
wbarrier_decl (llvm::Module *module)
{
	llvm::LLVMContext &ctx = module->getContext ();
	llvm::Type *ptr = llvm::PointerType::get (ctx, 0);

	return module->getOrInsertFunction ("mono_gc_wbarrier_generic_store_internal",
	                                    llvm::Type::getVoidTy (ctx), ptr, ptr);
}

} // namespace

/// The field TOKEN names, with its declaring class laid out so that its offset can be
/// asked for.
llvm::Expected<MonoClassField *>
MethodLLVMEmitter::resolve_field (uint32_t token)
{
	ERROR_DECL (metadata_error);
	MonoClass *klass = nullptr;
	MonoClassField *field =
		mono_field_from_token_checked (m_class_get_image (method->klass), token, &klass,
		                               mono_method_get_context (method), metadata_error);

	if (field == nullptr)
		return runtime_error (metadata_error);

	/*
	 * Laying the class out is what makes the offset readable; it settles metadata
	 * only, and is not the class initializer, which must never run here.
	 */
	mono_class_init_checked (field->parent, metadata_error);
	if (!is_ok (metadata_error))
		return runtime_error (metadata_error);

	if (mono_field_get_flags (field) & FIELD_ATTRIBUTE_STATIC) {
		char *name = mono_field_full_name (field);
		llvm::Error error =
			invalid_il (llvm::Twine ("a field instruction needs an instance field, but ")
			            + name + " is static");

		g_free (name);
		return std::move (error);
	}

	return field;
}

/// Where FIELD lives inside the object on top of the stack.
llvm::Expected<llvm::Value *>
MethodLLVMEmitter::field_address (MonoIrBuilder &builder, StackValue object,
                                  MonoClassField *field)
{
	StackType type = stack_type (object.type);

	if (type != ObjectRef && type != ManagedPtr && type != NativeInt)
		return invalid_il (llvm::Twine ("a field cannot be reached through operand type ")
		                   + describe (object.type, type));

	llvm::Value *base = object.value;

	/* A native int receiver is only a number until something dereferences it. */
	if (!base->getType ()->isPointerTy ())
		base = builder.CreateIntToPtr (base, llvm::PointerType::get (context (), 0));

	emit_null_check (builder, base);

	/*
	 * A field's recorded offset counts from the start of the MonoObject, so a value
	 * type - which is reached through its own address and carries no header - needs
	 * that header taken back off.
	 */
	int32_t offset = static_cast<int32_t> (m_field_get_offset (field));

	if (m_class_is_valuetype (field->parent))
		offset -= MONO_ABI_SIZEOF (MonoObject);

	return builder.CreateGEP (builder.getInt8Ty (), base, builder.getInt32 (offset));
}

/*
 * III.4.10  ldfld - load field of an object
 *
 *   Format     Assembly Format   Description
 *   7B <T>     ldfld field       Push the value of field of object (or value type) obj,
 *                                onto the stack.
 *
 * Stack Transition:
 *
 *   ..., obj -> ..., value
 *
 * Description:
 *
 *   The ldfld instruction pushes onto the stack the value of a field of obj. obj shall
 *   be an object (type O), a managed pointer (type &), an unmanaged pointer (type
 *   native int), or an instance of a value type. The use of an unmanaged pointer is not
 *   permitted in verifiable code. field is a metadata token (a fieldref or fielddef see
 *   Partition II) that shall refer to a field member. The return type is that
 *   associated with field. ldfld pops the object reference off the stack and pushes the
 *   value for the field in its place. The field can be either an instance field (in
 *   which case obj shall not be null) or a static field.
 *
 *   The ldfld instruction can be preceded by either or both of the unaligned. and
 *   volatile. prefixes.
 *
 *   If required field values are converted to the representation of their intermediate
 *   type (§I.8.7) when loaded onto the stack (§III.1.1.1).
 *
 *   [Note: That is field values that are smaller than 4 bytes, a boolean or a character
 *   are converted to 4 bytes by sign or zero-extension as appropriate. Floating-point
 *   values are converted to their native size (type F). end note]
 *
 * Exceptions:
 *
 *   System.FieldAccessException is thrown if field is not accessible.
 *
 *   System.MissingFieldException is thrown if field is not found in the metadata. This
 *   is typically checked when CIL is converted to native code, not at runtime.
 *
 *   System.NullReferenceException is thrown if obj is null and the field is not static.
 */
llvm::Error
MethodLLVMEmitter::emit_ldfld (MonoIrBuilder &builder, uint32_t token)
{
	if (stack.empty ())
		return unbalanced_stack (1);

	llvm::Expected<MonoClassField *> field = resolve_field (token);
	if (!field)
		return field.takeError ();

	MonoType *ftype = mono_field_get_type_internal (*field);
	llvm::Expected<llvm::Type *> type = convert_type (ftype);
	if (!type)
		return type.takeError ();

	llvm::Expected<llvm::Value *> address = field_address (builder, get_stack (0), *field);
	if (!address)
		return address.takeError ();

	llvm::Value *value = builder.CreateAlignedLoad (*type, *address, type_alignment (ftype));
	MonoType *pushed = ftype;

	/* The same I.8.7 widening a load out of a local or an argument gets. */
	if (!ftype->byref) {
		switch (mini_get_underlying_type (ftype)->type) {
		case MONO_TYPE_I1:
		case MONO_TYPE_I2:
			value = builder.CreateSExt (value, builder.getInt32Ty ());
			pushed = mono_get_int32_type ();
			break;
		case MONO_TYPE_BOOLEAN:
		case MONO_TYPE_CHAR:
		case MONO_TYPE_U1:
		case MONO_TYPE_U2:
			value = builder.CreateZExt (value, builder.getInt32Ty ());
			pushed = mono_get_int32_type ();
			break;
		default:
			break;
		}
	}

	pop_stack (1);
	push_stack (value, pushed);
	return llvm::Error::success ();
}

/*
 * III.4.11  ldflda - load field address
 *
 *   Format     Assembly Format   Description
 *   7C <T>     ldflda field      Push the address of field of object obj on the stack.
 *
 * Stack Transition:
 *
 *   ..., obj -> ..., address
 *
 * Description:
 *
 *   The ldflda instruction pushes the address of a field of obj. obj is either an
 *   object, type O, a managed pointer, type &, or an unmanaged pointer, type native
 *   int. The use of an unmanaged pointer is not allowed in verifiable code. The value
 *   returned by ldflda is a managed pointer (type &) unless obj is an unmanaged
 *   pointer, in which case it is an unmanaged pointer (type native int).
 *
 *   field is a metadata token (a fieldref or fielddef; see Partition II) that shall
 *   refer to a field member. The field can be either an instance field (in which case
 *   obj shall not be null) or a static field.
 *
 * Exceptions:
 *
 *   System.FieldAccessException is thrown if field is not accessible.
 *
 *   System.InvalidOperationException is thrown if the obj is not within the application
 *   domain from which it is being accessed. The address of a field that is not inside
 *   the accessing application domain cannot be loaded.
 *
 *   System.MissingFieldException is thrown if field is not found in the metadata. This
 *   is typically checked when CIL is converted to native code, not at runtime.
 *
 *   System.NullReferenceException is thrown if obj is null and the field isn't static.
 *
 * Correctness:
 *
 *   Correct CIL ensures that field is a valid fieldref token and that the type of obj
 *   is compatible-with the Class of field.
 *
 * Verifiability:
 *
 *   For verifiable code, obj shall not be an unmanaged pointer.
 */
llvm::Error
MethodLLVMEmitter::emit_ldflda (MonoIrBuilder &builder, uint32_t token)
{
	if (stack.empty ())
		return unbalanced_stack (1);

	llvm::Expected<MonoClassField *> field = resolve_field (token);
	if (!field)
		return field.takeError ();

	StackValue object = get_stack (0);
	llvm::Expected<llvm::Value *> address = field_address (builder, object, *field);
	if (!address)
		return address.takeError ();

	/*
	 * The address of a field of something the GC does not know about is not something
	 * it should start tracking, so an unmanaged receiver keeps its kind.
	 */
	MonoType *ftype = mono_field_get_type_internal (*field);
	MonoType *pushed =
		stack_type (object.type) == NativeInt
			? mono_get_int_type ()
			: m_class_get_this_arg (mono_class_from_mono_type_internal (ftype));

	pop_stack (1);
	push_stack (*address, pushed);
	return llvm::Error::success ();
}

/*
 * III.4.28  stfld - store into a field of an object
 *
 *   Format     Assembly Format   Description
 *   7D <T>     stfld field       Replace the value of field of the object obj with
 *                                value.
 *
 * Stack Transition:
 *
 *   ..., obj, value -> ...,
 *
 * Description:
 *
 *   The stfld instruction replaces the value of a field of an obj (an O) or via a
 *   pointer (type native int, or &) with value. field is a metadata token (a fieldref
 *   or fielddef; see Partition II) that refers to a field member reference. stfld pops
 *   the value and the object reference off the stack and updates the object.
 *
 *   Storing into fields that hold a value smaller than 4 bytes truncates the value as
 *   it moves from the stack to the local variable. Floating-point values are rounded
 *   from their native size (type F) to the size associated with the argument. (See
 *   §III.1.1.1, Numeric data types.)
 *
 *   The stfld instruction can have a prefix of either or both of unaligned. and
 *   volatile..
 *
 * Exceptions:
 *
 *   System.FieldAccessException is thrown if field is not accessible.
 *
 *   System.NullReferenceException is thrown if obj is null and the field isn't static.
 *
 *   System.MissingFieldException is thrown if field is not found in the metadata. This
 *   is typically checked when CIL is converted to native code, not at runtime.
 *
 * Correctness:
 *
 *   Correct CIL ensures that field is a valid token referring to a field, and that obj
 *   and value will always have types appropriate for the assignment being performed,
 *   subject to implicit conversion as specified in §III.1.6.
 */
llvm::Error
MethodLLVMEmitter::emit_stfld (MonoIrBuilder &builder, uint32_t token)
{
	if (stack.size () < 2)
		return unbalanced_stack (2);

	llvm::Expected<MonoClassField *> field = resolve_field (token);
	if (!field)
		return field.takeError ();

	MonoType *ftype = mono_field_get_type_internal (*field);
	llvm::Expected<llvm::Value *> value =
		coerce_to_location (builder, get_stack (0), ftype);
	if (!value)
		return value.takeError ();

	llvm::Expected<llvm::Value *> address = field_address (builder, get_stack (1), *field);
	if (!address)
		return address.takeError ();

	pop_stack (2);

	/*
	 * A reference going into the heap has to go through the collector rather than
	 * straight to memory, or the card table never learns that this object now points
	 * somewhere it did not before.
	 */
	if (mini_type_is_reference (ftype))
		builder.CreateCall (wbarrier_decl (module), { *address, *value });
	else
		builder.CreateAlignedStore (*value, *address, type_alignment (ftype));

	return llvm::Error::success ();
}

} // namespace mono

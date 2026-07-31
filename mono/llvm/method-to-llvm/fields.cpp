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

/// The collector's hook for storing a reference through a pointer it does not otherwise
/// know about, which is the general form that also covers a field reached through the
/// interior of an object and an element reached through an array.
llvm::FunctionCallee
MethodLLVMEmitter::wbarrier_decl ()
{
	llvm::LLVMContext &ctx = context ();
	llvm::Type *ptr = llvm::PointerType::get (ctx, 0);

	return module->getOrInsertFunction ("mono_gc_wbarrier_generic_store_internal",
	                                    llvm::Type::getVoidTy (ctx), ptr, ptr);
}

/// The field TOKEN names, with its declaring class laid out so that its offset can be
/// asked for.
llvm::Expected<MonoClassField *>
MethodLLVMEmitter::resolve_field (uint32_t token, bool want_static)
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

	bool is_static = (mono_field_get_flags (field) & FIELD_ATTRIBUTE_STATIC) != 0;

	if (is_static != want_static) {
		char *name = mono_field_full_name (field);
		llvm::Error error = invalid_il (llvm::Twine ("this instruction needs ")
		                                + (want_static ? "a static" : "an instance")
		                                + " field, but " + name + " is "
		                                + (is_static ? "static" : "an instance field"));

		g_free (name);
		return std::move (error);
	}

	return field;
}

/// The address the engine has to resolve for a per-class run-time structure.
///
/// A class contributes two: mono_statics_<class>, the block its static fields live in,
/// and mono_vtable_<class>, its MonoVTable. One relocation per class rather than per
/// field, so every static of a class shares a symbol and differs only in the offset the
/// GEP adds.
llvm::Constant *
MethodLLVMEmitter::class_symbol (MonoClass *klass, const char *prefix)
{
	char *name = mono_type_full_name (m_class_get_byval_arg (klass));
	std::string symbol = std::string (prefix) + name;

	g_free (name);

	if (llvm::GlobalVariable *existing = module->getNamedGlobal (symbol))
		return existing;

	return new llvm::GlobalVariable (*module, llvm::Type::getInt8Ty (context ()), false,
	                                 llvm::GlobalValue::ExternalLinkage, nullptr, symbol);
}

/// Run KLASS's static constructor if it has not run yet.
///
/// This is a call rather than something settled while compiling: a cctor is arbitrary
/// managed code and must never run on a compilation thread. mono_generic_class_init is
/// idempotent and returns once the class is ready, so the only cost of emitting it at
/// every access is one that a later pass can take back out - and it has to be emitted
/// every time, because a vtable is not observably initialized even to the thread that
/// just initialized it.
void
MethodLLVMEmitter::emit_class_init (MonoIrBuilder &builder, MonoClass *klass)
{
	llvm::LLVMContext &ctx = context ();
	llvm::FunctionCallee init = module->getOrInsertFunction (
		"mono_generic_class_init", llvm::Type::getVoidTy (ctx),
		llvm::PointerType::get (ctx, 0));

	/* A cctor can throw, so inside a try region this has to be able to unwind. */
	emit_protected_call (builder, init, { class_symbol (klass, "mono_vtable_") });
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

/// Where FIELD lives in its class's statics block.
llvm::Value *
MethodLLVMEmitter::static_field_address (MonoIrBuilder &builder, MonoClassField *field)
{
	/* A static's offset is into the block itself, so there is no header to discount. */
	return builder.CreateGEP (builder.getInt8Ty (),
	                          class_symbol (field->parent, "mono_statics_"),
	                          builder.getInt32 (m_field_get_offset (field)));
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

	llvm::Expected<MonoClassField *> field = resolve_field (token, false);
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

	pop_stack (1);
	push_stack (widen_to_stack (builder, value, ftype), stack_slot_type (ftype));
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

	llvm::Expected<MonoClassField *> field = resolve_field (token, false);
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

	llvm::Expected<MonoClassField *> field = resolve_field (token, false);
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
		builder.CreateCall (wbarrier_decl (), { *address, *value });
	else
		builder.CreateAlignedStore (*value, *address, type_alignment (ftype));

	return llvm::Error::success ();
}

/*
 * III.4.14  ldsfld - load static field of a class
 *
 *   Format     Assembly Format   Description
 *   7E <T>     ldsfld field      Push the value of field on the stack.
 *
 * Stack Transition:
 *
 *   ..., -> ..., value
 *
 * Description:
 *
 *   The ldsfld instruction pushes the value of a static (shared among all instances of
 *   a class) field on the stack. field is a metadata token (a fieldref or fielddef; see
 *   Partition II) referring to a static field member. The return type is that
 *   associated with field.
 *
 *   The ldsfld instruction can have a volatile. prefix.
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
 */
llvm::Error
MethodLLVMEmitter::emit_ldsfld (MonoIrBuilder &builder, uint32_t token)
{
	llvm::Expected<MonoClassField *> field = resolve_field (token, true);
	if (!field)
		return field.takeError ();

	MonoType *ftype = mono_field_get_type_internal (*field);
	llvm::Expected<llvm::Type *> type = convert_type (ftype);
	if (!type)
		return type.takeError ();

	emit_class_init (builder, (*field)->parent);

	llvm::Value *address = static_field_address (builder, *field);
	llvm::Value *value = builder.CreateAlignedLoad (*type, address, type_alignment (ftype));

	push_stack (widen_to_stack (builder, value, ftype), stack_slot_type (ftype));
	return llvm::Error::success ();
}

/*
 * III.4.30  stsfld - store a static field of a class
 *
 *   Format     Assembly Format   Description
 *   80 <T>     stsfld field      Replace the value of field with val.
 *
 * Stack Transition:
 *
 *   ..., val -> ...,
 *
 * Description:
 *
 *   The stsfld instruction replaces the value of a static field with a value from the
 *   stack. field is a metadata token (a fieldref or fielddef; see Partition II) that
 *   shall refer to a static field member. stsfld pops the value off the stack and
 *   updates the static field with that value.
 *
 *   Storing into fields that hold a value smaller than 4 bytes truncates the value as
 *   it moves from the stack to the local variable. Floating-point values are rounded
 *   from their native size (type F) to the size associated with the argument. (See
 *   §III.1.1.1, Numeric data types.)
 *
 *   The stsfld instruction can have a volatile. prefix.
 *
 * Exceptions:
 *
 *   System.FieldAccessException is thrown if field is not accessible.
 *
 *   System.MissingFieldException is thrown if field is not found in the metadata. This
 *   is typically checked when CIL is converted to native code, not at runtime.
 */
llvm::Error
MethodLLVMEmitter::emit_stsfld (MonoIrBuilder &builder, uint32_t token)
{
	if (stack.empty ())
		return unbalanced_stack (1);

	llvm::Expected<MonoClassField *> field = resolve_field (token, true);
	if (!field)
		return field.takeError ();

	MonoType *ftype = mono_field_get_type_internal (*field);
	llvm::Expected<llvm::Value *> value = coerce_to_location (builder, get_stack (0), ftype);
	if (!value)
		return value.takeError ();

	emit_class_init (builder, (*field)->parent);

	llvm::Value *address = static_field_address (builder, *field);

	pop_stack (1);

	if (mini_type_is_reference (ftype))
		builder.CreateCall (wbarrier_decl (), { address, *value });
	else
		builder.CreateAlignedStore (*value, address, type_alignment (ftype));

	return llvm::Error::success ();
}

} // namespace mono

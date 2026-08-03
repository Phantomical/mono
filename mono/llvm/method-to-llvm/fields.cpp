#include "method-to-llvm.hpp"
#include "runtime-error.hpp"
#include "mono/metadata/abi-details.h"
#include "mono/metadata/class-internals.h"
#include "mono/metadata/debug-helpers.h"
#include "mono/metadata/loader.h"
#include "mono/metadata/metadata.h"
#include "mono/metadata/object-internals.h"
#include "mono/metadata/remoting.h"
#include <llvm/ADT/StringRef.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Type.h>
#include <cstdio>

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
///
/// The instance opcodes accept a static field - the object is popped and discarded -
/// so their callers pass IS_STATIC and reroute; the static opcodes refuse an instance
/// field outright.
llvm::Expected<MonoClassField *>
MethodLLVMEmitter::resolve_field (uint32_t token, bool want_static, bool *out_is_static)
{
	ERROR_DECL (metadata_error);
	MonoClass *klass = nullptr;
	MonoClassField *field;

	if (in_wrapper ()) {
		field = static_cast<MonoClassField *> (wrapper_data (token));

		if (field == nullptr)
			return invalid_il (llvm::Twine ("wrapper data slot ") + llvm::Twine (token)
			                   + " does not name a field");
	} else {
		field = mono_field_from_token_checked (
			m_class_get_image (method->klass), token, &klass,
			mono_method_get_context (method), metadata_error);

		if (field == nullptr)
			return runtime_error (metadata_error);
	}

	/*
	 * Laying the class out is what makes the offset readable; it settles metadata
	 * only, and is not the class initializer, which must never run here.
	 */
	mono_class_init_checked (field->parent, metadata_error);
	if (!is_ok (metadata_error))
		return runtime_error (metadata_error);

	bool is_static = (mono_field_get_flags (field) & FIELD_ATTRIBUTE_STATIC) != 0;

	if (out_is_static != nullptr)
		*out_is_static = is_static;
	if (is_static && !want_static && out_is_static != nullptr)
		return field;
	if (is_static != want_static) {
		char *name = mono_field_full_name (field);
		llvm::Error error =
			invalid_il (llvm::Twine ("this instruction needs ")
		                    + (want_static ? "a static" : "an instance") + " field, but "
		                    + name + " is " + (is_static ? "static" : "an instance field"));

		g_free (name);
		return std::move (error);
	}

	return field;
}

/// The external global called NAME, whose address the engine resolves against the
/// runtime, created on first use.
llvm::Constant *
MethodLLVMEmitter::extern_symbol (const std::string &name)
{
	if (llvm::GlobalVariable *existing = module->getNamedGlobal (name))
		return existing;

	return new llvm::GlobalVariable (*module, llvm::Type::getInt8Ty (context ()), false,
	                                 llvm::GlobalValue::ExternalLinkage, nullptr, name);
}

/// Note that NAME stands for OBJECT, so that the engine can resolve it without
/// reading the metadata back out of the name.
void
MethodLLVMEmitter::record_external (const std::string &name, ExternalSymbol::Kind kind,
                                    void *object)
{
	if (externals != nullptr)
		externals->push_back ({ name, kind, object });
}

/// A symbol name standing for OBJECT: NAME is what makes it readable, the pointer
/// is what makes it unique.
///
/// No printed name is unique on its own. Two assemblies loaded from different
/// bytes can carry the same assembly name and define classes, methods and fields
/// that print identically - Assembly.Load (byte[]) over two builds of the same
/// source is the ordinary way to get there. The engine resolves a symbol by name
/// and keeps the first definition it is given, so without the pointer the second
/// assembly's code would link against the first's MonoClass, vtable, statics and
/// string literals.
std::string
MethodLLVMEmitter::identity_symbol (const std::string &name, const void *object)
{
	char suffix[32];

	snprintf (suffix, sizeof (suffix), "@%p", object);
	return name + suffix;
}

/// The address the engine has to resolve for a per-class run-time structure.
///
/// A class contributes three: mono_statics_<class>, the block its static fields live
/// in, mono_vtable_<class>, its MonoVTable, and mono_class_<class>, the MonoClass
/// itself. One relocation per class rather than per field, so every static of a class
/// shares a symbol and differs only in the offset the GEP adds.
llvm::Constant *
MethodLLVMEmitter::class_symbol (MonoClass *klass, const char *prefix)
{
	char *name = mono_type_full_name (m_class_get_byval_arg (klass));
	std::string symbol = identity_symbol (std::string (prefix) + name, klass);
	ExternalSymbol::Kind kind = ExternalSymbol::Kind::Class;

	if (llvm::StringRef (prefix) == "mono_vtable_")
		kind = ExternalSymbol::Kind::VTable;
	else if (llvm::StringRef (prefix) == "mono_statics_")
		kind = ExternalSymbol::Kind::Statics;

	g_free (name);
	record_external (symbol, kind, klass);
	return extern_symbol (symbol);
}

/// The address the engine has to resolve for FIELD's own MonoClassField.
llvm::Constant *
MethodLLVMEmitter::field_symbol (MonoClassField *field)
{
	char *name = mono_field_full_name (field);
	std::string symbol = identity_symbol (std::string ("mono_field_") + name, field);

	g_free (name);
	record_external (symbol, ExternalSymbol::Kind::Field, field);
	return extern_symbol (symbol);
}

/// Run KLASS's static constructor if it has not run yet.
///
/// This is a call rather than something settled while compiling: a cctor is arbitrary
/// managed code and must never run on a compilation thread. mono_generic_class_init is
/// idempotent and returns once the class is ready, so the only cost of emitting it at
/// every access is one that a later pass can take back out - and it has to be emitted
/// every time, because a vtable is not observably initialized even to the thread that
/// just initialized it.
llvm::Error
MethodLLVMEmitter::emit_class_init (MonoIrBuilder &builder, MonoClass *klass)
{
	/* Through the wrapper: a cctor can throw, and the failure is pending until
	 * the wrapper's check turns it into a real one. */
	llvm::Expected<llvm::Function *> init =
		icall_wrapper_decl (MONO_JIT_ICALL_mono_generic_class_init);

	if (!init)
		return init.takeError ();

	emit_protected_call (builder, *init,
	                     adapt_to_callee (builder, *init,
	                                      {class_symbol (klass, "mono_vtable_")}));
	return llvm::Error::success ();
}

/// Whether an instance access to FIELD through RECEIVER can land on a
/// transparent proxy, in which case the field's bytes are not at their offset -
/// they are in the real object the proxy stands for, possibly in another
/// context or domain - and the access has to go through the runtime's remoting
/// field wrappers.
///
/// A receiver that is this method's own `this` is usually proof of a real
/// object, but not on ContextBoundObject and not on MarshalByRefObject itself:
/// the runtime runs those bodies with `this` still the proxy (the same-context
/// shortcut in mono_remoting_wrapper does exactly that), so their fields check
/// every time.
bool
MethodLLVMEmitter::remote_field_access (StackValue receiver, MonoClassField *field)
{
#ifdef DISABLE_REMOTING
	return false;
#else
	MonoClass *klass = field->parent;

	if (m_class_is_valuetype (klass) || stack_type (receiver.type) != ObjectRef)
		return false;
	if (mono_class_is_contextbound (klass)
	    || klass == mono_defaults.marshalbyrefobject_class)
		return true;
	return mono_class_is_marshalbyref (klass) && !is_own_this (receiver.value);
#endif
}

/// The constant operands every remoting field wrapper takes after the object:
/// the declaring MonoClass, the MonoClassField, and the field's offset, pushed
/// in that order so a wrapper call can be collected off the stack like any
/// other.
void
MethodLLVMEmitter::push_field_wrapper_operands (MonoIrBuilder &builder,
                                                MonoClassField *field)
{
	MonoType *nint = mono_get_int_type ();

	push_stack (class_symbol (field->parent, "mono_class_"), nint);
	push_stack (field_symbol (field), nint);
	push_stack (builder.getInt64 (m_field_get_offset (field)), nint);
}

/// Where FIELD lives inside the object on top of the stack.
llvm::Expected<llvm::Value *>
MethodLLVMEmitter::field_address (MonoIrBuilder &builder, StackValue object,
                                  MonoClassField *field, bool null_check)
{
	StackType type = stack_type (object.type);

	if (type != ObjectRef && type != ManagedPtr && type != NativeInt)
		return invalid_il (llvm::Twine ("a field cannot be reached through operand type ")
		                   + describe (object.type, type));

	llvm::Value *base = object.value;

	/* A native int receiver is only a number until something dereferences it. */
	if (!base->getType ()->isPointerTy ())
		base = builder.CreateIntToPtr (base, llvm::PointerType::get (context (), 0));

	if (null_check)
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
///
/// An RVA field also lives there: creating the vtable copies the image data into the
/// block at the field's offset. A thread- or context-local static does not - the
/// offset it records is a per-thread lookup cookie - so its address has to come from
/// the runtime, on every access.
llvm::Expected<llvm::Value *>
MethodLLVMEmitter::static_field_address (MonoIrBuilder &builder, MonoClassField *field)
{
	if (mono_class_field_is_special_static (field)) {
		llvm::Type *ptr = llvm::PointerType::get (context (), 0);
		llvm::Value *domain = builder.CreateCall (
			module->getOrInsertFunction ("mono_domain_get", ptr));
		llvm::Constant *symbol = field_symbol (field);
		llvm::Expected<llvm::Function *> lookup =
			icall_wrapper_decl (MONO_JIT_ICALL_mono_class_static_field_address);

		if (!lookup)
			return lookup.takeError ();

		llvm::Value *address = emit_protected_call (
			builder, *lookup,
			adapt_to_callee (builder, *lookup, {domain, symbol}));

		/* The signature says native int; every user here wants the pointer. */
		if (!address->getType ()->isPointerTy ())
			address = builder.CreateIntToPtr (address, ptr);
		return address;
	}

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

	bool is_static = false;
	llvm::Expected<MonoClassField *> field = resolve_field (token, false, &is_static);
	if (!field)
		return field.takeError ();

	if (is_static) {
		pop_stack (1);
		return emit_ldsfld (builder, token);
	}

	MonoType *ftype = mono_field_get_type_internal (*field);
	llvm::Expected<llvm::Type *> type = convert_type (ftype);
	if (!type)
		return type.takeError ();

	StackValue object = get_stack (0);

#ifndef DISABLE_REMOTING
	if (remote_field_access (object, *field)) {
		MonoMethod *wrapper = mono_marshal_get_ldfld_wrapper (ftype);
		llvm::Expected<llvm::Function *> decl = create_method_decl (wrapper);

		if (!decl)
			return decl.takeError ();

		MonoMethodSignature *wsig = mono_method_signature_internal (wrapper);

		push_field_wrapper_operands (builder, *field);

		llvm::Expected<std::vector<llvm::Value *>> args =
			pop_call_arguments (builder, wsig);

		if (!args)
			return args.takeError ();

		llvm::Value *value = emit_protected_call (builder, *decl, *args);

		pop_stack (4);
		push_stack (widen_to_stack (builder, value, wsig->ret),
		            stack_slot_type (wsig->ret));
		return llvm::Error::success ();
	}
#endif

	/*
	 * The object may also be an instance of a value type, sitting on the stack
	 * as the value itself (III.4.10). It has no address until it is given one,
	 * so it goes to a temporary and the field is read out of that. No null
	 * check: a value is never null. Decided off the raw type, not the
	 * underlying one: a magic nint rides the stack as its scalar, and reading
	 * its field through the underlying native int would dereference the value.
	 */
	if (!object.type->byref && MONO_TYPE_ISSTRUCT (object.type)
	    && !object.value->getType ()->isPointerTy ()) {
		llvm::Value *home = spill_to_temporary (builder, object.type);
		int32_t offset = static_cast<int32_t> (m_field_get_offset (*field))
		                 - MONO_ABI_SIZEOF (MonoObject);
		llvm::Value *address = builder.CreateGEP (builder.getInt8Ty (), home,
		                                          builder.getInt32 (offset));
		llvm::Value *value = emit_memory_load (builder, *type, address, ftype);

		pop_stack (1);
		push_stack (widen_to_stack (builder, value, ftype), stack_slot_type (ftype));
		return llvm::Error::success ();
	}

	llvm::Expected<llvm::Value *> address = field_address (builder, object, *field);
	if (!address)
		return address.takeError ();

	llvm::Value *value = emit_memory_load (builder, *type, *address, ftype);

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

	bool is_static = false;
	llvm::Expected<MonoClassField *> field = resolve_field (token, false, &is_static);
	if (!field)
		return field.takeError ();

	if (is_static) {
		pop_stack (1);
		return emit_ldsflda (builder, token);
	}

	StackValue object = get_stack (0);

#ifndef DISABLE_REMOTING
	if (remote_field_access (object, *field)) {
		MonoMethod *wrapper =
			mono_marshal_get_ldflda_wrapper (mono_field_get_type_internal (*field));
		llvm::Expected<llvm::Function *> decl = create_method_decl (wrapper);

		if (!decl)
			return decl.takeError ();

		push_field_wrapper_operands (builder, *field);

		llvm::Expected<std::vector<llvm::Value *>> args =
			pop_call_arguments (builder, mono_method_signature_internal (wrapper));

		if (!args)
			return args.takeError ();

		llvm::Value *value = emit_protected_call (builder, *decl, *args);

		if (!value->getType ()->isPointerTy ())
			value = builder.CreateIntToPtr (
				value, llvm::PointerType::get (context (), 0));

		pop_stack (4);
		push_stack (value,
		            m_class_get_this_arg (mono_class_from_mono_type_internal (
				    mono_field_get_type_internal (*field))));
		return llvm::Error::success ();
	}
#endif

	/*
	 * Taking the address dereferences nothing, so only an object-reference
	 * receiver is checked - through a native pointer even null just computes,
	 * and the fault waits for whatever dereferences the result.
	 */
	llvm::Expected<llvm::Value *> address =
		field_address (builder, object, *field,
		               stack_type (object.type) == ObjectRef);
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

	bool is_static = false;
	llvm::Expected<MonoClassField *> field = resolve_field (token, false, &is_static);
	if (!field)
		return field.takeError ();

	if (is_static) {
		StackValue stored = get_stack (0);

		pop_stack (2);
		push_stack (stored.value, stored.type);
		return emit_stsfld (builder, token);
	}

	MonoType *ftype = mono_field_get_type_internal (*field);

#ifndef DISABLE_REMOTING
	if (remote_field_access (get_stack (1), *field)) {
		MonoMethod *wrapper = mono_marshal_get_stfld_wrapper (ftype);
		llvm::Expected<llvm::Function *> decl = create_method_decl (wrapper);

		if (!decl)
			return decl.takeError ();

		/* The wrapper wants the value after the constants, so it comes off
		 * and goes back on top of them. */
		StackValue stored = get_stack (0);

		pop_stack (1);
		push_field_wrapper_operands (builder, *field);
		push_stack (stored.value, stored.type);

		llvm::Expected<std::vector<llvm::Value *>> args =
			pop_call_arguments (builder, mono_method_signature_internal (wrapper));

		if (!args)
			return args.takeError ();

		emit_protected_call (builder, *decl, *args);
		pop_stack (5);
		return llvm::Error::success ();
	}
#endif

	llvm::Expected<llvm::Value *> value = coerce_to_location (builder, get_stack (0), ftype);
	if (!value)
		return value.takeError ();

	llvm::Expected<llvm::Value *> address = field_address (builder, get_stack (1), *field);
	if (!address)
		return address.takeError ();

	pop_stack (2);
	emit_memory_store (builder, *value, *address, ftype);
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

	if (llvm::Error error = emit_class_init (builder, (*field)->parent))
		return error;

	llvm::Expected<llvm::Value *> address = static_field_address (builder, *field);
	if (!address)
		return address.takeError ();

	llvm::Value *value = emit_memory_load (builder, *type, *address, ftype);

	push_stack (widen_to_stack (builder, value, ftype), stack_slot_type (ftype));
	return llvm::Error::success ();
}

/*
 * III.4.15  ldsflda - load static field address
 *
 *   Format     Assembly Format   Description
 *   7F <T>     ldsflda field     Push the address of the static field, field, on the
 *                                stack.
 *
 * Stack Transition:
 *
 *   ..., -> ..., address
 *
 * Description:
 *
 *   The ldsflda instruction pushes the address (a managed pointer, type &, if field
 *   refers to a type whose memory is managed; otherwise an unmanaged pointer, type
 *   native int) of a static field on the stack. field is a metadata token (a fieldref
 *   or fielddef; see Partition II) referring to a static field member. (Note that
 *   field can be a static global with assigned RVA, in which case its memory is
 *   unmanaged; where RVA stands for Relative Virtual Address, the offset of the field
 *   from the base address at which its containing PE file is loaded into memory)
 *
 * Exceptions:
 *
 *   System.FieldAccessException is thrown if field is not accessible.
 *
 *   System.MissingFieldException is thrown if field is not found in the metadata. This
 *   is typically checked when CIL is converted to native code, not at runtime.
 *
 * Correctness:
 *
 *   Correct CIL ensures that field is a valid metadata token referring to a static
 *   field member if field refers to a type whose memory is managed.
 *
 * Verifiability:
 *
 *   For verifiable code, field cannot be init-only. If field refers to a type whose
 *   memory is managed, verification (§III.1.8) tracks the type of the value loaded
 *   onto the stack as a managed pointer to the verification type (§I.8.7) of field. If
 *   field refers to a type whose memory is unmanaged, verification (§III.1.8) tracks
 *   the type of the value loaded onto the stack as an unmanaged pointer.
 *
 * Remark:
 *
 *   Using ldsflda to compute the address of a static, init-only field and then using
 *   the resulting pointer to modify that value outside the body of the class
 *   initializer can lead to unpredictable behavior.
 */
llvm::Error
MethodLLVMEmitter::emit_ldsflda (MonoIrBuilder &builder, uint32_t token)
{
	llvm::Expected<MonoClassField *> field = resolve_field (token, true);
	if (!field)
		return field.takeError ();

	/*
	 * Whoever takes the address is about to touch the field, so the class has to be
	 * as initialized here as it would be for the load itself.
	 */
	if (llvm::Error error = emit_class_init (builder, (*field)->parent))
		return error;

	MonoType *ftype = mono_field_get_type_internal (*field);
	llvm::Expected<llvm::Value *> address = static_field_address (builder, *field);

	if (!address)
		return address.takeError ();

	push_stack (*address,
	            m_class_get_this_arg (mono_class_from_mono_type_internal (ftype)));
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

	if (llvm::Error error = emit_class_init (builder, (*field)->parent))
		return error;

	llvm::Expected<llvm::Value *> address = static_field_address (builder, *field);

	if (!address)
		return address.takeError ();

	pop_stack (1);
	emit_memory_store (builder, *value, *address, ftype);
	return llvm::Error::success ();
}

} // namespace mono

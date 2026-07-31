#include "method-to-llvm.hpp"
#include "runtime-error.hpp"
#include "mono/metadata/abi-details.h"
#include "mono/metadata/class-internals.h"
#include "mono/metadata/debug-helpers.h"
#include "mono/metadata/loader.h"
#include "mono/metadata/metadata.h"
#include "mono/metadata/object-internals.h"
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/InstrTypes.h>
#include <llvm/IR/Type.h>

namespace mono {

/// The method TOKEN names, resolved against this method's generic context.
llvm::Expected<MonoMethod *>
MethodLLVMEmitter::resolve_method (uint32_t token)
{
	ERROR_DECL (metadata_error);
	MonoMethod *target =
		mono_get_method_checked (m_class_get_image (method->klass), token, nullptr,
	                                 mono_method_get_context (method), metadata_error);

	if (target == nullptr)
		return runtime_error (metadata_error);

	return target;
}

/// Take a call's arguments off the evaluation stack, converted to what the signature
/// asks for.
///
/// The receiver of an instance method is argument zero and is not in the parameter list,
/// so it comes back at the front having been coerced to nothing - what a `this` may be
/// is settled by the caller, which is the only one that knows whether the call is
/// virtual.
llvm::Expected<std::vector<llvm::Value *>>
MethodLLVMEmitter::pop_call_arguments (MonoIrBuilder &builder, MonoMethodSignature *sig)
{
	size_t count = sig->param_count + sig->hasthis;

	if (stack.size () < count)
		return unbalanced_stack (count);

	std::vector<llvm::Value *> args (count);

	/* The last parameter is on top, so the stack unwinds into the list backwards. */
	for (size_t i = count; i-- > 0;) {
		StackValue value = get_stack (count - 1 - i);

		if (sig->hasthis && i == 0) {
			args[i] = value.value;
			continue;
		}

		llvm::Expected<llvm::Value *> converted =
			coerce_to_location (builder, value, sig->params[i - sig->hasthis]);

		if (!converted)
			return converted.takeError ();

		args[i] = *converted;
	}

	return args;
}

/// The pointer stored OFFSET bytes into the vtable of the object RECEIVER points at.
llvm::Value *
MethodLLVMEmitter::vtable_entry (MonoIrBuilder &builder, llvm::Value *receiver, int32_t offset)
{
	llvm::Type *ptr = llvm::PointerType::get (context (), 0);
	llvm::Value *vtable = builder.CreateAlignedLoad (
		ptr,
		builder.CreateGEP (builder.getInt8Ty (), receiver,
	                           builder.getInt32 (MONO_STRUCT_OFFSET (MonoObject, vtable))),
		llvm::Align (TARGET_SIZEOF_VOID_P));

	return builder.CreateAlignedLoad (
		ptr, builder.CreateGEP (builder.getInt8Ty (), vtable, builder.getInt32 (offset)),
		llvm::Align (TARGET_SIZEOF_VOID_P));
}

/// The address of TARGET's entry in the vtable of the object RECEIVER points at.
///
/// A virtual call reads the callee out of the receiver rather than knowing it: the
/// object's vtable pointer, indexed by the slot the method was assigned when its class
/// was laid out.
llvm::Value *
MethodLLVMEmitter::virtual_callee (MonoIrBuilder &builder, llvm::Value *receiver,
                                   MonoMethod *target)
{
	return vtable_entry (builder, receiver,
	                     MONO_STRUCT_OFFSET (MonoVTable, vtable)
	                             + mono_method_get_vtable_index (target)
	                                       * TARGET_SIZEOF_VOID_P);
}

/// The address of TARGET's entry in the IMT of the object RECEIVER points at.
///
/// An interface method has no fixed vtable slot - where an implementation lands depends
/// on the class implementing it - so dispatch goes through the interface method table
/// instead, a small hash table the runtime lays out in the words immediately before
/// each MonoVTable. Its slots are therefore reached at negative offsets from the same
/// base the ordinary slots are.
llvm::Value *
MethodLLVMEmitter::interface_callee (MonoIrBuilder &builder, llvm::Value *receiver,
                                     MonoMethod *target)
{
	int32_t slot = static_cast<int32_t> (mono_method_get_imt_slot (target)) - MONO_IMT_SIZE;

	return vtable_entry (builder, receiver, slot * TARGET_SIZEOF_VOID_P);
}

/// The address the engine has to resolve for TARGET's own MonoMethod - the runtime's
/// description of the method, as opposed to its code.
llvm::Constant *
MethodLLVMEmitter::method_symbol (MonoMethod *target)
{
	char *name = mono_method_full_name (target, TRUE);
	std::string symbol = std::string ("mono_method_") + name;

	g_free (name);
	return extern_symbol (symbol);
}

/*
 * III.3.19  call - call a method
 *
 *   Format     Assembly Format   Description
 *   28 <T>     call method       Call method described by method.
 *
 * Stack Transition:
 *
 *   ..., arg0, arg1 ... argN -> ..., retVal (not always returned)
 *
 * Description:
 *
 *   The call instruction calls the method indicated by the descriptor method. method is
 *   a metadata token (a methodref, methoddef, or methodspec; see Partition II) that
 *   indicates the method to call, and the number, type, and order of the arguments that
 *   have been placed on the stack to be passed to that method, as well as the calling
 *   convention to be used. (See Partition I for a detailed description of the CIL
 *   calling sequence.) The call instruction can be immediately preceded by a tail.
 *   prefix to specify that the current method state should be released before
 *   transferring control (see §III.2.3).
 *
 *   The metadata token carries sufficient information to determine whether the call is
 *   to a static method, an instance method, a virtual method, or a global function. In
 *   all of these cases the destination address is determined entirely from the metadata
 *   token. (Contrast this with the callvirt instruction for calling virtual methods,
 *   where the destination address also depends upon the runtime type of the instance
 *   reference pushed before the callvirt.)
 *
 * Exceptions:
 *
 *   System.SecurityException is thrown if system security does not grant the caller
 *   access to the called method. The security check can occur when the CIL is converted
 *   to native code rather than at runtime.
 *
 *   System.MethodAccessException is thrown when there is an invalid attempt to access a
 *   non-public method.
 *
 *
 * III.4.2  callvirt - call a method associated, at runtime, with an object
 *
 *   Format     Assembly Format     Description
 *   6F <T>     callvirt method     Call a method associated with an object.
 *
 * Stack Transition:
 *
 *   ..., obj, arg1, ... argN -> ..., returnVal (not always returned)
 *
 * Description:
 *
 *   The callvirt instruction calls a late-bound method on an object. That is, the
 *   method is chosen based on the runtime type of obj rather than the compile-time
 *   class visible in the method pointer. callvirt can be used to call both virtual and
 *   instance methods.
 *
 *   The callvirt instruction can be immediately preceded by a tail. prefix to specify
 *   that the current method state should be released before transferring control.
 *
 * Exceptions:
 *
 *   System.NullReferenceException is thrown if obj is null.
 *
 *   System.MissingMethodException is thrown if a non-static method with the indicated
 *   name and signature could not be found in the class of obj or any of its
 *   superclasses. This is typically detected when CIL is converted to native code,
 *   rather than at runtime.
 */
llvm::Error
MethodLLVMEmitter::emit_call (MonoIrBuilder &builder, uint32_t token, bool is_virtual)
{
	llvm::Expected<MonoMethod *> target = resolve_method (token);
	if (!target)
		return target.takeError ();

	MonoMethod *callee_method = *target;
	MonoClass *constrained = nullptr;
	bool direct_this = false;
	bool box_receiver = false;

	if (prefixes.constrained != 0) {
		/*
		 * The prefix is only defined ahead of callvirt (III.2.1). The one
		 * later use of constrained. call - static virtual interface members -
		 * cannot appear in metadata this runtime accepts.
		 */
		if (!is_virtual)
			return invalid_il ("constrained. on a plain call");

		llvm::Expected<MonoType *> ctype = element_type_from_token (prefixes.constrained);
		if (!ctype)
			return ctype.takeError ();
		constrained = mono_class_from_mono_type_internal (*ctype);

		/*
		 * A value type that implements the method takes the call directly, with
		 * the managed pointer as this. One that does not - the method lives on
		 * Object, ValueType or Enum - would have its receiver boxed first.
		 */
		if (m_class_is_valuetype (constrained)) {
			ERROR_DECL (resolve_error);
			MonoMethod *impl = mono_class_get_virtual_method (
				constrained, callee_method, FALSE, resolve_error);

			if (!is_ok (resolve_error))
				return runtime_error (resolve_error);
			if (impl != nullptr && impl->klass == constrained) {
				callee_method = impl;
				direct_this = true;
			} else {
				/*
				 * The value type does not override the method - it lives
				 * on Object, ValueType or Enum - so the receiver is boxed
				 * and the call dispatches on the box.
				 */
				box_receiver = true;
			}
		}
	}

	MonoMethodSignature *sig = mono_method_signature_internal (callee_method);

	if (sig == nullptr)
		return invalid_il ("the called method has no signature");
	if (is_virtual && !sig->hasthis)
		return invalid_il ("callvirt needs an instance method");
	if (is_virtual && sig->generic_param_count != 0 && !callee_method->is_inflated)
		return invalid_il ("callvirt on an open generic method");

	/*
	 * The boxed receiver replaces the managed pointer in its stack slot, below the
	 * explicit arguments, before the arguments are collected.
	 */
	if (box_receiver) {
		size_t depth = sig->param_count;

		if (stack.size () < depth + 1)
			return unbalanced_stack (depth + 1);

		MonoType *vtype = m_class_get_byval_arg (constrained);
		llvm::Expected<llvm::Type *> slot = convert_type (vtype);
		if (!slot)
			return slot.takeError ();

		StackValue &receiver = stack[stack.size () - 1 - depth];
		llvm::Value *value = builder.CreateAlignedLoad (*slot, receiver.value,
		                                                type_alignment (vtype));

		receiver.value = box_value (builder, constrained, vtype, value);
		receiver.type = mono_get_object_type ();
	}

	llvm::Expected<llvm::Function *> declaration = create_method_decl (callee_method);
	if (!declaration)
		return declaration.takeError ();

	llvm::Expected<std::vector<llvm::Value *>> args = pop_call_arguments (builder, sig);
	if (!args)
		return args.takeError ();

	/* A reference-typed constrained receiver arrives as a pointer to the reference. */
	if (constrained != nullptr && !direct_this && !box_receiver)
		(*args)[0] =
			builder.CreateAlignedLoad (llvm::PointerType::get (context (), 0),
		                                   (*args)[0], llvm::Align (TARGET_SIZEOF_VOID_P));

	llvm::FunctionCallee callee = *declaration;
	bool keyed = false;

	if (is_virtual) {
		/*
		 * The receiver has to be there whether or not the callee is reached through
		 * it - an instance call on null throws before it dispatches.
		 */
		emit_null_check (builder, (*args)[0]);

		/*
		 * Only a method that can still be overridden has to be looked up. A final or
		 * non-virtual one is already the answer, and a callvirt on it is a null check
		 * with a direct call behind it - as is one a constrained. prefix already
		 * resolved to the value type's own implementation.
		 */
		bool overridable = !direct_this && (callee_method->flags & METHOD_ATTRIBUTE_VIRTUAL)
		                   && !(callee_method->flags & METHOD_ATTRIBUTE_FINAL);

		bool is_interface = mono_class_is_interface (callee_method->klass);
		bool generic_virtual = sig->generic_param_count != 0;

		if (overridable && (is_interface || generic_virtual)) {
			/*
			 * Several interface methods can hash to the same IMT slot, in which
			 * case what the slot holds is a thunk that picks the real target by
			 * looking at which method was asked for. The caller supplies that key
			 * in a register set aside for it, and the nest attribute is how
			 * unmodified LLVM is talked into filling it: nest pins an argument to
			 * %r10, which is exactly MONO_ARCH_IMT_REG on amd64. The key travels
			 * as one extra leading argument that the target, once reached, never
			 * looks at.
			 *
			 * A virtual generic method dispatches the same way even off a class:
			 * its slot can never hold one instantiation's code, so what sits
			 * there is a trampoline that reads the asked-for inflated method out
			 * of that same register to pick the instantiation.
			 */
			keyed = true;

			llvm::Value *code =
				is_interface
					? interface_callee (builder, (*args)[0], callee_method)
					: virtual_callee (builder, (*args)[0], callee_method);
			llvm::FunctionType *direct = (*declaration)->getFunctionType ();
			std::vector<llvm::Type *> params (direct->param_begin (),
			                                  direct->param_end ());

			params.insert (params.begin (), llvm::PointerType::get (context (), 0));
			callee = llvm::FunctionCallee (
				llvm::FunctionType::get (direct->getReturnType (), params,
			                                 direct->isVarArg ()),
				code);
			args->insert (args->begin (), method_symbol (callee_method));
		} else if (overridable && mono_method_get_vtable_index (callee_method) >= 0) {
			callee = llvm::FunctionCallee (
				(*declaration)->getFunctionType (),
				virtual_callee (builder, (*args)[0], callee_method));
		}
	}

	llvm::Value *result = emit_protected_call (builder, callee, *args);

	if (keyed)
		llvm::cast<llvm::CallBase> (result)->addParamAttr (0, llvm::Attribute::Nest);

	pop_stack (sig->param_count + sig->hasthis);

	if (sig->ret->type == MONO_TYPE_VOID && !sig->ret->byref)
		return llvm::Error::success ();

	push_stack (widen_to_stack (builder, result, sig->ret), stack_slot_type (sig->ret));
	return llvm::Error::success ();
}

} // namespace mono

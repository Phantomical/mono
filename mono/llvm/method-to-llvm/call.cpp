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

/// The address of TARGET's entry in the vtable of the object ARGS[0] points at.
///
/// A virtual call reads the callee out of the receiver rather than knowing it: the
/// object's vtable pointer, indexed by the slot the method was assigned when its class
/// was laid out.
llvm::Value *
MethodLLVMEmitter::virtual_callee (MonoIrBuilder &builder, llvm::Value *receiver,
                                   MonoMethod *target)
{
	llvm::Type *ptr = llvm::PointerType::get (context (), 0);
	llvm::Value *vtable = builder.CreateAlignedLoad (
		ptr,
		builder.CreateGEP (builder.getInt8Ty (), receiver,
		                   builder.getInt32 (MONO_STRUCT_OFFSET (MonoObject, vtable))),
		llvm::Align (TARGET_SIZEOF_VOID_P));
	int32_t slot = MONO_STRUCT_OFFSET (MonoVTable, vtable)
	               + mono_method_get_vtable_index (target) * TARGET_SIZEOF_VOID_P;

	return builder.CreateAlignedLoad (ptr,
	                                  builder.CreateGEP (builder.getInt8Ty (), vtable,
	                                                     builder.getInt32 (slot)),
	                                  llvm::Align (TARGET_SIZEOF_VOID_P));
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

	MonoMethodSignature *sig = mono_method_signature_internal (*target);

	if (sig == nullptr)
		return invalid_il ("the called method has no signature");
	if (is_virtual && !sig->hasthis)
		return invalid_il ("callvirt needs an instance method");

	llvm::Expected<llvm::Function *> declaration = create_method_decl (*target);
	if (!declaration)
		return declaration.takeError ();

	llvm::Expected<std::vector<llvm::Value *>> args = pop_call_arguments (builder, sig);
	if (!args)
		return args.takeError ();

	llvm::FunctionCallee callee = *declaration;

	if (is_virtual) {
		/*
		 * The receiver has to be there whether or not the callee is reached through
		 * it - an instance call on null throws before it dispatches.
		 */
		emit_null_check (builder, (*args)[0]);

		/*
		 * Only a method that can still be overridden has to be looked up. A final or
		 * non-virtual one is already the answer, and a callvirt on it is a null check
		 * with a direct call behind it.
		 */
		if (mono_method_get_vtable_index (*target) >= 0
		    && ((*target)->flags & METHOD_ATTRIBUTE_VIRTUAL)
		    && !((*target)->flags & METHOD_ATTRIBUTE_FINAL)
		    && !mono_class_is_interface ((*target)->klass))
			callee = llvm::FunctionCallee (
				(*declaration)->getFunctionType (),
				virtual_callee (builder, (*args)[0], *target));
		else if (mono_class_is_interface ((*target)->klass))
			return unsupported_il ("interface dispatch");
	}

	llvm::Value *result = emit_protected_call (builder, callee, *args);

	pop_stack (sig->param_count + sig->hasthis);

	if (sig->ret->type == MONO_TYPE_VOID && !sig->ret->byref)
		return llvm::Error::success ();

	push_stack (widen_to_stack (builder, result, sig->ret), stack_slot_type (sig->ret));
	return llvm::Error::success ();
}

} // namespace mono

#include "method-to-llvm.hpp"
#include "runtime-error.hpp"
#include "mono/metadata/class-internals.h"
#include "mono/metadata/marshal.h"
#include "mono/metadata/metadata-internals.h"
#include "mono/metadata/metadata.h"
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Type.h>

namespace mono {

/*
 * III.3.41  ldftn - load method pointer
 *
 *   Format        Assembly Format   Description
 *   FE 06 <T>     ldftn method      Push a pointer to a method referenced by method,
 *                                   on the stack.
 *
 * Stack Transition:
 *
 *   ... -> ..., ftn
 *
 * Description:
 *
 *   The ldftn instruction pushes a method pointer (§II.14.5) to the native code
 *   implementing the method described by method (a metadata token, either a methoddef
 *   or methodref (see Partition II)), or to some other implementation-specific
 *   description of method (see Note) onto the stack). The value pushed can be called
 *   using the calli instruction if it references a managed method (or a stub that
 *   transitions from managed to unmanaged code). It may also be used to construct a
 *   delegate, stored in a variable, etc.
 *
 *   The CLI resolves the method pointer according to the rules specified in
 *   §I.12.4.1.3 (Computed destinations), except that the destination is computed with
 *   respect to the class specified by method.
 *
 *   The value returned points to native code (see Note) using the calling convention
 *   specified by method. Thus a method pointer can be passed to unmanaged native code
 *   (e.g., as a callback routine). Note that the address computed by this instruction
 *   can be to a thunk produced specially for this purpose (for example, to re-enter
 *   the CIL interpreter when a native version of the method isn't available).
 *
 * Exceptions:
 *
 *   System.MethodAccessException can be thrown when there is an invalid attempt to
 *   access a non-public method.
 *
 * Correctness:
 *
 *   Correct CIL requires that method is a valid methoddef or methodref token.
 */
llvm::Error
MethodLLVMEmitter::emit_ldftn (MonoIrBuilder &builder, uint32_t token)
{
	llvm::Expected<MonoMethod *> target = resolve_method (token);
	if (!target)
		return target.takeError ();

	/*
	 * The declaration is the address: the engine resolves the function symbol to
	 * the method's entry point, which is exactly what the spec asks this to push.
	 */
	llvm::Expected<llvm::Function *> declaration = create_method_decl (*target);
	if (!declaration)
		return declaration.takeError ();

	push_stack (*declaration, mono_get_int_type ());
	return llvm::Error::success ();
}

/*
 * III.4.18  ldvirtftn - load a virtual method pointer
 *
 *   Format        Assembly Format    Description
 *   FE 07 <T>     ldvirtftn method   Push address of virtual method method on the
 *                                    stack.
 *
 * Stack Transition:
 *
 *   ..., object -> ..., ftn
 *
 * Description:
 *
 *   The ldvirtftn instruction pushes a method pointer (§II.14.5) to the native code
 *   implementing the virtual method associated with object and described by the
 *   method reference method (a metadata token, a methoddef, methodref or methodspec;
 *   see Partition II), or to some other implementation-specific description of the
 *   method associated with object (see Note), onto the stack. The value pushed can be
 *   called using the calli instruction if it references a managed method (or a stub
 *   that transitions from managed to unmanaged code). It may also be used to
 *   construct a delegate, stored in a variable, etc.
 *
 * Exceptions:
 *
 *   System.MethodAccessException can be thrown when there is an invalid attempt to
 *   access a non-public method.
 *
 *   System.NullReferenceException is thrown if object is null.
 *
 * Correctness:
 *
 *   Correct CIL ensures that method is a valid methoddef, methodref or methodspec
 *   token. Also that method references a non-static method that is defined for
 *   object.
 */
llvm::Error
MethodLLVMEmitter::emit_ldvirtftn (MonoIrBuilder &builder, uint32_t token)
{
	llvm::Expected<MonoMethod *> target = resolve_method (token);
	if (!target)
		return target.takeError ();

	if (stack.empty ())
		return unbalanced_stack (1);

	StackValue obj = get_stack (0);
	StackType obj_type = stack_type (obj.type);

	if (obj_type != ObjectRef)
		return invalid_il (llvm::Twine ("ldvirtftn is not defined for operand type ")
		                   + describe (obj.type, obj_type));

	/*
	 * Which slot the method lands in - vtable, IMT, a generic virtual resolver -
	 * is the runtime's business, so the lookup is its helper rather than a slot
	 * load spelled out here.
	 */
	emit_null_check (builder, obj.value);

	llvm::Expected<llvm::Function *> lookup =
		icall_wrapper_decl (MONO_JIT_ICALL_mono_ldvirtfn);

	if (!lookup)
		return lookup.takeError ();

	llvm::Value *ftn = emit_protected_call (
		builder, *lookup,
		adapt_to_callee (builder, *lookup, {obj.value, method_symbol (*target)}));

	pop_stack (1);
	push_stack (ftn, mono_get_int_type ());
	return llvm::Error::success ();
}

/*
 * III.3.20  calli - indirect method call
 *
 *   Format     Assembly Format        Description
 *   29 <T>     calli callsitedescr    Call method indicated on the stack with
 *                                     arguments described by callsitedescr.
 *
 * Stack Transition:
 *
 *   ..., arg0, arg1 ... argN, ftn -> ..., retVal (not always returned)
 *
 * Description:
 *
 *   The calli instruction calls ftn (a pointer to a method entry point) with the
 *   arguments arg0 ... argN. The types of these arguments are described by the
 *   signature callsitedescr. (See Partition I for a description of the CIL calling
 *   sequence.) The calli instruction can be immediately preceded by a tail. prefix to
 *   specify that the current method state should be released before transferring
 *   control.
 *
 *   The ftn argument must be a method pointer to a method that can be legitimately
 *   called with the arguments described by callsitedescr (a metadata token for a
 *   stand-alone signature). Such a pointer can be created using the ldftn or
 *   ldvirtftn instructions, or could have been passed in from native code.
 *
 *   The standalone signature specifies the number and type of parameters being
 *   passed, as well as the calling convention (See Partition II) The calling
 *   convention is not checked dynamically, so code that uses a calli instruction will
 *   not work correctly if the destination does not actually use the specified calling
 *   convention.
 *
 *   The arguments are placed on the stack in left-to-right order. That is, the first
 *   argument is computed and placed on the stack, then the second argument, and so
 *   on. The argument-building code sequence for an instance or virtual method shall
 *   push that instance reference (the this pointer, which shall not be null) first.
 *   [Note: for calls to methods on value types, the this pointer is a managed
 *   pointer, not an instance reference. §I.8.6.1.5. end note]
 *
 *   The arguments are passed as though by implicit starg (§III.3.61) instructions,
 *   see Implicit argument coercion §III.1.6.
 *
 *   calli pops the this pointer, if any, and the arguments off the evaluation stack
 *   before calling the method. If the method has a return value, it is pushed on the
 *   stack upon method completion. On the callee side, the arg0 parameter/this pointer
 *   is accessed as argument 0, arg1 as argument 1, and so on.
 *
 * Exceptions:
 *
 *   System.SecurityException can be thrown if the system security does not grant the
 *   caller access to the called method. The security check can occur when the CIL is
 *   converted to native code rather than at runtime.
 *
 * Correctness:
 *
 *   Correct CIL requires that the function pointer contains the address of a method
 *   whose signature is method-signature compatible-with that specified by
 *   callsitedescr and that the arguments correctly correspond to the types of the
 *   destination function's this pointer, if required, and parameters.
 */
llvm::Error
MethodLLVMEmitter::emit_calli (MonoIrBuilder &builder, uint32_t token)
{
	ERROR_DECL (metadata_error);
	MonoMethodSignature *sig;

	if (in_wrapper ()) {
		sig = static_cast<MonoMethodSignature *> (wrapper_data (token));

		if (sig == nullptr)
			return invalid_il (llvm::Twine ("wrapper data slot ") + llvm::Twine (token)
			                   + " does not hold a call site signature");
	} else {
		sig = mono_metadata_parse_signature_checked (
			m_class_get_image (method->klass), token, metadata_error);

		if (sig == nullptr)
			return runtime_error (metadata_error);
	}

	if (MonoGenericContext *ctx = mono_method_get_context (method)) {
		sig = mono_inflate_generic_signature (sig, ctx, metadata_error);
		if (sig == nullptr)
			return runtime_error (metadata_error);
	}

	/*
	 * An unmanaged target needs a managed-to-native transition, and the runtime's
	 * indirect native-func wrapper is that transition: a managed method built for
	 * this signature that takes the function pointer as its leading argument,
	 * flips the GC state, marshals, and makes the native call. What gets emitted
	 * here is then an ordinary call to the wrapper. A signature that suppresses
	 * the transition asked for the raw call instead, and takes the plain indirect
	 * path below.
	 *
	 * Inside a wrapper the calli is the other side of that arrangement: it is the
	 * native call the wrapper exists to make, and the wrapper's own body already
	 * says everything the transition needs. Wrapping it again would ask the cache
	 * for this signature's wrapper - which is the method being translated - and
	 * emit a call to it from inside itself.
	 */
	if (sig->pinvoke && !sig->suppress_gc_transition && !in_wrapper ()) {
		if (sig->hasthis)
			return invalid_il ("an unmanaged calli signature cannot take a this");
		/*
		 * The wrapper cache keys on the signature, which for a dynamic method
		 * lives in memory the method's disposal frees.
		 */
		if (method->dynamic)
			return unsupported_il ("calli to an unmanaged target in a "
			                       "dynamic method");

		MonoMethod *wrapper = mono_marshal_get_native_func_wrapper_indirect (
			method->klass, sig, FALSE);
		llvm::Expected<llvm::Function *> declaration = create_method_decl (wrapper);
		if (!declaration)
			return declaration.takeError ();

		MonoMethodSignature *wsig = mono_method_signature_internal (wrapper);

		if (stack.empty ())
			return unbalanced_stack (1);

		llvm::Expected<llvm::Value *> ftn =
			coerce_to_location (builder, get_stack (0), wsig->params[0]);
		if (!ftn)
			return ftn.takeError ();
		pop_stack (1);

		llvm::Expected<std::vector<llvm::Value *>> args =
			pop_call_arguments (builder, sig, wsig, 1);
		if (!args)
			return args.takeError ();

		args->insert (args->begin (), *ftn);

		/*
		 * The wrapper is a managed method, so its return follows the managed
		 * convention like anyone else's - and its leading parameter is the
		 * ftn, an intptr, so a by-address slot goes in front of everything.
		 */
		llvm::Expected<llvm::AllocaInst *> vret = insert_vret_arg (wsig, *args);
		if (!vret)
			return vret.takeError ();

		llvm::Expected<const SignatureABI *> wabi = lower_signature (wsig);
		if (!wabi)
			return wabi.takeError ();

		llvm::Value *result = emit_protected_call (builder, *declaration, *args);

		apply_arg_abi (llvm::cast<llvm::CallBase> (result), **wabi, 0);
		pop_stack (sig->param_count);

		if (sig->ret->type == MONO_TYPE_VOID && !sig->ret->byref)
			return llvm::Error::success ();

		if (*vret != nullptr)
			result = builder.CreateAlignedLoad ((*vret)->getAllocatedType (),
			                                    *vret, (*vret)->getAlign ());

		result = decoerce_vtype_return (builder, result, wsig->ret, **wabi);
		push_stack (widen_to_stack (builder, result, wsig->ret),
		            stack_slot_type (wsig->ret));
		return llvm::Error::success ();
	}

	llvm::Expected<llvm::FunctionType *> type = convert_method_signature (sig);
	if (!type)
		return type.takeError ();

	if (stack.empty ())
		return unbalanced_stack (1);

	llvm::Value *ftn = get_stack (0).value;

	if (!ftn->getType ()->isPointerTy ())
		ftn = builder.CreateIntToPtr (ftn, llvm::PointerType::get (context (), 0));
	pop_stack (1);

	llvm::Expected<std::vector<llvm::Value *>> args = pop_call_arguments (builder, sig);
	if (!args)
		return args.takeError ();

	llvm::Expected<const SignatureABI *> abi = lower_signature (sig);
	if (!abi)
		return abi.takeError ();

	if (should_tail_call (sig, nullptr, *type))
		return emit_tail_call (builder, llvm::FunctionCallee (*type, ftn), **abi,
		                       *args, sig->param_count + sig->hasthis);

	llvm::Expected<llvm::AllocaInst *> vret = insert_vret_arg (sig, *args);
	if (!vret)
		return vret.takeError ();

	llvm::Value *result =
		emit_protected_call (builder, llvm::FunctionCallee (*type, ftn), *args);

	apply_arg_abi (llvm::cast<llvm::CallBase> (result), **abi, 0);
	pop_stack (sig->param_count + sig->hasthis);

	if (sig->ret->type == MONO_TYPE_VOID && !sig->ret->byref)
		return llvm::Error::success ();

	if (*vret != nullptr)
		result = builder.CreateAlignedLoad ((*vret)->getAllocatedType (), *vret,
		                                    (*vret)->getAlign ());

	result = decoerce_vtype_return (builder, result, sig->ret, **abi);
	push_stack (widen_to_stack (builder, result, sig->ret), stack_slot_type (sig->ret));
	return llvm::Error::success ();
}

} // namespace mono

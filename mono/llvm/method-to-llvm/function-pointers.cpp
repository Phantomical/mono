#include "method-to-llvm.hpp"
#include "hidden-return.hpp"
#include "runtime-error.hpp"
#include "mono/metadata/class.h"
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

	if (checks_accessibility () && !mono_method_can_access_method (method, *target)) {
		if (llvm::Error error = emit_method_access_failure (builder, *target))
			return error;
	}

	// A synchronized method hands out the locking wrapper's entry. Whoever
	// calls through this pointer has no other chance to take the lock.
	llvm::Expected<llvm::Constant *> address =
		code_address_symbol (synchronized_target (*target));
	if (!address)
		return address.takeError ();

	push_stack (*address, mono_get_int_type ());
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

	// The lookup can resolve to a vtable slot, an IMT slot, or a generic
	// virtual resolver. That choice is the runtime's business, so this call
	// uses its helper instead of a slot load written out here.
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

/// Calls the native target on the stack from a dynamic method, through the
/// managed-to-native wrapper the runtime builds for it.
///
/// A dynamic method's signature is memory that its disposal frees. An
/// ordinary method's signature instead lives in its image for as long as the
/// image does. The runtime's general wrapper cache needs that lifetime to
/// hold a signature-keyed entry safely. This function does not use that
/// cache. It builds the wrapper on the spot instead, while the signature is
/// still known to be alive. mono_get_native_calli_wrapper () takes the
/// image, the signature and the target. It returns the address of a
/// compiled wrapper for exactly that target. The call that follows is then
/// an ordinary managed call.
llvm::Error
MethodLLVMEmitter::emit_dynamic_native_calli (MonoIrBuilder &builder,
                                              MonoMethodSignature *sig)
{
	llvm::Expected<llvm::Function *> build =
		icall_wrapper_decl (MONO_JIT_ICALL_mono_get_native_calli_wrapper);
	if (!build)
		return build.takeError ();

	// The wrapper itself is a managed method, whatever the signature it wraps says.
	llvm::Expected<llvm::FunctionType *> type =
		convert_method_signature (sig, /*native=*/false);
	if (!type)
		return type.takeError ();

	if (stack.empty ())
		return unbalanced_stack (1);

	llvm::Type *ptr = llvm::PointerType::get (context (), 0);
	llvm::Value *target = get_stack (0).value;

	if (!target->getType ()->isPointerTy ())
		target = builder.CreateIntToPtr (target, ptr);
	pop_stack (1);

	llvm::Constant *image = llvm::ConstantExpr::getIntToPtr (
		builder.getInt64 (
			reinterpret_cast<uint64_t> (m_class_get_image (method->klass))),
		ptr);
	llvm::Constant *signature = llvm::ConstantExpr::getIntToPtr (
		builder.getInt64 (reinterpret_cast<uint64_t> (sig)), ptr);

	llvm::Value *wrapper = emit_protected_call (
		builder, *build,
		adapt_to_callee (builder, *build, { image, signature, target }));

	// The icall returns the address as a native int. A call target must be a pointer.
	if (!wrapper->getType ()->isPointerTy ())
		wrapper = builder.CreateIntToPtr (wrapper, ptr);

	llvm::Expected<std::vector<llvm::Value *>> args = pop_call_arguments (builder, sig);
	if (!args)
		return args.takeError ();

	// The wrapper is a method this backend published, so callers enter its stub like any other.
	llvm::Type *hidden = nullptr;
	unsigned at = 0;

	if (returns_by_hidden_pointer ((*type)->getReturnType ())) {
		hidden = (*type)->getReturnType ();
		*type = hidden_return_prototype (*type, hidden);
		at = hidden_return_index ((*type)->getNumParams ());
	}

	llvm::Value *result = emit_protected_call (
		builder, llvm::FunctionCallee (*type, wrapper), *args, {}, hidden, at);

	pop_stack (sig->param_count);

	if (sig->ret->type == MONO_TYPE_VOID && !sig->ret->byref)
		return llvm::Error::success ();

	return push_produced (builder, result, sig->ret);
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

	// A vararg callee wants its variable arguments packed into a cookie buffer.
	// Only the direct call path builds one. A calli through a pointer hands
	// the callee the raw argument list instead.
	if (sig->call_convention == MONO_CALL_VARARG)
		return unsupported_il ("calli through a vararg signature");

	/*
	 * An unmanaged target needs a managed-to-native transition. The runtime's
	 * indirect native-func wrapper is that transition. It is a managed
	 * method, built for this signature, that takes the function pointer as
	 * its leading argument. It flips the GC state, marshals the arguments
	 * and makes the native call. This function then emits an ordinary call
	 * to that wrapper. A signature that suppresses the transition asks for
	 * the raw call instead, and takes the plain indirect path below.
	 *
	 * Inside a wrapper, a calli is the other side of that same arrangement.
	 * It is the native call the wrapper exists to make. The wrapper's own
	 * body already does everything the transition needs. If the call wraps
	 * the target again, the cache returns this signature's wrapper. That
	 * wrapper is the method this function translates right now, so the call
	 * enters itself.
	 *
	 * A dynamic method is the exception. Its body is the program's own IL,
	 * never built to make a native call, so it needs the transition like any
	 * other method. It counts as a wrapper only because that is how the
	 * runtime gives an emitted body somewhere to keep its token references.
	 */
	bool dynamic_method = method->wrapper_type == MONO_WRAPPER_DYNAMIC_METHOD;

	if (sig->pinvoke && !sig->suppress_gc_transition && (!in_wrapper () || dynamic_method)) {
		if (sig->hasthis)
			return invalid_il ("an unmanaged calli signature cannot take a this");

		if (dynamic_method)
			return emit_dynamic_native_calli (builder, sig);

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
			pop_call_arguments (builder, sig);
		if (!args)
			return args.takeError ();

		args->insert (args->begin (), *ftn);

		llvm::Value *result = emit_protected_call (builder, *declaration, *args);

		pop_stack (sig->param_count);

		if (sig->ret->type == MONO_TYPE_VOID && !sig->ret->byref)
			return llvm::Error::success ();

		return push_produced (builder, result, wsig->ret);
	}

	llvm::Expected<llvm::FunctionType *> type = convert_method_signature (sig, sig->pinvoke);
	if (!type)
		return type.takeError ();

	if (stack.empty ())
		return unbalanced_stack (1);

	llvm::Value *ftn = get_stack (0).value;

	if (!ftn->getType ()->isPointerTy ())
		ftn = builder.CreateIntToPtr (ftn, llvm::PointerType::get (context (), 0));
	pop_stack (1);

	llvm::Expected<std::vector<llvm::Value *>> args =
		pop_call_arguments (builder, sig, sig->pinvoke);
	if (!args)
		return args.takeError ();

	// A native signature really does reach native code. That call crosses the
	// boundary, and LegacyAbiPass lowers it. A managed signature instead reaches
	// a method this backend published - ldftn and ldvirtftn hand out its stub -
	// so it is an ordinary call. Spelling out the hidden return pointer in the
	// prototype is then this function's job, not the pass's.
	llvm::Type *hidden = nullptr;
	unsigned at = 0;

	if (sig->pinvoke == 0 && returns_by_hidden_pointer ((*type)->getReturnType ())) {
		hidden = (*type)->getReturnType ();
		*type = hidden_return_prototype (*type, hidden);
		at = hidden_return_index ((*type)->getNumParams ());
	}

	llvm::Value *result = emit_protected_call (
		builder, llvm::FunctionCallee (*type, ftn), *args, {}, hidden, at);

	if (sig->pinvoke != 0)
		mark_legacy_call (llvm::cast<llvm::CallBase> (result), sig);
	consume_save_last_error (builder);
	pop_stack (sig->param_count + sig->hasthis);

	if (sig->ret->type == MONO_TYPE_VOID && !sig->ret->byref)
		return llvm::Error::success ();

	return push_produced (builder, result, sig->ret, sig->pinvoke != 0);
}

/*
 * III.F0.18  mono_calli_extra_arg - calli carrying a hidden context argument
 *
 * The delegate invoke wrapper calls the bound method through its published
 * method_ptr. It stacks the delegate's extra_arg on the way, right under the
 * function pointer. Only llvmonly ever fills extra_arg in, so it is always
 * null here. This function drops it, and what remains is an ordinary calli.
 */
llvm::Error
MethodLLVMEmitter::emit_mono_calli_extra_arg (MonoIrBuilder &builder, uint32_t token)
{
	if (stack.size () < 2)
		return unbalanced_stack (2);

	StackValue ftn = get_stack (0);

	pop_stack (2);
	push_stack (ftn.value, ftn.type);
	return emit_calli (builder, token);
}

} // namespace mono

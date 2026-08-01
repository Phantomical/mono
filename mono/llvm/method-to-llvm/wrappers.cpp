/**
 * \file
 * \brief The instructions only a generated body can contain.
 *
 * Wrappers are written by the runtime rather than by a compiler, and say things
 * CIL has no way to say: call this icall, push this MonoClass, read the flag
 * that says a thread has been asked to stop. They live in an opcode space of
 * mono's own behind MONO_CUSTOM_PREFIX.
 *
 * Being JIT-only makes most of them straightforward. Where mini has to describe
 * a runtime address as a patch that some later stage fills in, here the address
 * is simply known while translating, and travels as a symbol the engine
 * resolves - which keeps it out of the IR, the same way every other runtime
 * address the translator emits does.
 */

#include "method-to-llvm.hpp"
#include "runtime-error.hpp"

#include "mono/metadata/class-internals.h"
#include "mono/metadata/debug-helpers.h"
#include "mono/metadata/loader.h"

#include <llvm/IR/Function.h>
#include <llvm/IR/Type.h>

#include <string>
#include <vector>

namespace mono {

/// The external global called NAME, standing for ADDRESS.
///
/// For the runtime addresses that are not a class, a method or a field - an
/// icall's entry point, a global the runtime keeps - where the name exists only
/// so that the engine has something to resolve.
llvm::Constant *
MethodLLVMEmitter::address_symbol (const std::string &name, void *address)
{
	record_external (name, ExternalSymbol::Kind::Address, address);
	return extern_symbol (name);
}

/*
 * III.F0.00  mono_icall - call one of the runtime's own entry points
 *
 * The operand is a MonoJitICallId rather than a token: the runtime knows the
 * signature and the address, and nothing about either is in any metadata.
 */
llvm::Error
MethodLLVMEmitter::emit_mono_icall (MonoIrBuilder &builder, uint32_t id)
{
	MonoJitICallInfo *info = mono_find_jit_icall_info (static_cast<MonoJitICallId> (id));

	if (info == nullptr || info->sig == nullptr)
		return invalid_il (llvm::Twine ("icall id ") + llvm::Twine (id)
		                   + " is not one the runtime registered");

	llvm::Expected<llvm::FunctionType *> type = convert_method_signature (info->sig);
	if (!type)
		return type.takeError ();

	llvm::Expected<std::vector<llvm::Value *>> args =
		pop_call_arguments (builder, info->sig);
	if (!args)
		return args.takeError ();

	llvm::Constant *target =
		address_symbol (std::string ("mono_icall_") + info->name,
	                        const_cast<void *> (info->func));
	llvm::Value *result =
		emit_protected_call (builder, llvm::FunctionCallee (*type, target), *args);

	pop_stack (info->sig->param_count);

	if (info->sig->ret->type == MONO_TYPE_VOID && !info->sig->ret->byref)
		return llvm::Error::success ();

	push_stack (widen_to_stack (builder, result, info->sig->ret),
	            stack_slot_type (info->sig->ret));
	return llvm::Error::success ();
}

/*
 * III.F0.02  mono_ldptr - push a runtime address the wrapper was built with
 *
 * Anything the runtime chose to bake in: a function to call, a structure to
 * read. Only the wrapper's own data says what it is, so it is pushed as a
 * native int and whatever follows decides.
 */
llvm::Error
MethodLLVMEmitter::emit_mono_ldptr (MonoIrBuilder &builder, uint32_t token)
{
	void *pointer = wrapper_data (token);

	if (pointer == nullptr)
		return invalid_il (llvm::Twine ("wrapper data slot ") + llvm::Twine (token)
		                   + " does not hold a pointer");

	/*
	 * Named after the slot it came from. Two wrappers' slots are unrelated, so
	 * the method's own name is what keeps one from colliding with another.
	 */
	char *owner = mono_method_full_name (method, TRUE);
	std::string name =
		std::string ("mono_wrapper_ptr_") + owner + "_" + std::to_string (token);

	g_free (owner);
	push_stack (address_symbol (name, pointer),
	            m_class_get_byval_arg (mono_defaults.int_class));
	return llvm::Error::success ();
}

/*
 * III.F0.01  mono_objaddr - take an object reference as a managed pointer
 *
 * A reference already is the object's address, so this only changes what the
 * evaluation stack calls it - enough to let the instructions that want a
 * managed pointer accept it.
 */
llvm::Error
MethodLLVMEmitter::emit_mono_objaddr (MonoIrBuilder &builder)
{
	if (stack.size () < 1)
		return unbalanced_stack (1);

	StackValue object = get_stack (0);

	if (stack_type (object.type) != ObjectRef)
		return invalid_il (llvm::Twine ("mono_objaddr wants an object reference, not ")
		                   + describe (object.type, stack_type (object.type)));

	pop_stack (1);
	push_stack (object.value, m_class_get_this_arg (mono_defaults.object_class));
	return llvm::Error::success ();
}

/*
 * III.F0.03  mono_vtaddr - take the address of a value type on the stack
 *
 * The stack holds the value itself, so there is nothing to point at until it
 * has somewhere to live: it goes to a temporary whose address is pushed.
 */
llvm::Error
MethodLLVMEmitter::emit_mono_vtaddr (MonoIrBuilder &builder)
{
	if (stack.size () < 1)
		return unbalanced_stack (1);

	StackValue value = get_stack (0);
	llvm::Value *address = spill_to_temporary (builder, value.type);

	pop_stack (1);
	push_stack (address, m_class_get_this_arg (mono_defaults.object_class));
	return llvm::Error::success ();
}

/*
 * III.F0.0B  mono_classconst - push a MonoClass the wrapper was built with
 */
llvm::Error
MethodLLVMEmitter::emit_mono_classconst (MonoIrBuilder &builder, uint32_t token)
{
	MonoClass *klass = static_cast<MonoClass *> (wrapper_data (token));

	if (klass == nullptr)
		return invalid_il (llvm::Twine ("wrapper data slot ") + llvm::Twine (token)
		                   + " does not name a class");

	push_stack (class_symbol (klass, "mono_class_"),
	            m_class_get_byval_arg (mono_defaults.int_class));
	return llvm::Error::success ();
}

/*
 * III.F0.21  mono_methodconst - push a MonoMethod the wrapper was built with
 */
llvm::Error
MethodLLVMEmitter::emit_mono_methodconst (MonoIrBuilder &builder, uint32_t token)
{
	MonoMethod *target = static_cast<MonoMethod *> (wrapper_data (token));

	if (target == nullptr)
		return invalid_il (llvm::Twine ("wrapper data slot ") + llvm::Twine (token)
		                   + " does not name a method");

	push_stack (method_symbol (target), m_class_get_byval_arg (mono_defaults.int_class));
	return llvm::Error::success ();
}

} // namespace mono

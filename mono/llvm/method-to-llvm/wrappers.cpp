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
#include "mono/metadata/object-internals.h"
#include "mono/utils/mono-memory-model.h"
#include "mono/utils/mono-tls.h"

#include <llvm/IR/Function.h>
#include <llvm/IR/InlineAsm.h>
#include <llvm/IR/Intrinsics.h>
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
 * A save_lmf wrapper links a frame onto the thread's LMF chain, which is how a
 * stack walk that starts in native code finds its way back to managed frames:
 * the walker reads the caller ip out of *(lmf->rsp - 8) and carries on from
 * this frame's own unwind info (mono_arch_unwind_frame, exceptions-amd64.c).
 *
 * Only the linking and the two register values are emitted here, exactly what
 * mini's lmf_ir mode records. rsp has to be the frame's settled value - the
 * one live at the transition call - which stacksave reads after the prologue
 * has reserved everything, since codegen never moves rsp again inside a frame
 * without dynamic allocas. frameaddress pins rbp the same way.
 */
llvm::Error
MethodLLVMEmitter::emit_push_lmf (MonoIrBuilder &builder)
{
	MonoJitICallInfo *info = mono_find_jit_icall_info (
		mono_get_tls_key_to_jit_icall_id (TLS_KEY_LMF_ADDR));

	if (info == nullptr || info->func == nullptr)
		return invalid_il ("the LMF thread-local's getter is not registered");

	llvm::Type *ptr = llvm::PointerType::get (context (), 0);
	llvm::Type *i8 = builder.getInt8Ty ();
	llvm::Align align (TARGET_SIZEOF_VOID_P);

	/*
	 * Unwinding through the LMF hop zeroes every callee-saved register and
	 * rebuilds them from this frame's own unwind info
	 * (mono_arch_unwind_frame, exceptions-amd64.c) - which can only restore
	 * what the frame saved. Clobbering the whole file makes the prologue
	 * save all of it, the way mini's save_lmf prologues do.
	 */
	builder.CreateCall (llvm::InlineAsm::get (
		llvm::FunctionType::get (builder.getVoidTy (), false), "",
		"~{rbx},~{r12},~{r13},~{r14},~{r15}", /*hasSideEffects=*/true));

	llvm::AllocaInst *slot = builder.CreateAlloca (
		llvm::ArrayType::get (i8, sizeof (MonoLMF)), nullptr, "lmf");

	slot->setAlignment (align);
	lmf_slot = slot;

	lmf_addr = builder.CreateCall (
		llvm::FunctionCallee (
			llvm::FunctionType::get (ptr, false),
			address_symbol (std::string ("mono_icall_") + info->name,
	                                const_cast<void *> (info->func))),
		{}, "lmf_addr");

	llvm::Value *previous = builder.CreateAlignedLoad (ptr, lmf_addr, align);

	builder.CreateAlignedStore (
		previous,
		builder.CreateConstInBoundsGEP1_32 (
			i8, slot, MONO_STRUCT_OFFSET (MonoLMF, previous_lmf)),
		align);
	builder.CreateAlignedStore (
		builder.CreatePtrToInt (
			builder.CreateIntrinsic (llvm::Intrinsic::frameaddress, { ptr },
		                                 { builder.getInt32 (0) }),
			builder.getInt64Ty ()),
		builder.CreateConstInBoundsGEP1_32 (i8, slot,
	                                            MONO_STRUCT_OFFSET (MonoLMF, rbp)),
		align);
	builder.CreateAlignedStore (
		builder.CreatePtrToInt (builder.CreateStackSave (),
	                                builder.getInt64Ty ()),
		builder.CreateConstInBoundsGEP1_32 (i8, slot,
	                                            MONO_STRUCT_OFFSET (MonoLMF, rsp)),
		align);
	builder.CreateAlignedStore (slot, lmf_addr, align);
	return llvm::Error::success ();
}

void
MethodLLVMEmitter::emit_pop_lmf (MonoIrBuilder &builder)
{
	llvm::Type *ptr = llvm::PointerType::get (context (), 0);
	llvm::Align align (TARGET_SIZEOF_VOID_P);

	llvm::Value *previous = builder.CreateAlignedLoad (
		ptr,
		builder.CreateConstInBoundsGEP1_32 (
			builder.getInt8Ty (), lmf_slot,
			MONO_STRUCT_OFFSET (MonoLMF, previous_lmf)),
		align);

	builder.CreateAlignedStore (previous, lmf_addr, align);
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

	/* The icall is a C function; its signature says so. */
	mark_legacy_call (llvm::cast<llvm::CallBase> (result), info->sig);
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
 * III.F0.19  mono_lddomain - push the domain the wrapper was built for
 *
 * A native-to-managed wrapper opens by attaching the thread it was entered on,
 * and has to say which domain to attach it to. It cannot ask the thread: an
 * unattached one has no answer to give, which is the whole reason the wrapper
 * is attaching it. The domain is settled when the wrapper is compiled, so it
 * is baked in here - the same constant mini folds in.
 */
llvm::Error
MethodLLVMEmitter::emit_mono_lddomain (MonoIrBuilder &builder)
{
	/*
	 * One name for every domain: a linker belongs to a single domain and
	 * resolves the symbol to that domain, so two domains' copies of the same
	 * wrapper never share a binding for it.
	 */
	push_stack (address_symbol ("mono_domain", cfg->domain),
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
	StackType type = stack_type (object.type);

	/*
	 * Anything address-shaped is admitted, not just a reference: an alloc
	 * wrapper builds its object out of raw pointer arithmetic, so what it
	 * relabels here has been a native int the whole way.
	 */
	if (type != ObjectRef && type != NativeInt && type != ManagedPtr)
		return invalid_il (llvm::Twine ("mono_objaddr wants an address, not ")
		                   + describe (object.type, type));

	llvm::Value *value = object.value;

	if (!value->getType ()->isPointerTy ())
		value = builder.CreateIntToPtr (value,
		                                llvm::PointerType::get (context (), 0));

	pop_stack (1);
	push_stack (value, m_class_get_this_arg (mono_defaults.object_class));
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
 * III.F0.1E  mono_ld_delegate_method_ptr - load a delegate's bound entry point
 *
 * Pops a delegate reference and pushes its method_ptr, the entry the runtime
 * published into the delegate when it was bound. The invoke wrapper feeds it
 * straight into a calli.
 */
llvm::Error
MethodLLVMEmitter::emit_mono_ld_delegate_method_ptr (MonoIrBuilder &builder)
{
	if (stack.size () < 1)
		return unbalanced_stack (1);

	llvm::Value *delegate = get_stack (0).value;
	llvm::Value *ftn = builder.CreateAlignedLoad (
		llvm::PointerType::get (context (), 0),
		builder.CreateGEP (builder.getInt8Ty (), delegate,
	                           builder.getInt32 (MONO_STRUCT_OFFSET (MonoDelegate, method_ptr))),
		llvm::Align (TARGET_SIZEOF_VOID_P));

	pop_stack (1);
	push_stack (ftn, m_class_get_byval_arg (mono_defaults.int_class));
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
 * III.F0.14  mono_jit_icall_addr - push a runtime entry point's address
 *
 * The operand is a MonoJitICallId, as in mono_icall; this pushes the entry
 * point where that calls it.
 */
llvm::Error
MethodLLVMEmitter::emit_mono_jit_icall_addr (MonoIrBuilder &builder, uint32_t id)
{
	MonoJitICallInfo *info = mono_find_jit_icall_info (static_cast<MonoJitICallId> (id));

	if (info == nullptr || info->func == nullptr)
		return invalid_il (llvm::Twine ("icall id ") + llvm::Twine (id)
		                   + " is not one the runtime registered");

	push_stack (address_symbol (std::string ("mono_icall_") + info->name,
	                            const_cast<void *> (info->func)),
	            m_class_get_byval_arg (mono_defaults.int_class));
	return llvm::Error::success ();
}

/*
 * III.F0.08  mono_icall_addr - push an internal call's native entry point
 *
 * The operand names a MonoMethod in the wrapper's data; what gets pushed is the
 * C function the runtime registered as that method's implementation.
 */
llvm::Error
MethodLLVMEmitter::emit_mono_icall_addr (MonoIrBuilder &builder, uint32_t token)
{
	MonoMethod *target = static_cast<MonoMethod *> (wrapper_data (token));

	if (target == nullptr)
		return invalid_il (llvm::Twine ("wrapper data slot ") + llvm::Twine (token)
		                   + " does not name a method");

	void *address = mono_lookup_internal_call (target);

	if (address == nullptr) {
		char *name = mono_method_full_name (target, TRUE);
		llvm::Error refusal = invalid_il (
			llvm::Twine (name) + " has no registered internal call");

		g_free (name);
		return refusal;
	}

	char *name = mono_method_full_name (target, TRUE);
	std::string symbol = std::string ("mono_icall_impl_") + name;

	g_free (name);
	push_stack (address_symbol (symbol, address),
	            m_class_get_byval_arg (mono_defaults.int_class));
	return llvm::Error::success ();
}

/*
 * III.F0.16  mono_tls - push one of the runtime's per-thread variables
 *
 * The operand is a MonoTlsKey. The runtime registers a getter for each as a
 * jit icall, so the value comes from a call rather than from reproducing the
 * thread-local access sequence here.
 */
llvm::Error
MethodLLVMEmitter::emit_mono_tls (MonoIrBuilder &builder, uint32_t key)
{
	if (key >= TLS_KEY_NUM)
		return invalid_il (llvm::Twine (key) + " is not a thread-local the runtime keeps");

	MonoJitICallInfo *info = mono_find_jit_icall_info (
		mono_get_tls_key_to_jit_icall_id (static_cast<MonoTlsKey> (key)));

	if (info == nullptr || info->func == nullptr)
		return invalid_il ("the thread-local's getter is not registered");

	llvm::Type *ptr = llvm::PointerType::get (context (), 0);
	llvm::FunctionType *type = llvm::FunctionType::get (ptr, false);
	llvm::Value *value = builder.CreateCall (llvm::FunctionCallee (
		type, address_symbol (std::string ("mono_icall_") + info->name,
	                              const_cast<void *> (info->func))));

	push_stack (value, m_class_get_byval_arg (mono_defaults.int_class));
	return llvm::Error::success ();
}

/*
 * III.F0.17  mono_atomic_store_i4 - store an int32 with ordering
 *
 * The operand is a MonoMemoryBarrierKind saying how strongly the store orders
 * against its neighbors. The stack carries the address below the value.
 */
llvm::Error
MethodLLVMEmitter::emit_mono_atomic_store_i4 (MonoIrBuilder &builder, uint32_t barrier)
{
	if (stack.size () < 2)
		return unbalanced_stack (2);

	StackValue value = get_stack (0);
	StackValue address = get_stack (1);

	if (stack_type (value.type) != Int32)
		return invalid_il (llvm::Twine ("mono_atomic_store_i4 stores an int32, not ")
		                   + describe (value.type, stack_type (value.type)));

	llvm::AtomicOrdering ordering;

	switch (barrier) {
	case MONO_MEMORY_BARRIER_NONE:
		ordering = llvm::AtomicOrdering::Monotonic;
		break;
	case MONO_MEMORY_BARRIER_REL:
		ordering = llvm::AtomicOrdering::Release;
		break;
	case MONO_MEMORY_BARRIER_SEQ:
		ordering = llvm::AtomicOrdering::SequentiallyConsistent;
		break;
	default:
		return invalid_il (llvm::Twine ("a store cannot order as barrier kind ")
		                   + llvm::Twine (barrier));
	}

	llvm::Value *target = address.value;

	if (!target->getType ()->isPointerTy ())
		target = builder.CreateIntToPtr (target,
		                                 llvm::PointerType::get (context (), 0));

	llvm::StoreInst *store =
		builder.CreateAlignedStore (value.value, target, llvm::Align (4));

	store->setAtomic (ordering);
	pop_stack (2);
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

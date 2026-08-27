/**
 * \file
 * \brief Compiling Environment.CurrentManagedThreadId as a read of the thread's TLS.
 */

#include "method-to-llvm.hpp"

// class-internals.h brings in jit-icall-reg.h, which has no include guard.
#include "mono/metadata/class-internals.h"
#include "mono/metadata/metadata.h"
#include "mono/metadata/object-internals.h"

#include <llvm/IR/Attributes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Metadata.h>
#include <llvm/Support/ModRef.h>

#include <string_view>

namespace mono {

bool
is_current_managed_thread_id (MonoMethod *target, MonoMethodSignature *sig)
{
	if (sig == nullptr || sig->hasthis || sig->param_count != 0)
		return false;
	if (sig->ret->byref || sig->ret->type != MONO_TYPE_I4)
		return false;

	MonoClass *klass = target->klass;

	if (m_class_get_image (klass) != mono_get_corlib ())
		return false;
	if (std::string_view (m_class_get_name_space (klass)) != "System")
		return false;
	if (std::string_view (m_class_get_name (klass)) != "Environment")
		return false;

	return std::string_view (target->name) == "get_CurrentManagedThreadId";
}

/// Reads the calling thread's MonoInternalThread out of TLS.
llvm::Expected<llvm::Value *>
MethodLLVMEmitter::emit_internal_thread (MonoIrBuilder &builder)
{
	// mini_init () (mono/mini/mini-runtime.c) registers this one with no wrapper.
	llvm::Expected<llvm::FunctionCallee> tls = raw_icall_decl (
		mono_find_jit_icall_info (MONO_JIT_ICALL_mono_tls_get_thread_extern));

	if (!tls)
		return tls.takeError ();

	// The MonoInternalThread is the same for as long as the thread is attached,
	// and managed code runs on no other kind of thread. So LLVM can share one
	// call between the sites the IL wrote, and move one onto a path that had
	// none. Both stay on the thread the site was reached from.
	if (auto *decl = llvm::dyn_cast<llvm::Function> (tls->getCallee ())) {
		decl->setDoesNotThrow ();
		decl->setWillReturn ();
		decl->setMemoryEffects (llvm::MemoryEffects::none ());
		decl->addFnAttr (llvm::Attribute::Speculatable);
	}

	llvm::CallInst *thread = builder.CreateCall (
		*tls, llvm::ArrayRef<llvm::Value *> (), "internal_thread");

	mark_mono_call (thread);

	// The signature says native int, and a field read wants a pointer.
	if (thread->getType ()->isPointerTy ())
		return thread;

	return builder.CreateIntToPtr (thread, llvm::PointerType::get (context (), 0));
}

/// Pushes the calling thread's managed id, in place of a call to the property.
llvm::Error
MethodLLVMEmitter::emit_current_managed_thread_id (MonoIrBuilder &builder,
                                                   MonoMethodSignature *sig)
{
	llvm::Expected<llvm::Value *> address = emit_internal_thread (builder);

	if (!address)
		return address.takeError ();

	llvm::Value *slot = builder.CreateGEP (
		builder.getInt8Ty (), *address,
		builder.getInt32 (MONO_STRUCT_OFFSET (MonoInternalThread, managed_id)));
	llvm::LoadInst *id = builder.CreateAlignedLoad (builder.getInt32Ty (), slot,
	                                                llvm::Align (4), "managed_id");

	// init_internal_thread_object () (mono/metadata/threads.c) writes managed_id
	// once, before the thread it belongs to is reachable.
	id->setMetadata (llvm::LLVMContext::MD_invariant_load,
	                 llvm::MDNode::get (context (), {}));

	return push_produced (builder, id, sig->ret);
}

/// Builds the address of the thread static at index and offset in the calling
/// thread's static data.
llvm::Expected<llvm::Value *>
MethodLLVMEmitter::thread_static_address (MonoIrBuilder &builder, uint32_t index,
                                          uint32_t offset)
{
	llvm::Expected<llvm::Value *> thread = emit_internal_thread (builder);

	if (!thread)
		return thread.takeError ();

	llvm::Type *ptr = llvm::PointerType::get (context (), 0);
	llvm::MDNode *invariant = llvm::MDNode::get (context (), {});

	/*
	 * Both pointers below are written once and never rewritten.
	 * mono_alloc_static_data () fills thread->static_data and each of its block
	 * slots only where it finds NULL, and mono_free_static_data () clears them at
	 * thread teardown. So !invariant.load holds for as long as the thread can run
	 * managed code, which is what lets LLVM share one address between the sites in
	 * a body and lift it out of a loop. Without the marks the store a body makes
	 * to the field itself may alias the block slot, and both loads stay in the
	 * loop.
	 *
	 * Neither load is null-checked, because both are non-NULL before compiled
	 * code can reach them. mono_alloc_special_static_data () walks every attached
	 * thread under mono_threads_lock (), and a thread attaching allocates up to
	 * the current high-water mark under the same lock. So a block exists for any
	 * offset a translation can read, and get_thread_static_data () rests on the
	 * same invariant.
	 */
	llvm::Value *blocks_at = builder.CreateGEP (
		builder.getInt8Ty (), *thread,
		builder.getInt32 (MONO_STRUCT_OFFSET (MonoInternalThread, static_data)));
	llvm::LoadInst *blocks = builder.CreateAlignedLoad (
		ptr, blocks_at, llvm::Align (sizeof (gpointer)), "static_data");

	blocks->setMetadata (llvm::LLVMContext::MD_invariant_load, invariant);

	llvm::Value *block_at = builder.CreateGEP (ptr, blocks, builder.getInt32 (index));
	llvm::LoadInst *block = builder.CreateAlignedLoad (
		ptr, block_at, llvm::Align (sizeof (gpointer)), "static_block");

	block->setMetadata (llvm::LLVMContext::MD_invariant_load, invariant);

	return builder.CreateGEP (builder.getInt8Ty (), block, builder.getInt32 (offset));
}

} // namespace mono

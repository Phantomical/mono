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

/// Pushes the calling thread's managed id, in place of a call to the property.
llvm::Error
MethodLLVMEmitter::emit_current_managed_thread_id (MonoIrBuilder &builder,
                                                   MonoMethodSignature *sig)
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
	llvm::Value *address = thread;

	if (!address->getType ()->isPointerTy ())
		address = builder.CreateIntToPtr (address,
		                                  llvm::PointerType::get (context (), 0));

	llvm::Value *slot = builder.CreateGEP (
		builder.getInt8Ty (), address,
		builder.getInt32 (MONO_STRUCT_OFFSET (MonoInternalThread, managed_id)));
	llvm::LoadInst *id = builder.CreateAlignedLoad (builder.getInt32Ty (), slot,
	                                                llvm::Align (4), "managed_id");

	// init_internal_thread_object () (mono/metadata/threads.c) writes managed_id
	// once, before the thread it belongs to is reachable.
	id->setMetadata (llvm::LLVMContext::MD_invariant_load,
	                 llvm::MDNode::get (context (), {}));

	return push_produced (builder, id, sig->ret);
}

} // namespace mono

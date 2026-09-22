/**
 * \file
 * \brief Compiling Object.GetHashCode with its cached hash read in front.
 */

#include "method-to-llvm.hpp"

#include "../internal-loads.hpp"

#include "mono/metadata/monitor.h"

#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instructions.h>

namespace mono {

/* Read the cached hash from the thin or inflated lock word. An object with no
 * cached hash falls back to mono_object_hash_internal (). */
llvm::Error
MethodLLVMEmitter::emit_hash_code_fast_path (MonoIrBuilder &builder, MonoMethod *callee_method,
                                             MonoMethodSignature *sig)
{
	llvm::Expected<std::vector<llvm::Value *>> args = pop_call_arguments (builder, sig);
	if (!args)
		return args.takeError ();

	llvm::Value *object = (*args)[0];

	llvm::Expected<llvm::Function *> slow_decl =
		create_method_decl (icall_wrapper_target (callee_method));
	if (!slow_decl)
		return slow_decl.takeError ();

	llvm::Type *i8_ty = builder.getInt8Ty ();
	llvm::Type *word_ty = builder.getIntNTy (TARGET_SIZEOF_VOID_P * 8);

	// RuntimeHelpers.GetHashCode () reaches InternalGetHashCode () too, with a
	// receiver that can be null. mono_object_hash_internal () returns 0 for
	// null instead of reading its lock word.
	llvm::Value *is_null = builder.CreateICmpEQ (
		object, llvm::ConstantPointerNull::get (llvm::PointerType::get (context (), 0)),
		"is_null");

	llvm::BasicBlock *has_this = llvm::BasicBlock::Create (context (), "hash_has_this", function);
	llvm::BasicBlock *is_null_bb = llvm::BasicBlock::Create (context (), "hash_null", function);
	llvm::BasicBlock *cached = llvm::BasicBlock::Create (context (), "hash_cached", function);
	llvm::BasicBlock *thin = llvm::BasicBlock::Create (context (), "hash_thin", function);
	llvm::BasicBlock *fat = llvm::BasicBlock::Create (context (), "hash_fat", function);
	llvm::BasicBlock *uncached = llvm::BasicBlock::Create (context (), "hash_uncached", function);
	llvm::BasicBlock *done = llvm::BasicBlock::Create (context (), "hash_done", function);

	builder.CreateCondBr (is_null, is_null_bb, has_this);

	builder.SetInsertPoint (is_null_bb);
	builder.CreateBr (done);

	builder.SetInsertPoint (has_this);
	llvm::Value *sync_field = builder.CreateGEP (
		i8_ty, object, builder.getInt32 (MONO_STRUCT_OFFSET (MonoObject, synchronisation)));
	// A monitor operation and the first hash both write this word.
	llvm::Value *lock_word = mark_internal_load (
		builder.CreateAlignedLoad (word_ty, sync_field, llvm::Align (TARGET_SIZEOF_VOID_P),
	                                   "lock_word"),
		object_header_tbaa_leaf, InternalLife::varies);

	llvm::Value *has_hash = builder.CreateICmpNE (
		builder.CreateAnd (lock_word, llvm::ConstantInt::get (word_ty, LOCK_WORD_HAS_HASH)),
		llvm::ConstantInt::get (word_ty, 0), "has_hash");

	builder.CreateCondBr (has_hash, cached, uncached);

	builder.SetInsertPoint (cached);
	llvm::Value *is_inflated = builder.CreateICmpNE (
		builder.CreateAnd (lock_word, llvm::ConstantInt::get (word_ty, LOCK_WORD_INFLATED)),
		llvm::ConstantInt::get (word_ty, 0), "is_inflated");
	builder.CreateCondBr (is_inflated, fat, thin);

	builder.SetInsertPoint (thin);
	llvm::Value *thin_hash = builder.CreateTrunc (
		builder.CreateLShr (lock_word, LOCK_WORD_HASH_SHIFT), builder.getInt32Ty (), "thin_hash");
	builder.CreateBr (done);

	builder.SetInsertPoint (fat);
	llvm::Value *sync_ptr = builder.CreateIntToPtr (
		builder.CreateAnd (lock_word,
		                   llvm::ConstantInt::get (word_ty, ~(uint64_t) LOCK_WORD_STATUS_MASK)),
		llvm::PointerType::get (context (), 0), "sync_ptr");
	llvm::Value *fat_hash = builder.CreateAlignedLoad (
		builder.getInt32Ty (),
		builder.CreateGEP (i8_ty, sync_ptr,
		                   builder.getInt32 (MONO_STRUCT_OFFSET (MonoThreadsSync, hash_code))),
		llvm::Align (4), "fat_hash");
	builder.CreateBr (done);

	builder.SetInsertPoint (uncached);
	llvm::Value *slow_hash = emit_protected_call (
		builder, *slow_decl, adapt_to_callee (builder, *slow_decl, *args));

	// An invoke may leave the builder in a continuation block.
	llvm::BasicBlock *uncached_end = builder.GetInsertBlock ();
	builder.CreateBr (done);

	builder.SetInsertPoint (done);
	llvm::PHINode *hash = builder.CreatePHI (builder.getInt32Ty (), 4, "hash");
	hash->addIncoming (llvm::ConstantInt::get (builder.getInt32Ty (), 0), is_null_bb);
	hash->addIncoming (thin_hash, thin);
	hash->addIncoming (fat_hash, fat);
	hash->addIncoming (slow_hash, uncached_end);

	pop_stack (sig->param_count);
	return push_produced (builder, hash, sig->ret);
}

/* Compute the address-based hash directly when the collector never moves
 * objects. Moving collectors use the cached-hash path above. */
llvm::Error
MethodLLVMEmitter::emit_hash_code_pointer_fast_path (MonoIrBuilder &builder,
                                                      MonoMethodSignature *sig)
{
	llvm::Expected<std::vector<llvm::Value *>> args = pop_call_arguments (builder, sig);
	if (!args)
		return args.takeError ();

	llvm::Value *object = (*args)[0];

	llvm::Type *i32_ty = builder.getInt32Ty ();
	llvm::Type *word_ty = builder.getIntNTy (TARGET_SIZEOF_VOID_P * 8);

	// GPOINTER_TO_UINT () truncates the address to 32 bits before the
	// shift. Shifting first and truncating after gives a different value
	// whenever the address holds bits above bit 31. 3 is monitor.c's own
	// MONO_OBJECT_ALIGNMENT_SHIFT, private to that file, so the value is
	// copied here rather than included.
	llvm::Value *addr = builder.CreatePtrToInt (object, word_ty, "addr");
	llvm::Value *addr_lo32 = builder.CreateTrunc (addr, i32_ty, "addr_lo32");
	llvm::Value *shifted = builder.CreateLShr (addr_lo32, 3, "addr_shr");
	llvm::Value *hash = builder.CreateMul (
		shifted, llvm::ConstantInt::get (i32_ty, 2654435761u), "addr_hash");

	pop_stack (sig->param_count);
	return push_produced (builder, hash, sig->ret);
}

} // namespace mono

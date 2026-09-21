/**
 * \file
 * \brief Compiling the three System.Threading.Volatile overloads that are
 * their own icalls rather than a reinterpret through Unsafe.
 *
 * Long, ulong and double are 8 bytes wide, which this target already reads
 * and writes in one instruction. Every narrower overload instead gets there
 * by reinterpreting through Unsafe.As<T, VolatileT>
 * (System.Threading/Volatile.cs).
 */

#include "method-to-llvm.hpp"

#include "mono/metadata/metadata.h"

namespace mono {

/// Read (ref long/ulong/double): a plain 8-byte load is already atomic here,
/// so the acquire ordering I.12.6.7 asks for rides the load itself.
llvm::Error
MethodLLVMEmitter::emit_volatile_read_wide (MonoIrBuilder &builder, MonoMethodSignature *sig)
{
	llvm::Expected<std::vector<llvm::Value *>> args = pop_call_arguments (builder, sig);
	if (!args)
		return args.takeError ();

	llvm::Type *type = sig->params[0]->type == MONO_TYPE_R8 ? builder.getDoubleTy ()
	                                                        : builder.getInt64Ty ();
	llvm::LoadInst *value = builder.CreateAlignedLoad (type, (*args)[0], llvm::Align (8));

	value->setAtomic (llvm::AtomicOrdering::Acquire);

	pop_stack (sig->param_count);
	return push_produced (builder, value, sig->ret);
}

/// Write (ref long/ulong/double, T): the release-ordered counterpart.
llvm::Error
MethodLLVMEmitter::emit_volatile_write_wide (MonoIrBuilder &builder, MonoMethodSignature *sig)
{
	llvm::Expected<std::vector<llvm::Value *>> args = pop_call_arguments (builder, sig);
	if (!args)
		return args.takeError ();

	llvm::StoreInst *store =
		builder.CreateAlignedStore ((*args)[1], (*args)[0], llvm::Align (8));

	store->setAtomic (llvm::AtomicOrdering::Release);

	pop_stack (sig->param_count);
	return llvm::Error::success ();
}

} // namespace mono

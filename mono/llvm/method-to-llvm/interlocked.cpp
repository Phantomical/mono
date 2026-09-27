/**
 * \file
 * \brief Compiling System.Threading.Interlocked as the atomic instruction it
 * names, in place of the managed-to-native transition every overload takes
 * today.
 *
 * mono's own Interlocked is sequentially consistent, so every atomic access
 * below takes AtomicOrdering::SequentiallyConsistent - a compare-exchange on
 * both its success and its failure arm. x86-64 has no weaker ordering a
 * `lock`-prefixed instruction could ask for instead.
 */

#include "method-to-llvm.hpp"
#include "../passes/gc-barrier.hpp"

#include "mono/metadata/metadata.h"

#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Instructions.h>

namespace mono {

namespace {

constexpr llvm::AtomicOrdering seq_cst = llvm::AtomicOrdering::SequentiallyConsistent;

} // namespace

/// The width a scalar Interlocked overload's own MonoType carries, or
/// nothing for the generic, reference-typed overloads this backend leaves
/// alone. The registry asks this to decide whether a call matches; this
/// file's own emitters ask it again to answer how.
std::optional<InterlockedScalarWidth>
interlocked_scalar_width (MonoType *t)
{
	switch (t->type) {
	case MONO_TYPE_I4:
		return InterlockedScalarWidth { 32, false };
	case MONO_TYPE_R4:
		return InterlockedScalarWidth { 32, true };
	case MONO_TYPE_I8:
	case MONO_TYPE_I:
	case MONO_TYPE_U:
		return InterlockedScalarWidth { 64, false };
	case MONO_TYPE_R8:
		return InterlockedScalarWidth { 64, true };
	default:
		return std::nullopt;
	}
}

/// CompareExchange (ref T, T, T) for the five scalar T's the overload set
/// carries: int, long, IntPtr, float and double. A T that is instead a
/// reference type is the generic CompareExchange<T> (ref) : class, which
/// this row declines - its own IL already reaches the object overload below.
llvm::Error
MethodLLVMEmitter::emit_interlocked_compare_exchange_scalar (MonoIrBuilder &builder,
                                                              MonoMethodSignature *sig)
{
	llvm::Expected<std::vector<llvm::Value *>> args = pop_call_arguments (builder, sig);
	if (!args)
		return args.takeError ();

	InterlockedScalarWidth width = *interlocked_scalar_width (sig->params[1]);
	llvm::Type *int_ty = builder.getIntNTy (width.bits);
	llvm::Value *location = (*args)[0];
	llvm::Value *value = (*args)[1];
	llvm::Value *comparand = (*args)[2];
	llvm::Type *original = value->getType ();

	if (width.is_float) {
		value = builder.CreateBitCast (value, int_ty);
		comparand = builder.CreateBitCast (comparand, int_ty);
	}

	llvm::AtomicCmpXchgInst *cx = builder.CreateAtomicCmpXchg (
		location, comparand, value, llvm::MaybeAlign (width.bits / 8), seq_cst, seq_cst);
	llvm::Value *old = builder.CreateExtractValue (cx, 0);

	if (width.is_float)
		old = builder.CreateBitCast (old, original);

	pop_stack (sig->param_count);
	return push_produced (builder, old, sig->ret);
}

/// The internal `int CompareExchange (ref int, int, int, ref bool)`. No
/// corlib caller reaches it today - the public three-argument overload is a
/// separate icall, not a forward to this one.
llvm::Error
MethodLLVMEmitter::emit_interlocked_compare_exchange_bool (MonoIrBuilder &builder,
                                                            MonoMethodSignature *sig)
{
	llvm::Expected<std::vector<llvm::Value *>> args = pop_call_arguments (builder, sig);
	if (!args)
		return args.takeError ();

	llvm::AtomicCmpXchgInst *cx =
		builder.CreateAtomicCmpXchg ((*args)[0], (*args)[2], (*args)[1],
		                            llvm::MaybeAlign (4), seq_cst, seq_cst);
	llvm::Value *old = builder.CreateExtractValue (cx, 0);
	llvm::Value *success = builder.CreateExtractValue (cx, 1);

	builder.CreateAlignedStore (builder.CreateZExt (success, builder.getInt8Ty ()),
	                            (*args)[3], llvm::Align (1));

	pop_stack (sig->param_count);
	return push_produced (builder, old, sig->ret);
}

/// The internal `void CompareExchange (ref object, ref object, ref object,
/// ref object)` the public generic and object-typed overloads both funnel
/// through. value and comparand arrive by reference themselves - see
/// Interlocked.cs's own comment on why - so this loads them first.
///
/// The card mark after the exchange follows the classic compiler's own
/// choice (mono/mini/tier0/intrinsics.c): it marks location for value
/// whether or not the exchange took it. That is the safe direction to be
/// wrong in. A mark ahead of a collection that then clears it, before the
/// store lands, would drop the reference. A mark behind a store that never
/// happened only costs a card scanned for nothing.
llvm::Error
MethodLLVMEmitter::emit_interlocked_compare_exchange_object (MonoIrBuilder &builder,
                                                              MonoMethodSignature *sig)
{
	llvm::Expected<std::vector<llvm::Value *>> args = pop_call_arguments (builder, sig);
	if (!args)
		return args.takeError ();

	llvm::Type *object = object_pointer_type (context ());
	llvm::Value *location = (*args)[0];
	llvm::Value *value = builder.CreateAlignedLoad (object, (*args)[1], llvm::Align (8));
	llvm::Value *comparand = builder.CreateAlignedLoad (object, (*args)[2], llvm::Align (8));

	llvm::AtomicCmpXchgInst *cx = builder.CreateAtomicCmpXchg (
		location, comparand, value, llvm::MaybeAlign (8), seq_cst, seq_cst);
	llvm::Value *old = builder.CreateExtractValue (cx, 0);

	const GcBarrierLayout &gc = current_write_barrier_layout ();

	record_barrier_symbols (gc);

	llvm::Function *barrier = gc_barrier_decl (*module, gc);

	builder.CreateCall (barrier, adapt_to_callee (builder, barrier, { location, value }));

	// result is always a fresh local of the one caller this icall has
	// (Interlocked.cs), never a field, so it owes no card of its own.
	builder.CreateAlignedStore (old, (*args)[3], llvm::Align (8));

	pop_stack (sig->param_count);
	return llvm::Error::success ();
}

/// Exchange (ref T, T) for the five scalar T's, the same set
/// emit_interlocked_compare_exchange_scalar () answers.
llvm::Error
MethodLLVMEmitter::emit_interlocked_exchange_scalar (MonoIrBuilder &builder,
                                                      MonoMethodSignature *sig)
{
	llvm::Expected<std::vector<llvm::Value *>> args = pop_call_arguments (builder, sig);
	if (!args)
		return args.takeError ();

	InterlockedScalarWidth width = *interlocked_scalar_width (sig->params[0]);
	llvm::Type *int_ty = builder.getIntNTy (width.bits);
	llvm::Value *location = (*args)[0];
	llvm::Value *value = (*args)[1];
	llvm::Type *original = value->getType ();

	if (width.is_float)
		value = builder.CreateBitCast (value, int_ty);

	llvm::Value *old = builder.CreateAtomicRMW (llvm::AtomicRMWInst::Xchg, location, value,
	                                            llvm::MaybeAlign (width.bits / 8), seq_cst);

	if (width.is_float)
		old = builder.CreateBitCast (old, original);

	pop_stack (sig->param_count);
	return push_produced (builder, old, sig->ret);
}

/// The internal `void Exchange (ref object, ref object, ref object)` the
/// public generic and object-typed Exchange overloads funnel through.
llvm::Error
MethodLLVMEmitter::emit_interlocked_exchange_object (MonoIrBuilder &builder,
                                                      MonoMethodSignature *sig)
{
	llvm::Expected<std::vector<llvm::Value *>> args = pop_call_arguments (builder, sig);
	if (!args)
		return args.takeError ();

	llvm::Value *location = (*args)[0];
	llvm::Value *value =
		builder.CreateAlignedLoad (object_pointer_type (context ()), (*args)[1], llvm::Align (8));

	llvm::Value *old = builder.CreateAtomicRMW (llvm::AtomicRMWInst::Xchg, location, value,
	                                            llvm::MaybeAlign (8), seq_cst);

	const GcBarrierLayout &gc = current_write_barrier_layout ();

	record_barrier_symbols (gc);

	llvm::Function *barrier = gc_barrier_decl (*module, gc);

	builder.CreateCall (barrier, adapt_to_callee (builder, barrier, { location, value }));

	builder.CreateAlignedStore (old, (*args)[2], llvm::Align (8));

	pop_stack (sig->param_count);
	return llvm::Error::success ();
}

/// Increment (ref int/long) and Decrement (ref int/long): an atomicrmw add
/// of 1 or -1, answering the value after the change as the two overloads
/// promise.
llvm::Error
MethodLLVMEmitter::emit_interlocked_increment_decrement (MonoIrBuilder &builder,
                                                          MonoMethodSignature *sig,
                                                          bool increment)
{
	llvm::Expected<std::vector<llvm::Value *>> args = pop_call_arguments (builder, sig);
	if (!args)
		return args.takeError ();

	unsigned bits = sig->params[0]->type == MONO_TYPE_I8 ? 64 : 32;
	llvm::Value *delta = llvm::ConstantInt::get (builder.getIntNTy (bits), increment ? 1 : -1,
	                                             /*isSigned=*/true);
	llvm::Value *before = builder.CreateAtomicRMW (llvm::AtomicRMWInst::Add, (*args)[0], delta,
	                                               llvm::MaybeAlign (bits / 8), seq_cst);
	llvm::Value *after = builder.CreateAdd (before, delta);

	pop_stack (sig->param_count);
	return push_produced (builder, after, sig->ret);
}

/// Add (ref int/long, int/long): an atomicrmw add, answering the sum as the
/// overload promises.
llvm::Error
MethodLLVMEmitter::emit_interlocked_add (MonoIrBuilder &builder, MonoMethodSignature *sig)
{
	llvm::Expected<std::vector<llvm::Value *>> args = pop_call_arguments (builder, sig);
	if (!args)
		return args.takeError ();

	unsigned bits = sig->params[0]->type == MONO_TYPE_I8 ? 64 : 32;
	llvm::Value *before = builder.CreateAtomicRMW (llvm::AtomicRMWInst::Add, (*args)[0],
	                                               (*args)[1], llvm::MaybeAlign (bits / 8),
	                                               seq_cst);
	llvm::Value *after = builder.CreateAdd (before, (*args)[1]);

	pop_stack (sig->param_count);
	return push_produced (builder, after, sig->ret);
}

/// Read (ref long): a plain 64-bit load is already atomic on this target, so
/// the sequentially consistent ordering rides an atomic load rather than a
/// no-op RMW.
llvm::Error
MethodLLVMEmitter::emit_interlocked_read (MonoIrBuilder &builder, MonoMethodSignature *sig)
{
	llvm::Expected<std::vector<llvm::Value *>> args = pop_call_arguments (builder, sig);
	if (!args)
		return args.takeError ();

	llvm::LoadInst *value =
		builder.CreateAlignedLoad (builder.getInt64Ty (), (*args)[0], llvm::Align (8));

	value->setAtomic (seq_cst);

	pop_stack (sig->param_count);
	return push_produced (builder, value, sig->ret);
}

} // namespace mono

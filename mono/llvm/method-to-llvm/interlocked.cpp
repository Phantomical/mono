/**
 * \file
 * \brief Compiling System.Threading.Interlocked as atomic IR in place of the
 * managed-to-native transition every overload otherwise costs.
 */

#include "method-to-llvm.hpp"
#include "../passes/gc-barrier.hpp"

// class-internals.h brings in jit-icall-reg.h, which has no include guard.
#include "mono/metadata/class-internals.h"
#include "mono/metadata/metadata.h"

#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Instructions.h>
#include <llvm/Support/Alignment.h>
#include <llvm/Support/ErrorHandling.h>

#include <optional>
#include <string_view>
#include <vector>

namespace mono {

namespace {

bool
is_interlocked_class (MonoClass *klass)
{
	if (m_class_get_image (klass) != mono_get_corlib ())
		return false;
	if (std::string_view (m_class_get_name_space (klass)) != "System.Threading")
		return false;

	return std::string_view (m_class_get_name (klass)) == "Interlocked";
}

/// The width t names, ignoring whether t is itself a byref. Every Interlocked
/// overload keeps the same type code on the byref location and the plain
/// value parameters beside it, so a caller checks byref-ness itself.
std::optional<InterlockedCall::Width>
scalar_width_of (MonoType *t)
{
	switch (t->type) {
	case MONO_TYPE_I4:
	case MONO_TYPE_U4:
		return InterlockedCall::Width::I32;
	case MONO_TYPE_I8:
	case MONO_TYPE_U8:
		return InterlockedCall::Width::I64;
	case MONO_TYPE_I:
	case MONO_TYPE_U:
		return InterlockedCall::Width::Native;
	case MONO_TYPE_R4:
		return InterlockedCall::Width::Single;
	case MONO_TYPE_R8:
		return InterlockedCall::Width::Double;
	default:
		return std::nullopt;
	}
}

/// Whether t is a managed pointer to System.Object, the shape every parameter
/// of the object&-overloads takes.
bool
is_object_byref (MonoType *t)
{
	return t->byref && t->type == MONO_TYPE_OBJECT;
}

/// ILOCK_1 through ILOCK_23 in mono/metadata/icall-def.h. emit_builtin_call
/// asks before the wrapper swap resolves callee to the icall table's C
/// function, so this matches by name and signature shape instead.
std::optional<InterlockedCall>
interlocked_shape (std::string_view name, MonoMethodSignature *sig)
{
	if (name == "MemoryBarrier" && sig->param_count == 0)
		return InterlockedCall { InterlockedCall::Op::MemoryBarrier,
		                        InterlockedCall::Width::I32 };

	if ((name == "Increment" || name == "Decrement") && sig->param_count == 1) {
		MonoType *location = sig->params[0];

		if (!location->byref)
			return std::nullopt;

		std::optional<InterlockedCall::Width> width = scalar_width_of (location);

		if (width != InterlockedCall::Width::I32 && width != InterlockedCall::Width::I64)
			return std::nullopt;

		return InterlockedCall { name == "Increment" ? InterlockedCall::Op::Increment
		                                             : InterlockedCall::Op::Decrement,
		                        *width };
	}

	if (name == "Read" && sig->param_count == 1) {
		MonoType *location = sig->params[0];

		if (!location->byref || scalar_width_of (location) != InterlockedCall::Width::I64)
			return std::nullopt;

		return InterlockedCall { InterlockedCall::Op::Read, InterlockedCall::Width::I64 };
	}

	if (name == "Add" && sig->param_count == 2) {
		MonoType *location = sig->params[0];
		MonoType *value = sig->params[1];

		if (!location->byref || value->byref)
			return std::nullopt;

		std::optional<InterlockedCall::Width> width = scalar_width_of (location);

		if (width != InterlockedCall::Width::I32 && width != InterlockedCall::Width::I64)
			return std::nullopt;
		if (scalar_width_of (value) != width)
			return std::nullopt;

		return InterlockedCall { InterlockedCall::Op::Add, *width };
	}

	if (name == "Exchange" && sig->param_count == 2) {
		MonoType *location = sig->params[0];
		MonoType *value = sig->params[1];

		if (!location->byref || value->byref)
			return std::nullopt;

		std::optional<InterlockedCall::Width> width = scalar_width_of (location);

		if (!width || scalar_width_of (value) != width)
			return std::nullopt;

		return InterlockedCall { InterlockedCall::Op::Exchange, *width };
	}

	if (name == "Exchange" && sig->param_count == 3) {
		if (!is_object_byref (sig->params[0]) || !is_object_byref (sig->params[1])
		    || !is_object_byref (sig->params[2]))
			return std::nullopt;

		return InterlockedCall { InterlockedCall::Op::ExchangeObject,
		                        InterlockedCall::Width::I32 };
	}

	if (name == "CompareExchange" && sig->param_count == 3) {
		MonoType *location = sig->params[0];
		MonoType *value = sig->params[1];
		MonoType *comparand = sig->params[2];

		if (!location->byref || value->byref || comparand->byref)
			return std::nullopt;

		std::optional<InterlockedCall::Width> width = scalar_width_of (location);

		if (!width || scalar_width_of (value) != width
		    || scalar_width_of (comparand) != width)
			return std::nullopt;

		return InterlockedCall { InterlockedCall::Op::CompareExchange, *width };
	}

	if (name == "CompareExchange" && sig->param_count == 4) {
		if (is_object_byref (sig->params[0])) {
			if (!is_object_byref (sig->params[1]) || !is_object_byref (sig->params[2])
			    || !is_object_byref (sig->params[3]))
				return std::nullopt;

			return InterlockedCall { InterlockedCall::Op::CompareExchangeObject,
			                        InterlockedCall::Width::I32 };
		}

		MonoType *location = sig->params[0];
		MonoType *value = sig->params[1];
		MonoType *comparand = sig->params[2];
		MonoType *success = sig->params[3];

		if (!location->byref || value->byref || comparand->byref || !success->byref)
			return std::nullopt;
		if (location->type != MONO_TYPE_I4 || value->type != MONO_TYPE_I4
		    || comparand->type != MONO_TYPE_I4 || success->type != MONO_TYPE_BOOLEAN)
			return std::nullopt;

		return InterlockedCall { InterlockedCall::Op::CompareExchangeSuccess,
		                        InterlockedCall::Width::I32 };
	}

	return std::nullopt;
}

llvm::Type *
scalar_llvm_type (llvm::LLVMContext &ctx, InterlockedCall::Width width)
{
	switch (width) {
	case InterlockedCall::Width::I32:
		return llvm::Type::getInt32Ty (ctx);
	case InterlockedCall::Width::I64:
	case InterlockedCall::Width::Native:
		return llvm::Type::getInt64Ty (ctx);
	case InterlockedCall::Width::Single:
		return llvm::Type::getFloatTy (ctx);
	case InterlockedCall::Width::Double:
		return llvm::Type::getDoubleTy (ctx);
	}

	llvm_unreachable ("unhandled InterlockedCall::Width");
}

llvm::Align
scalar_align (InterlockedCall::Width width)
{
	switch (width) {
	case InterlockedCall::Width::I32:
	case InterlockedCall::Width::Single:
		return llvm::Align (4);
	case InterlockedCall::Width::I64:
	case InterlockedCall::Width::Native:
	case InterlockedCall::Width::Double:
		return llvm::Align (8);
	}

	llvm_unreachable ("unhandled InterlockedCall::Width");
}

} // namespace

std::optional<InterlockedCall>
interlocked_op_for (MonoMethod *method, MonoMethodSignature *sig)
{
	if (sig == nullptr || sig->hasthis || sig->call_convention == MONO_CALL_VARARG)
		return std::nullopt;
	if (!is_interlocked_class (method->klass))
		return std::nullopt;

	return interlocked_shape (std::string_view (method->name), sig);
}

/**
 * Writes the atomic IR for call.op, in the shape interlocked_op_for () chose.
 *
 * Mono's Interlocked is sequentially consistent (mono_atomic_* and the
 * classic compiler's own MONO_ATOMIC_ADD/CAS/XCHG opcodes all take the full
 * fence). Every atomic below therefore orders SequentiallyConsistent, never
 * Monotonic.
 *
 * atomicrmw returns the value the location held before the op. That is
 * Interlocked.Exchange's own answer, but not Increment, Decrement or Add's:
 * those three return the location's new value, so each corrects the
 * atomicrmw answer by the delta it asked the location to apply.
 *
 * cmpxchg's extracted value is the location's old value regardless of
 * whether the swap took place, which is what Interlocked.CompareExchange
 * answers too.
 */
llvm::Error
MethodLLVMEmitter::emit_interlocked (MonoIrBuilder &builder, MonoMethodSignature *sig,
                                     const InterlockedCall &call)
{
	constexpr llvm::AtomicOrdering seq_cst = llvm::AtomicOrdering::SequentiallyConsistent;

	if (call.op == InterlockedCall::Op::MemoryBarrier) {
		builder.CreateFence (seq_cst);
		return llvm::Error::success ();
	}

	llvm::Expected<std::vector<llvm::Value *>> args = pop_call_arguments (builder, sig);

	if (!args)
		return args.takeError ();

	llvm::Value *result = nullptr;

	switch (call.op) {
	case InterlockedCall::Op::Increment:
	case InterlockedCall::Op::Decrement: {
		llvm::Type *type = scalar_llvm_type (context (), call.width);
		llvm::Align align = scalar_align (call.width);
		llvm::Constant *one = llvm::ConstantInt::get (type, 1);
		llvm::AtomicRMWInst::BinOp op = call.op == InterlockedCall::Op::Increment
		                                         ? llvm::AtomicRMWInst::Add
		                                         : llvm::AtomicRMWInst::Sub;
		llvm::Value *old = builder.CreateAtomicRMW (op, (*args)[0], one, align, seq_cst);

		result = call.op == InterlockedCall::Op::Increment
		                 ? builder.CreateAdd (old, one)
		                 : builder.CreateSub (old, one);
		break;
	}
	case InterlockedCall::Op::Add: {
		llvm::Align align = scalar_align (call.width);
		llvm::Value *old = builder.CreateAtomicRMW (llvm::AtomicRMWInst::Add, (*args)[0],
		                                            (*args)[1], align, seq_cst);

		result = builder.CreateAdd (old, (*args)[1]);
		break;
	}
	case InterlockedCall::Op::Exchange: {
		llvm::Align align = scalar_align (call.width);

		// xchg accepts an integer, a pointer or a floating-point operand
		// directly, unlike cmpxchg below, so the exchanged value needs no
		// bitcast either way.
		result = builder.CreateAtomicRMW (llvm::AtomicRMWInst::Xchg, (*args)[0],
		                                  (*args)[1], align, seq_cst);
		break;
	}
	case InterlockedCall::Op::CompareExchange: {
		llvm::Align align = scalar_align (call.width);
		bool is_float = call.width == InterlockedCall::Width::Single
		                || call.width == InterlockedCall::Width::Double;
		llvm::Value *value = (*args)[1];
		llvm::Value *comparand = (*args)[2];

		// cmpxchg takes an integer or a pointer operand only, so a float or
		// double compare-and-swap rides as same-width bits instead.
		if (is_float) {
			llvm::Type *bits = call.width == InterlockedCall::Width::Single
			                            ? builder.getInt32Ty ()
			                            : builder.getInt64Ty ();

			value = builder.CreateBitCast (value, bits);
			comparand = builder.CreateBitCast (comparand, bits);
		}

		llvm::Value *cmpxchg = builder.CreateAtomicCmpXchg (
			(*args)[0], comparand, value, align, seq_cst, seq_cst);

		result = builder.CreateExtractValue (cmpxchg, 0);

		if (is_float)
			result = builder.CreateBitCast (result,
			                                scalar_llvm_type (context (), call.width));
		break;
	}
	case InterlockedCall::Op::CompareExchangeSuccess: {
		llvm::Align align (4);
		llvm::Value *cmpxchg = builder.CreateAtomicCmpXchg (
			(*args)[0], (*args)[2], (*args)[1], align, seq_cst, seq_cst);

		result = builder.CreateExtractValue (cmpxchg, 0);

		llvm::Value *success = builder.CreateExtractValue (cmpxchg, 1);

		builder.CreateAlignedStore (builder.CreateZExt (success, builder.getInt8Ty ()),
		                           (*args)[3], llvm::Align (1));
		break;
	}
	case InterlockedCall::Op::ExchangeObject: {
		llvm::Align align (TARGET_SIZEOF_VOID_P);
		llvm::Type *ptr = llvm::PointerType::get (context (), 0);
		llvm::Value *location = (*args)[0];
		llvm::Value *value = builder.CreateAlignedLoad (ptr, (*args)[1], align);
		llvm::Value *old = builder.CreateAtomicRMW (llvm::AtomicRMWInst::Xchg, location,
		                                            value, align, seq_cst);

		builder.CreateAlignedStore (old, (*args)[2], align);

		const GcBarrierLayout &gc = write_barrier_layout ();

		record_barrier_symbols (gc);
		builder.CreateCall (gc_barrier_decl (*module, gc), { location, value });
		break;
	}
	case InterlockedCall::Op::CompareExchangeObject: {
		llvm::Align align (TARGET_SIZEOF_VOID_P);
		llvm::Type *ptr = llvm::PointerType::get (context (), 0);
		llvm::Value *location = (*args)[0];
		llvm::Value *value = builder.CreateAlignedLoad (ptr, (*args)[1], align);
		llvm::Value *comparand = builder.CreateAlignedLoad (ptr, (*args)[2], align);
		llvm::Value *cmpxchg = builder.CreateAtomicCmpXchg (location, comparand, value,
		                                                    align, seq_cst, seq_cst);
		llvm::Value *old = builder.CreateExtractValue (cmpxchg, 0);

		builder.CreateAlignedStore (old, (*args)[3], align);

		// Marks the card whether or not the swap took place, the same as
		// ves_icall_..._CompareExchange_Object () does through
		// mono_gc_wbarrier_generic_nostore_internal (). A losing comparand
		// costs an extra rescan, never a missed one.
		const GcBarrierLayout &gc = write_barrier_layout ();

		record_barrier_symbols (gc);
		builder.CreateCall (gc_barrier_decl (*module, gc), { location, value });
		break;
	}
	case InterlockedCall::Op::Read: {
		llvm::LoadInst *load =
			builder.CreateAlignedLoad (builder.getInt64Ty (), (*args)[0], llvm::Align (8));

		load->setAtomic (seq_cst);
		result = load;
		break;
	}
	case InterlockedCall::Op::MemoryBarrier:
		llvm_unreachable ("handled above");
	}

	pop_stack (sig->param_count);

	if (sig->ret->type == MONO_TYPE_VOID)
		return llvm::Error::success ();

	return push_produced (builder, result, sig->ret);
}

} // namespace mono

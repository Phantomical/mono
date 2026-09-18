/**
 * \file
 * \brief LLVM lowering for System.Runtime.Intrinsics.X86.Sse.
 */

#include "intrinsics.hpp"

#include "../runtime/options.hpp"
#include "method-to-llvm.hpp"
#include "simd-emit.hpp"

#include "mono/utils/mono-hwcap.h"

#include <llvm/IR/Constant.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Intrinsics.h>
#include <llvm/IR/IntrinsicsX86.h>
#include <llvm/IR/Type.h>
#include <llvm/IR/Value.h>

namespace mono {

namespace {

bool sse_lowering ()
{
	return mono_hwcap_x86_has_sse1;
}

struct SseEmitters : SimdEmit {
	static llvm::FixedVectorType *v4f32 (MethodLLVMEmitter &emitter)
	{
		return llvm::FixedVectorType::get (llvm::Type::getFloatTy (context (emitter)), 4);
	}

	static BuiltinResult is_supported (MethodLLVMEmitter &emitter,
	                                   llvm::IRBuilder<> &builder, MonoMethod *)
	{
		builder.CreateRet (builder.getInt8 (mono_hwcap_x86_has_sse1 ? 1 : 0));
		return llvm::Error::success ();
	}

	// Do not apply the relaxed floating-point flags used by SimdEmit operations.
	static llvm::Value *add (llvm::IRBuilder<> &builder, llvm::Value *lhs, llvm::Value *rhs)
	{
		return builder.CreateFAdd (lhs, rhs);
	}

	static llvm::Value *sub (llvm::IRBuilder<> &builder, llvm::Value *lhs, llvm::Value *rhs)
	{
		return builder.CreateFSub (lhs, rhs);
	}

	static llvm::Value *mul (llvm::IRBuilder<> &builder, llvm::Value *lhs, llvm::Value *rhs)
	{
		return builder.CreateFMul (lhs, rhs);
	}

	static llvm::Value *div (llvm::IRBuilder<> &builder, llvm::Value *lhs, llvm::Value *rhs)
	{
		return builder.CreateFDiv (lhs, rhs);
	}

	template <BinaryOp op>
	static BuiltinResult binary (MethodLLVMEmitter &emitter, llvm::IRBuilder<> &builder,
	                             MonoMethod *)
	{
		builder.CreateRet (op (builder, argument (emitter, 0), argument (emitter, 1)));
		return llvm::Error::success ();
	}

	/// Applies op to lane zero and preserves the remaining lanes from left.
	template <BinaryOp op>
	static BuiltinResult binary_scalar (MethodLLVMEmitter &emitter,
	                                    llvm::IRBuilder<> &builder, MonoMethod *)
	{
		llvm::Value *left = argument (emitter, 0);
		llvm::Value *right = argument (emitter, 1);
		llvm::Value *lane = op (builder, builder.CreateExtractElement (left, (uint64_t) 0),
		                        builder.CreateExtractElement (right, (uint64_t) 0));

		builder.CreateRet (builder.CreateInsertElement (left, lane, (uint64_t) 0));
		return llvm::Error::success ();
	}

	template <llvm::Intrinsic::ID id>
	static BuiltinResult binary_intrinsic (MethodLLVMEmitter &emitter,
	                                       llvm::IRBuilder<> &builder, MonoMethod *)
	{
		builder.CreateRet (builder.CreateIntrinsic (
			id, {}, { argument (emitter, 0), argument (emitter, 1) }));
		return llvm::Error::success ();
	}

	static BuiltinResult sqrt (MethodLLVMEmitter &emitter, llvm::IRBuilder<> &builder,
	                          MonoMethod *)
	{
		llvm::Value *value = argument (emitter, 0);

		builder.CreateRet (
			builder.CreateIntrinsic (llvm::Intrinsic::sqrt, { value->getType () }, { value }));
		return llvm::Error::success ();
	}

	/// Fixed-type x86 intrinsics do not need an overloaded type.
	template <llvm::Intrinsic::ID id>
	static BuiltinResult unary_intrinsic (MethodLLVMEmitter &emitter,
	                                      llvm::IRBuilder<> &builder, MonoMethod *)
	{
		builder.CreateRet (builder.CreateIntrinsic (id, {}, { argument (emitter, 0) }));
		return llvm::Error::success ();
	}

	template <int predicate>
	static BuiltinResult compare (MethodLLVMEmitter &emitter, llvm::IRBuilder<> &builder,
	                              MonoMethod *)
	{
		builder.CreateRet (builder.CreateIntrinsic (
			llvm::Intrinsic::x86_sse_cmp_ps, {},
			{ argument (emitter, 0), argument (emitter, 1), builder.getInt8 (predicate) }));
		return llvm::Error::success ();
	}

	template <llvm::BinaryOperator::BinaryOps op>
	static BuiltinResult bitwise (MethodLLVMEmitter &emitter, llvm::IRBuilder<> &builder,
	                              MonoMethod *)
	{
		llvm::Type *i32x4 = llvm::FixedVectorType::get (
			llvm::Type::getInt32Ty (context (emitter)), 4);
		llvm::Value *left = builder.CreateBitCast (argument (emitter, 0), i32x4);
		llvm::Value *right = builder.CreateBitCast (argument (emitter, 1), i32x4);

		builder.CreateRet (
			builder.CreateBitCast (builder.CreateBinOp (op, left, right), v4f32 (emitter)));
		return llvm::Error::success ();
	}

	/// Computes (~left) & right.
	static BuiltinResult and_not (MethodLLVMEmitter &emitter, llvm::IRBuilder<> &builder,
	                              MonoMethod *)
	{
		llvm::Type *i32x4 = llvm::FixedVectorType::get (
			llvm::Type::getInt32Ty (context (emitter)), 4);
		llvm::Value *left = builder.CreateBitCast (argument (emitter, 0), i32x4);
		llvm::Value *right = builder.CreateBitCast (argument (emitter, 1), i32x4);

		builder.CreateRet (builder.CreateBitCast (
			builder.CreateAnd (builder.CreateNot (left), right), v4f32 (emitter)));
		return llvm::Error::success ();
	}

	static BuiltinResult set_zero (MethodLLVMEmitter &emitter, llvm::IRBuilder<> &builder,
	                               MonoMethod *)
	{
		builder.CreateRet (llvm::Constant::getNullValue (v4f32 (emitter)));
		return llvm::Error::success ();
	}

	static BuiltinResult load (MethodLLVMEmitter &emitter, llvm::IRBuilder<> &builder,
	                           llvm::Align align)
	{
		builder.CreateRet (
			builder.CreateAlignedLoad (v4f32 (emitter), argument (emitter, 0), align));
		return llvm::Error::success ();
	}

	static BuiltinResult load_unaligned (MethodLLVMEmitter &emitter, llvm::IRBuilder<> &builder,
	                                     MonoMethod *)
	{
		return load (emitter, builder, llvm::Align (4));
	}

	static BuiltinResult load_aligned (MethodLLVMEmitter &emitter, llvm::IRBuilder<> &builder,
	                                   MonoMethod *)
	{
		return load (emitter, builder, llvm::Align (16));
	}

	static BuiltinResult store (MethodLLVMEmitter &emitter, llvm::IRBuilder<> &builder,
	                            llvm::Align align)
	{
		builder.CreateAlignedStore (argument (emitter, 1), argument (emitter, 0), align);
		builder.CreateRetVoid ();
		return llvm::Error::success ();
	}

	static BuiltinResult store_unaligned (MethodLLVMEmitter &emitter, llvm::IRBuilder<> &builder,
	                                      MonoMethod *)
	{
		return store (emitter, builder, llvm::Align (4));
	}

	static BuiltinResult store_aligned (MethodLLVMEmitter &emitter, llvm::IRBuilder<> &builder,
	                                    MonoMethod *)
	{
		return store (emitter, builder, llvm::Align (16));
	}

	/// Leaves methods with managed implementations alone and rejects unmatched intrinsics.
	static BuiltinResult unimplemented (MethodLLVMEmitter &emitter, llvm::IRBuilder<> &,
	                                    MonoMethod *method)
	{
		std::string_view name = method->name;

		if (name == "SetAllVector128" || name == "SetVector128")
			return std::nullopt;

		return unsupported_il (
			emitter, llvm::Twine ("Sse.") + name + ", which this backend does not lower yet");
	}
};

const ClassKey sse = { nullptr, "System.Runtime.Intrinsics.X86", "Sse" };

using Ops = llvm::BinaryOperator;

const BuiltinBody sse_table[] = {
	{ sse, "get_IsSupported", "", false, nullptr, SseEmitters::is_supported },

	{ sse, "Add", "VV", false, sse_lowering, SseEmitters::binary<SseEmitters::add> },
	{ sse, "Subtract", "VV", false, sse_lowering, SseEmitters::binary<SseEmitters::sub> },
	{ sse, "Multiply", "VV", false, sse_lowering, SseEmitters::binary<SseEmitters::mul> },
	{ sse, "Divide", "VV", false, sse_lowering, SseEmitters::binary<SseEmitters::div> },

	{ sse, "AddScalar", "VV", false, sse_lowering,
	  SseEmitters::binary_scalar<SseEmitters::add> },
	{ sse, "SubtractScalar", "VV", false, sse_lowering,
	  SseEmitters::binary_scalar<SseEmitters::sub> },
	{ sse, "MultiplyScalar", "VV", false, sse_lowering,
	  SseEmitters::binary_scalar<SseEmitters::mul> },
	{ sse, "DivideScalar", "VV", false, sse_lowering,
	  SseEmitters::binary_scalar<SseEmitters::div> },

	{ sse, "And", "VV", false, sse_lowering, SseEmitters::bitwise<Ops::And> },
	{ sse, "Or", "VV", false, sse_lowering, SseEmitters::bitwise<Ops::Or> },
	{ sse, "Xor", "VV", false, sse_lowering, SseEmitters::bitwise<Ops::Xor> },
	{ sse, "AndNot", "VV", false, sse_lowering, SseEmitters::and_not },

	{ sse, "Max", "VV", false, sse_lowering,
	  SseEmitters::binary_intrinsic<llvm::Intrinsic::x86_sse_max_ps> },
	{ sse, "Min", "VV", false, sse_lowering,
	  SseEmitters::binary_intrinsic<llvm::Intrinsic::x86_sse_min_ps> },
	{ sse, "MaxScalar", "VV", false, sse_lowering,
	  SseEmitters::binary_intrinsic<llvm::Intrinsic::x86_sse_max_ss> },
	{ sse, "MinScalar", "VV", false, sse_lowering,
	  SseEmitters::binary_intrinsic<llvm::Intrinsic::x86_sse_min_ss> },

	{ sse, "Sqrt", "V", false, sse_lowering, SseEmitters::sqrt },
	{ sse, "Reciprocal", "V", false, sse_lowering,
	  SseEmitters::unary_intrinsic<llvm::Intrinsic::x86_sse_rcp_ps> },

	// CMPPS immediate predicates.
	{ sse, "CompareEqual", "VV", false, sse_lowering, SseEmitters::compare<0> },
	{ sse, "CompareLessThan", "VV", false, sse_lowering, SseEmitters::compare<1> },
	{ sse, "CompareLessThanOrEqual", "VV", false, sse_lowering, SseEmitters::compare<2> },
	{ sse, "CompareUnordered", "VV", false, sse_lowering, SseEmitters::compare<3> },
	{ sse, "CompareNotEqual", "VV", false, sse_lowering, SseEmitters::compare<4> },
	{ sse, "CompareNotLessThan", "VV", false, sse_lowering, SseEmitters::compare<5> },
	{ sse, "CompareGreaterThanOrEqual", "VV", false, sse_lowering, SseEmitters::compare<5> },
	{ sse, "CompareNotLessThanOrEqual", "VV", false, sse_lowering, SseEmitters::compare<6> },
	{ sse, "CompareGreaterThan", "VV", false, sse_lowering, SseEmitters::compare<6> },
	{ sse, "CompareOrdered", "VV", false, sse_lowering, SseEmitters::compare<7> },
	{ sse, "CompareNotGreaterThan", "VV", false, sse_lowering, SseEmitters::compare<2> },
	{ sse, "CompareNotGreaterThanOrEqual", "VV", false, sse_lowering, SseEmitters::compare<1> },

	{ sse, "LoadVector128", "S", false, sse_lowering, SseEmitters::load_unaligned },
	{ sse, "LoadAlignedVector128", "S", false, sse_lowering, SseEmitters::load_aligned },
	{ sse, "Store", "SV", false, sse_lowering, SseEmitters::store_unaligned },
	{ sse, "StoreAligned", "SV", false, sse_lowering, SseEmitters::store_aligned },

	{ sse, "SetZeroVector128", "", false, sse_lowering, SseEmitters::set_zero },

	{ sse, {}, any_signature, false, nullptr, SseEmitters::unimplemented },
};

} // namespace

llvm::ArrayRef<BuiltinBody>
simd_x86_bodies ()
{
	return sse_table;
}

} // namespace mono

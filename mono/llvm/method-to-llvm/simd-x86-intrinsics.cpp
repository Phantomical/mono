/**
 * \file
 * \brief LLVM lowering for System.Runtime.Intrinsics.X86.*.
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
#include <llvm/IR/InstrTypes.h>
#include <llvm/IR/Intrinsics.h>
#include <llvm/IR/IntrinsicsX86.h>
#include <llvm/IR/Type.h>
#include <llvm/IR/Value.h>

#include <vector>

namespace mono {

namespace {

bool sse_lowering ()
{
	return mono_hwcap_x86_has_sse1;
}

bool sse2_lowering ()
{
	return mono_hwcap_x86_has_sse2;
}

bool sse3_lowering ()
{
	return mono_hwcap_x86_has_sse3;
}

bool ssse3_lowering ()
{
	return mono_hwcap_x86_has_ssse3;
}

struct SseEmitters : SimdEmit {
	static bool is_float (llvm::Value *value)
	{
		return value->getType ()->isFPOrFPVectorTy ();
	}

	static BuiltinResult is_supported (bool supported, llvm::IRBuilder<> &builder)
	{
		builder.CreateRet (builder.getInt8 (supported ? 1 : 0));
		return llvm::Error::success ();
	}

	static BuiltinResult sse_is_supported (MethodLLVMEmitter &, llvm::IRBuilder<> &builder,
	                                       MonoMethod *)
	{
		return is_supported (mono_hwcap_x86_has_sse1, builder);
	}

	static BuiltinResult sse2_is_supported (MethodLLVMEmitter &, llvm::IRBuilder<> &builder,
	                                        MonoMethod *)
	{
		return is_supported (mono_hwcap_x86_has_sse2, builder);
	}

	static BuiltinResult sse3_is_supported (MethodLLVMEmitter &, llvm::IRBuilder<> &builder,
	                                        MonoMethod *)
	{
		return is_supported (mono_hwcap_x86_has_sse3, builder);
	}

	static BuiltinResult ssse3_is_supported (MethodLLVMEmitter &, llvm::IRBuilder<> &builder,
	                                         MonoMethod *)
	{
		return is_supported (mono_hwcap_x86_has_ssse3, builder);
	}

	// Preserve exact floating-point semantics; integer overloads use matching integer operations.
	static llvm::Value *add (llvm::IRBuilder<> &builder, llvm::Value *lhs, llvm::Value *rhs)
	{
		return is_float (lhs) ? builder.CreateFAdd (lhs, rhs) : builder.CreateAdd (lhs, rhs);
	}

	static llvm::Value *sub (llvm::IRBuilder<> &builder, llvm::Value *lhs, llvm::Value *rhs)
	{
		return is_float (lhs) ? builder.CreateFSub (lhs, rhs) : builder.CreateSub (lhs, rhs);
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

	/// Passes predicate as the CMPPS immediate operand.
	template <int predicate>
	static BuiltinResult compare (MethodLLVMEmitter &emitter, llvm::IRBuilder<> &builder,
	                              MonoMethod *)
	{
		builder.CreateRet (builder.CreateIntrinsic (
			llvm::Intrinsic::x86_sse_cmp_ps, {},
			{ argument (emitter, 0), argument (emitter, 1), builder.getInt8 (predicate) }));
		return llvm::Error::success ();
	}

	/// Uses CMPPD for double and sign-extends integer comparisons to all-one or zero lanes.
	template <int predicate, llvm::CmpInst::Predicate int_predicate>
	static BuiltinResult compare2 (MethodLLVMEmitter &emitter, llvm::IRBuilder<> &builder,
	                               MonoMethod *)
	{
		llvm::Value *lhs = argument (emitter, 0);
		llvm::Value *rhs = argument (emitter, 1);

		if (is_float (lhs))
			builder.CreateRet (builder.CreateIntrinsic (
				llvm::Intrinsic::x86_sse2_cmp_pd, {}, { lhs, rhs, builder.getInt8 (predicate) }));
		else
			builder.CreateRet (
				builder.CreateSExt (builder.CreateICmp (int_predicate, lhs, rhs), lhs->getType ()));
		return llvm::Error::success ();
	}

	template <llvm::BinaryOperator::BinaryOps op>
	static BuiltinResult bitwise (MethodLLVMEmitter &emitter, llvm::IRBuilder<> &builder,
	                              MonoMethod *)
	{
		llvm::Value *left = argument (emitter, 0);
		llvm::Value *right = argument (emitter, 1);
		llvm::Type *original = left->getType ();
		llvm::Type *i32x4 = llvm::FixedVectorType::get (
			llvm::Type::getInt32Ty (context (emitter)), 4);

		builder.CreateRet (builder.CreateBitCast (
			builder.CreateBinOp (op, builder.CreateBitCast (left, i32x4),
			                     builder.CreateBitCast (right, i32x4)),
			original));
		return llvm::Error::success ();
	}

	/// Computes (~left) & right.
	static BuiltinResult and_not (MethodLLVMEmitter &emitter, llvm::IRBuilder<> &builder,
	                              MonoMethod *)
	{
		llvm::Value *left = argument (emitter, 0);
		llvm::Value *right = argument (emitter, 1);
		llvm::Type *original = left->getType ();
		llvm::Type *i32x4 = llvm::FixedVectorType::get (
			llvm::Type::getInt32Ty (context (emitter)), 4);

		builder.CreateRet (builder.CreateBitCast (
			builder.CreateAnd (builder.CreateNot (builder.CreateBitCast (left, i32x4)),
			                   builder.CreateBitCast (right, i32x4)),
			original));
		return llvm::Error::success ();
	}

	/// Rejects Sse2.Multiply's unsupported PMULUDQ overload.
	static BuiltinResult multiply_double (MethodLLVMEmitter &emitter, llvm::IRBuilder<> &builder,
	                                      MonoMethod *method)
	{
		llvm::Value *lhs = argument (emitter, 0);

		if (!is_float (lhs))
			return unsupported_il (
				emitter, llvm::Twine (method->name) + ", which this backend does not lower yet");

		builder.CreateRet (builder.CreateFMul (lhs, argument (emitter, 1)));
		return llvm::Error::success ();
	}

	/// Selects the double, unsigned-byte, or signed-short operation from the element type.
	template <llvm::Intrinsic::ID float_id, llvm::Intrinsic::ID narrow_id,
	         llvm::Intrinsic::ID wide_id>
	static BuiltinResult minmax (MethodLLVMEmitter &emitter, llvm::IRBuilder<> &builder,
	                             MonoMethod *)
	{
		llvm::Value *lhs = argument (emitter, 0);
		llvm::Value *rhs = argument (emitter, 1);
		llvm::Type *elem =
			llvm::cast<llvm::FixedVectorType> (lhs->getType ())->getElementType ();

		if (elem->isFloatingPointTy ())
			builder.CreateRet (builder.CreateIntrinsic (float_id, {}, { lhs, rhs }));
		else if (elem->getIntegerBitWidth () == 8)
			builder.CreateRet (
				builder.CreateIntrinsic (narrow_id, { lhs->getType () }, { lhs, rhs }));
		else
			builder.CreateRet (
				builder.CreateIntrinsic (wide_id, { lhs->getType () }, { lhs, rhs }));
		return llvm::Error::success ();
	}

	/// Select the packed single- or double-precision intrinsic from the element type.
	template <llvm::Intrinsic::ID ps_id, llvm::Intrinsic::ID pd_id>
	static BuiltinResult horizontal (MethodLLVMEmitter &emitter, llvm::IRBuilder<> &builder,
	                                 MonoMethod *)
	{
		llvm::Value *lhs = argument (emitter, 0);
		llvm::Value *rhs = argument (emitter, 1);
		llvm::Type *elem =
			llvm::cast<llvm::FixedVectorType> (lhs->getType ())->getElementType ();

		builder.CreateRet (
			builder.CreateIntrinsic (elem->isDoubleTy () ? pd_id : ps_id, {}, { lhs, rhs }));
		return llvm::Error::success ();
	}

	/// Select the 16- or 32-bit intrinsic from the element width.
	template <llvm::Intrinsic::ID w_id, llvm::Intrinsic::ID d_id>
	static BuiltinResult horizontal_int (MethodLLVMEmitter &emitter, llvm::IRBuilder<> &builder,
	                                     MonoMethod *)
	{
		llvm::Value *lhs = argument (emitter, 0);
		llvm::Value *rhs = argument (emitter, 1);
		llvm::Type *elem =
			llvm::cast<llvm::FixedVectorType> (lhs->getType ())->getElementType ();

		builder.CreateRet (builder.CreateIntrinsic (
			elem->getIntegerBitWidth () == 16 ? w_id : d_id, {}, { lhs, rhs }));
		return llvm::Error::success ();
	}

	/// Select the 8-, 16-, or 32-bit intrinsic from the element width.
	template <llvm::Intrinsic::ID b_id, llvm::Intrinsic::ID w_id, llvm::Intrinsic::ID d_id>
	static BuiltinResult sign (MethodLLVMEmitter &emitter, llvm::IRBuilder<> &builder,
	                           MonoMethod *)
	{
		llvm::Value *lhs = argument (emitter, 0);
		llvm::Value *rhs = argument (emitter, 1);
		llvm::Type *elem =
			llvm::cast<llvm::FixedVectorType> (lhs->getType ())->getElementType ();
		llvm::Intrinsic::ID id = elem->getIntegerBitWidth () == 8
			? b_id
			: (elem->getIntegerBitWidth () == 16 ? w_id : d_id);

		builder.CreateRet (builder.CreateIntrinsic (id, {}, { lhs, rhs }));
		return llvm::Error::success ();
	}

	/// LLVM has no 128-bit x86 PABS intrinsic, so use the generic vector operation.
	static BuiltinResult abs (MethodLLVMEmitter &emitter, llvm::IRBuilder<> &builder, MonoMethod *)
	{
		llvm::Value *value = argument (emitter, 0);
		llvm::Value *result = builder.CreateIntrinsic (
			llvm::Intrinsic::abs, { value->getType () }, { value, builder.getFalse () });

		builder.CreateRet (builder.CreateBitCast (result, return_type (emitter)));
		return llvm::Error::success ();
	}

	/// Implement PALIGNR with integer shifts so the byte count may be a runtime value.
	/// Counts of 32 or more produce zero, matching the instruction.
	static BuiltinResult align_right (MethodLLVMEmitter &emitter, llvm::IRBuilder<> &builder,
	                                  MonoMethod *)
	{
		llvm::Value *left = argument (emitter, 0);
		llvm::Value *right = argument (emitter, 1);
		llvm::Value *mask = argument (emitter, 2);
		llvm::LLVMContext &ctx = context (emitter);
		llvm::Type *i128 = llvm::Type::getIntNTy (ctx, 128);
		llvm::Type *i256 = llvm::Type::getIntNTy (ctx, 256);

		llvm::Value *hi = builder.CreateShl (
			builder.CreateZExt (builder.CreateBitCast (left, i128), i256),
			llvm::ConstantInt::get (i256, 128));
		llvm::Value *lo = builder.CreateZExt (builder.CreateBitCast (right, i128), i256);
		llvm::Value *concat = builder.CreateOr (hi, lo);

		llvm::Value *shift =
			builder.CreateShl (builder.CreateZExt (mask, i256), llvm::ConstantInt::get (i256, 3));
		llvm::Value *shifted =
			builder.CreateBitCast (builder.CreateTrunc (builder.CreateLShr (concat, shift), i128),
			                       return_type (emitter));

		builder.CreateRet (builder.CreateSelect (
			builder.CreateICmpUGE (mask, builder.getInt8 (32)),
			llvm::Constant::getNullValue (return_type (emitter)), shifted));
		return llvm::Error::success ();
	}

	/// Lower LoadDquVector128 with LDDQU rather than a generic unaligned load.
	static BuiltinResult load_dqu (MethodLLVMEmitter &emitter, llvm::IRBuilder<> &builder,
	                               MonoMethod *)
	{
		llvm::Value *loaded = builder.CreateIntrinsic (
			llvm::Intrinsic::x86_sse3_ldu_dq, {}, { argument (emitter, 0) });

		builder.CreateRet (builder.CreateBitCast (loaded, return_type (emitter)));
		return llvm::Error::success ();
	}

	static BuiltinResult load_and_duplicate (MethodLLVMEmitter &emitter,
	                                         llvm::IRBuilder<> &builder, MonoMethod *)
	{
		llvm::Value *scalar = builder.CreateLoad (
			llvm::Type::getDoubleTy (context (emitter)), argument (emitter, 0));

		builder.CreateRet (builder.CreateVectorSplat (2, scalar));
		return llvm::Error::success ();
	}

	static BuiltinResult move_and_duplicate (MethodLLVMEmitter &emitter,
	                                         llvm::IRBuilder<> &builder, MonoMethod *)
	{
		builder.CreateRet (builder.CreateShuffleVector (argument (emitter, 0), { 0, 0 }));
		return llvm::Error::success ();
	}

	static BuiltinResult move_high_and_duplicate (MethodLLVMEmitter &emitter,
	                                              llvm::IRBuilder<> &builder, MonoMethod *)
	{
		builder.CreateRet (
			builder.CreateShuffleVector (argument (emitter, 0), { 1, 1, 3, 3 }));
		return llvm::Error::success ();
	}

	static BuiltinResult move_low_and_duplicate (MethodLLVMEmitter &emitter,
	                                             llvm::IRBuilder<> &builder, MonoMethod *)
	{
		builder.CreateRet (
			builder.CreateShuffleVector (argument (emitter, 0), { 0, 0, 2, 2 }));
		return llvm::Error::success ();
	}

	static BuiltinResult set_zero (MethodLLVMEmitter &emitter, llvm::IRBuilder<> &builder,
	                               MonoMethod *)
	{
		builder.CreateRet (llvm::Constant::getNullValue (return_type (emitter)));
		return llvm::Error::success ();
	}

	static BuiltinResult load (MethodLLVMEmitter &emitter, llvm::IRBuilder<> &builder,
	                           llvm::Align align)
	{
		builder.CreateRet (
			builder.CreateAlignedLoad (return_type (emitter), argument (emitter, 0), align));
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

		return unsupported_il (emitter, llvm::Twine (m_class_get_name (method->klass)) + "." + name
			+ ", which this backend does not lower yet");
	}
};

const ClassKey sse = { nullptr, "System.Runtime.Intrinsics.X86", "Sse" };
const ClassKey sse2 = { nullptr, "System.Runtime.Intrinsics.X86", "Sse2" };
const ClassKey sse3 = { nullptr, "System.Runtime.Intrinsics.X86", "Sse3" };
const ClassKey ssse3 = { nullptr, "System.Runtime.Intrinsics.X86", "Ssse3" };

using Ops = llvm::BinaryOperator;
namespace Intr = llvm::Intrinsic;

const BuiltinBody sse_table[] = {
	{ sse, "get_IsSupported", "", false, nullptr, SseEmitters::sse_is_supported },

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

const BuiltinBody sse2_table[] = {
	{ sse2, "get_IsSupported", "", false, nullptr, SseEmitters::sse2_is_supported },

	{ sse2, "Add", "VV", false, sse2_lowering, SseEmitters::binary<SseEmitters::add> },
	{ sse2, "Subtract", "VV", false, sse2_lowering, SseEmitters::binary<SseEmitters::sub> },
	{ sse2, "Multiply", "VV", false, sse2_lowering, SseEmitters::multiply_double },
	{ sse2, "Divide", "VV", false, sse2_lowering, SseEmitters::binary<SseEmitters::div> },

	{ sse2, "AddScalar", "VV", false, sse2_lowering,
	  SseEmitters::binary_scalar<SseEmitters::add> },
	{ sse2, "SubtractScalar", "VV", false, sse2_lowering,
	  SseEmitters::binary_scalar<SseEmitters::sub> },
	{ sse2, "DivideScalar", "VV", false, sse2_lowering,
	  SseEmitters::binary_scalar<SseEmitters::div> },

	{ sse2, "And", "VV", false, sse2_lowering, SseEmitters::bitwise<Ops::And> },
	{ sse2, "Or", "VV", false, sse2_lowering, SseEmitters::bitwise<Ops::Or> },
	{ sse2, "Xor", "VV", false, sse2_lowering, SseEmitters::bitwise<Ops::Xor> },
	{ sse2, "AndNot", "VV", false, sse2_lowering, SseEmitters::and_not },

	{ sse2, "Min", "VV", false, sse2_lowering,
	  SseEmitters::minmax<Intr::x86_sse2_min_pd, Intr::umin, Intr::smin> },
	{ sse2, "Max", "VV", false, sse2_lowering,
	  SseEmitters::minmax<Intr::x86_sse2_max_pd, Intr::umax, Intr::smax> },

	{ sse2, "Sqrt", "V", false, sse2_lowering, SseEmitters::sqrt },

	// Ordered integer comparisons have only signed overloads.
	{ sse2, "CompareEqual", "VV", false, sse2_lowering,
	  SseEmitters::compare2<0, llvm::CmpInst::ICMP_EQ> },
	{ sse2, "CompareGreaterThan", "VV", false, sse2_lowering,
	  SseEmitters::compare2<6, llvm::CmpInst::ICMP_SGT> },
	{ sse2, "CompareLessThan", "VV", false, sse2_lowering,
	  SseEmitters::compare2<1, llvm::CmpInst::ICMP_SLT> },

	{ sse2, "LoadVector128", "S", false, sse2_lowering, SseEmitters::load_unaligned },
	{ sse2, "LoadAlignedVector128", "S", false, sse2_lowering, SseEmitters::load_aligned },
	{ sse2, "Store", "SV", false, sse2_lowering, SseEmitters::store_unaligned },
	{ sse2, "StoreAligned", "SV", false, sse2_lowering, SseEmitters::store_aligned },

	{ sse2, "SetZeroVector128", "", false, sse2_lowering, SseEmitters::set_zero },

	{ sse2, {}, any_signature, false, nullptr, SseEmitters::unimplemented },
};

const BuiltinBody sse3_table[] = {
	{ sse3, "get_IsSupported", "", false, nullptr, SseEmitters::sse3_is_supported },

	{ sse3, "AddSubtract", "VV", false, sse3_lowering,
	  SseEmitters::horizontal<Intr::x86_sse3_addsub_ps, Intr::x86_sse3_addsub_pd> },
	{ sse3, "HorizontalAdd", "VV", false, sse3_lowering,
	  SseEmitters::horizontal<Intr::x86_sse3_hadd_ps, Intr::x86_sse3_hadd_pd> },
	{ sse3, "HorizontalSubtract", "VV", false, sse3_lowering,
	  SseEmitters::horizontal<Intr::x86_sse3_hsub_ps, Intr::x86_sse3_hsub_pd> },

	{ sse3, "LoadAndDuplicateToVector128", "S", false, sse3_lowering,
	  SseEmitters::load_and_duplicate },
	{ sse3, "LoadDquVector128", "S", false, sse3_lowering, SseEmitters::load_dqu },

	{ sse3, "MoveAndDuplicate", "V", false, sse3_lowering, SseEmitters::move_and_duplicate },
	{ sse3, "MoveHighAndDuplicate", "V", false, sse3_lowering,
	  SseEmitters::move_high_and_duplicate },
	{ sse3, "MoveLowAndDuplicate", "V", false, sse3_lowering,
	  SseEmitters::move_low_and_duplicate },

	{ sse3, {}, any_signature, false, nullptr, SseEmitters::unimplemented },
};

const BuiltinBody ssse3_table[] = {
	{ ssse3, "get_IsSupported", "", false, nullptr, SseEmitters::ssse3_is_supported },

	{ ssse3, "Abs", "V", false, ssse3_lowering, SseEmitters::abs },
	{ ssse3, "AlignRight", "VVS", false, ssse3_lowering, SseEmitters::align_right },

	{ ssse3, "HorizontalAdd", "VV", false, ssse3_lowering,
	  SseEmitters::horizontal_int<Intr::x86_ssse3_phadd_w_128, Intr::x86_ssse3_phadd_d_128> },
	{ ssse3, "HorizontalAddSaturate", "VV", false, ssse3_lowering,
	  SseEmitters::binary_intrinsic<Intr::x86_ssse3_phadd_sw_128> },
	{ ssse3, "HorizontalSubtract", "VV", false, ssse3_lowering,
	  SseEmitters::horizontal_int<Intr::x86_ssse3_phsub_w_128, Intr::x86_ssse3_phsub_d_128> },
	{ ssse3, "HorizontalSubtractSaturate", "VV", false, ssse3_lowering,
	  SseEmitters::binary_intrinsic<Intr::x86_ssse3_phsub_sw_128> },

	{ ssse3, "MultiplyAddAdjacent", "VV", false, ssse3_lowering,
	  SseEmitters::binary_intrinsic<Intr::x86_ssse3_pmadd_ub_sw_128> },
	{ ssse3, "MultiplyHighRoundScale", "VV", false, ssse3_lowering,
	  SseEmitters::binary_intrinsic<Intr::x86_ssse3_pmul_hr_sw_128> },

	{ ssse3, "Shuffle", "VV", false, ssse3_lowering,
	  SseEmitters::binary_intrinsic<Intr::x86_ssse3_pshuf_b_128> },

	{ ssse3, "Sign", "VV", false, ssse3_lowering,
	  SseEmitters::sign<Intr::x86_ssse3_psign_b_128, Intr::x86_ssse3_psign_w_128,
	                    Intr::x86_ssse3_psign_d_128> },

	{ ssse3, {}, any_signature, false, nullptr, SseEmitters::unimplemented },
};

} // namespace

llvm::ArrayRef<BuiltinBody>
simd_x86_bodies ()
{
	static const std::vector<BuiltinBody> all = [] {
		std::vector<BuiltinBody> made (std::begin (sse_table), std::end (sse_table));
		made.insert (made.end (), std::begin (sse2_table), std::end (sse2_table));
		made.insert (made.end (), std::begin (sse3_table), std::end (sse3_table));
		made.insert (made.end (), std::begin (ssse3_table), std::end (ssse3_table));
		return made;
	} ();

	return all;
}

} // namespace mono

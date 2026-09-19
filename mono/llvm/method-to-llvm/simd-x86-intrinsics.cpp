/**
 * \file
 * \brief LLVM lowering for System.Runtime.Intrinsics.X86.*.
 */

#include "intrinsics.hpp"

#include "../runtime/options.hpp"
#include "method-to-llvm.hpp"
#include "simd-emit.hpp"

#include "mono/metadata/class-internals.h"
#include "mono/utils/mono-hwcap.h"

#include <llvm/ADT/STLFunctionalExtras.h>
#include <llvm/IR/BasicBlock.h>
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

bool sse41_lowering ()
{
	return mono_hwcap_x86_has_sse41;
}

bool sse42_lowering ()
{
	return mono_hwcap_x86_has_sse42;
}

bool avx_lowering ()
{
	return mono_hwcap_x86_has_avx;
}

bool avx2_lowering ()
{
	return mono_hwcap_x86_has_avx2;
}

bool popcnt_lowering ()
{
	return mono_hwcap_x86_has_popcnt;
}

bool lzcnt_lowering ()
{
	return mono_hwcap_x86_has_lzcnt;
}

bool bmi1_lowering ()
{
	return mono_hwcap_x86_has_bmi1;
}

bool bmi2_lowering ()
{
	return mono_hwcap_x86_has_bmi2;
}

bool aes_lowering ()
{
	return mono_hwcap_x86_has_aes;
}

/// Return whether the Vector128<T> parameter at index has an unsigned element type.
/// LLVM vector types do not encode signedness, so some lowerings must recover it
/// from the managed signature.
bool param_is_unsigned (MonoMethod *method, int index)
{
	MonoMethodSignature *sig = mono_method_signature_internal (method);
	MonoClass *klass = mono_class_from_mono_type_internal (sig->params[index]);

	if (klass == nullptr || !mono_class_is_ginst (klass))
		return false;

	MonoGenericInst *inst = mono_class_get_generic_class (klass)->context.class_inst;

	if (inst == nullptr || inst->type_argc < 1)
		return false;

	switch (inst->type_argv[0]->type) {
	case MONO_TYPE_U1:
	case MONO_TYPE_U2:
	case MONO_TYPE_U4:
	case MONO_TYPE_U8:
		return true;
	default:
		return false;
	}
}

/// Build a shuffle mask that selects the first n lanes.
std::vector<int> low_lanes_mask (unsigned n)
{
	std::vector<int> mask (n);
	for (unsigned i = 0; i < n; i++)
		mask[i] = (int) i;
	return mask;
}

/// Returns the number of elements in one 128-bit half of value.
unsigned half_lanes (llvm::Value *value)
{
	llvm::Type *elem = llvm::cast<llvm::FixedVectorType> (value->getType ())->getElementType ();
	return 128 / elem->getScalarSizeInBits ();
}

bool is_256 (llvm::Value *value)
{
	return llvm::cast<llvm::FixedVectorType> (value->getType ())
		       ->getPrimitiveSizeInBits ()
		       .getFixedValue () == 256;
}

/// Selects the low or high 128-bit half of value using the low bit of index.
llvm::Value *avx_low_or_high (llvm::IRBuilder<> &builder, llvm::Value *value, llvm::Value *index)
{
	unsigned half = llvm::cast<llvm::FixedVectorType> (value->getType ())->getNumElements () / 2;
	llvm::Value *low = builder.CreateShuffleVector (value, low_lanes_mask (half));
	std::vector<int> high_mask (half);

	for (unsigned i = 0; i < half; i++)
		high_mask[i] = (int) (half + i);

	llvm::Value *high = builder.CreateShuffleVector (value, high_mask);
	llvm::Value *bit0 =
		builder.CreateICmpNE (builder.CreateAnd (index, builder.getInt8 (1)), builder.getInt8 (0));

	return builder.CreateSelect (bit0, high, low);
}

/// Replaces the low or high 128-bit half of value, selected by index bit 0.
llvm::Value *avx_insert_half (llvm::IRBuilder<> &builder, llvm::Value *value, llvm::Value *data,
                              llvm::Value *index)
{
	unsigned lanes = llvm::cast<llvm::FixedVectorType> (value->getType ())->getNumElements ();
	unsigned half = lanes / 2;
	std::vector<int> extend_mask (half * 2, -1);

	for (unsigned i = 0; i < half; i++)
		extend_mask[i] = (int) i;

	llvm::Value *data_ext = builder.CreateShuffleVector (data, extend_mask);
	std::vector<int> low_mask (lanes), high_mask (lanes);

	for (unsigned i = 0; i < lanes; i++) {
		low_mask[i] = i < half ? (int) i : (int) (lanes + i);
		high_mask[i] = i < half ? (int) (lanes + i) : (int) (i - half);
	}

	llvm::Value *low_result = builder.CreateShuffleVector (data_ext, value, low_mask);
	llvm::Value *high_result = builder.CreateShuffleVector (data_ext, value, high_mask);
	llvm::Value *bit0 =
		builder.CreateICmpNE (builder.CreateAnd (index, builder.getInt8 (1)), builder.getInt8 (0));

	return builder.CreateSelect (bit0, high_result, low_result);
}
/// Dispatch a runtime selector to blocks that call emit_case with a constant value.
/// This permits use of LLVM intrinsics whose control operands must be immediate.
llvm::Value *dispatch_control (llvm::IRBuilder<> &builder, llvm::Value *selector,
                               unsigned case_count, llvm::Type *result_type,
                               llvm::function_ref<llvm::Value *(unsigned)> emit_case)
{
	llvm::LLVMContext &ctx = builder.getContext ();
	llvm::Function *function = builder.GetInsertBlock ()->getParent ();
	std::vector<llvm::BasicBlock *> blocks (case_count);

	for (unsigned i = 0; i < case_count; i++)
		blocks[i] = llvm::BasicBlock::Create (ctx, "", function);

	llvm::BasicBlock *merge = llvm::BasicBlock::Create (ctx, "", function);
	llvm::SwitchInst *sw = builder.CreateSwitch (selector, blocks[0], case_count);

	for (unsigned i = 0; i < case_count; i++)
		sw->addCase (llvm::ConstantInt::get (llvm::Type::getInt8Ty (ctx), i), blocks[i]);

	builder.SetInsertPoint (merge);
	llvm::PHINode *phi = builder.CreatePHI (result_type, case_count);

	for (unsigned i = 0; i < case_count; i++) {
		builder.SetInsertPoint (blocks[i]);

		llvm::Value *result = emit_case (i);
		llvm::BasicBlock *end_block = builder.GetInsertBlock ();

		builder.CreateBr (merge);
		phi->addIncoming (result, end_block);
	}

	builder.SetInsertPoint (merge);
	return phi;
}

/// Dispatch runtime gather scales to calls with constant operands.
llvm::Value *dispatch_scale (llvm::IRBuilder<> &builder, llvm::Value *scale, llvm::Type *result_type,
                             llvm::function_ref<llvm::Value *(uint8_t)> emit_case)
{
	static const uint8_t values[] = { 1, 2, 4, 8 };
	const unsigned case_count = 4;
	llvm::LLVMContext &ctx = builder.getContext ();
	llvm::Function *function = builder.GetInsertBlock ()->getParent ();
	std::vector<llvm::BasicBlock *> blocks (case_count);

	for (unsigned i = 0; i < case_count; i++)
		blocks[i] = llvm::BasicBlock::Create (ctx, "", function);

	llvm::BasicBlock *merge = llvm::BasicBlock::Create (ctx, "", function);
	llvm::SwitchInst *sw = builder.CreateSwitch (scale, blocks[case_count - 1], case_count);

	for (unsigned i = 0; i < case_count; i++)
		sw->addCase (llvm::ConstantInt::get (llvm::Type::getInt8Ty (ctx), values[i]), blocks[i]);

	builder.SetInsertPoint (merge);
	llvm::PHINode *phi = builder.CreatePHI (result_type, case_count);

	for (unsigned i = 0; i < case_count; i++) {
		builder.SetInsertPoint (blocks[i]);

		llvm::Value *result = emit_case (values[i]);
		llvm::BasicBlock *end_block = builder.GetInsertBlock ();

		builder.CreateBr (merge);
		phi->addIncoming (result, end_block);
	}

	builder.SetInsertPoint (merge);
	return phi;
}

/// Encode element width and signedness in bits 1:0 of a PCMPxSTR control byte.
/// Recover both properties from the managed signature after vector normalization.
uint8_t string_format_bits (MonoMethod *method, llvm::Value *left)
{
	bool byte_format =
		llvm::cast<llvm::FixedVectorType> (left->getType ())->getNumElements () == 16;
	bool is_unsigned = param_is_unsigned (method, 0);

	if (byte_format)
		return is_unsigned ? 0b00 : 0b10;
	return is_unsigned ? 0b01 : 0b11;
}

/// Cast an operand to the <16 x i8> type required by LLVM's PCMPxSTR intrinsics.
llvm::Value *as_v16i8 (llvm::IRBuilder<> &builder, llvm::Value *value)
{
	return builder.CreateBitCast (
		value, llvm::FixedVectorType::get (builder.getInt8Ty (), 16));
}

/// Cast an operand to the <2 x i64> type required by LLVM's AES-NI intrinsics.
llvm::Value *as_v2i64 (llvm::IRBuilder<> &builder, llvm::Value *value)
{
	return builder.CreateBitCast (
		value, llvm::FixedVectorType::get (builder.getInt64Ty (), 2));
}

/// Return the PCMPxSTR flag intrinsic for a ResultsFlag value.
std::optional<llvm::Intrinsic::ID> string_flag_intrinsic (bool explicit_length, uint8_t flag)
{
	// Indexed by ResultsFlag: CFlag, NotCFlagAndNotZFlag, OFlag, SFlag, ZFlag.
	static const llvm::Intrinsic::ID implicit_ids[] = {
		llvm::Intrinsic::x86_sse42_pcmpistric128, llvm::Intrinsic::x86_sse42_pcmpistria128,
		llvm::Intrinsic::x86_sse42_pcmpistrio128, llvm::Intrinsic::x86_sse42_pcmpistris128,
		llvm::Intrinsic::x86_sse42_pcmpistriz128,
	};
	static const llvm::Intrinsic::ID explicit_ids[] = {
		llvm::Intrinsic::x86_sse42_pcmpestric128, llvm::Intrinsic::x86_sse42_pcmpestria128,
		llvm::Intrinsic::x86_sse42_pcmpestrio128, llvm::Intrinsic::x86_sse42_pcmpestris128,
		llvm::Intrinsic::x86_sse42_pcmpestriz128,
	};

	if (flag >= 5)
		return std::nullopt;
	return explicit_length ? explicit_ids[flag] : implicit_ids[flag];
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

	static BuiltinResult sse41_is_supported (MethodLLVMEmitter &, llvm::IRBuilder<> &builder,
	                                         MonoMethod *)
	{
		return is_supported (mono_hwcap_x86_has_sse41, builder);
	}

	static BuiltinResult sse42_is_supported (MethodLLVMEmitter &, llvm::IRBuilder<> &builder,
	                                         MonoMethod *)
	{
		return is_supported (mono_hwcap_x86_has_sse42, builder);
	}

	static BuiltinResult avx_is_supported (MethodLLVMEmitter &, llvm::IRBuilder<> &builder,
	                                       MonoMethod *)
	{
		return is_supported (mono_hwcap_x86_has_avx, builder);
	}

	static BuiltinResult avx2_is_supported (MethodLLVMEmitter &, llvm::IRBuilder<> &builder,
	                                        MonoMethod *)
	{
		return is_supported (mono_hwcap_x86_has_avx2, builder);
	}

	static BuiltinResult popcnt_is_supported (MethodLLVMEmitter &, llvm::IRBuilder<> &builder,
	                                          MonoMethod *)
	{
		return is_supported (mono_hwcap_x86_has_popcnt, builder);
	}

	static BuiltinResult lzcnt_is_supported (MethodLLVMEmitter &, llvm::IRBuilder<> &builder,
	                                         MonoMethod *)
	{
		return is_supported (mono_hwcap_x86_has_lzcnt, builder);
	}

	static BuiltinResult bmi1_is_supported (MethodLLVMEmitter &, llvm::IRBuilder<> &builder,
	                                        MonoMethod *)
	{
		return is_supported (mono_hwcap_x86_has_bmi1, builder);
	}

	static BuiltinResult bmi2_is_supported (MethodLLVMEmitter &, llvm::IRBuilder<> &builder,
	                                        MonoMethod *)
	{
		return is_supported (mono_hwcap_x86_has_bmi2, builder);
	}

	static BuiltinResult aes_is_supported (MethodLLVMEmitter &, llvm::IRBuilder<> &builder,
	                                       MonoMethod *)
	{
		return is_supported (mono_hwcap_x86_has_aes, builder);
	}

	static bool is_double_vector (llvm::Value *value)
	{
		return llvm::cast<llvm::FixedVectorType> (value->getType ())
			->getElementType ()->isDoubleTy ();
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

	/// Returns an i32 vector type with the same total width as value.
	static llvm::Type *i32_lanes (llvm::LLVMContext &ctx, llvm::Type *value)
	{
		unsigned bits = llvm::cast<llvm::FixedVectorType> (value)
			->getPrimitiveSizeInBits ().getFixedValue ();
		return llvm::FixedVectorType::get (llvm::Type::getInt32Ty (ctx), bits / 32);
	}

	template <llvm::BinaryOperator::BinaryOps op>
	static BuiltinResult bitwise (MethodLLVMEmitter &emitter, llvm::IRBuilder<> &builder,
	                              MonoMethod *)
	{
		llvm::Value *left = argument (emitter, 0);
		llvm::Value *right = argument (emitter, 1);
		llvm::Type *original = left->getType ();
		llvm::Type *i32xn = i32_lanes (context (emitter), original);

		builder.CreateRet (builder.CreateBitCast (
			builder.CreateBinOp (op, builder.CreateBitCast (left, i32xn),
			                     builder.CreateBitCast (right, i32xn)),
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
		llvm::Type *i32xn = i32_lanes (context (emitter), original);

		builder.CreateRet (builder.CreateBitCast (
			builder.CreateAnd (builder.CreateNot (builder.CreateBitCast (left, i32xn)),
			                   builder.CreateBitCast (right, i32xn)),
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

	/// Implement immediate-controlled blends explicitly because the control byte is
	/// a runtime value in the managed intrinsic body.
	static BuiltinResult blend (MethodLLVMEmitter &emitter, llvm::IRBuilder<> &builder, MonoMethod *)
	{
		llvm::Value *left = argument (emitter, 0);
		llvm::Value *right = argument (emitter, 1);
		llvm::Value *control = argument (emitter, 2);
		unsigned lanes = llvm::cast<llvm::FixedVectorType> (left->getType ())->getNumElements ();
		llvm::Value *result = left;

		for (unsigned i = 0; i < lanes; i++) {
			llvm::Value *bit = builder.CreateICmpNE (
				builder.CreateAnd (control, builder.getInt8 (1u << i)), builder.getInt8 (0));
			result = builder.CreateInsertElement (
				result,
				builder.CreateSelect (bit, builder.CreateExtractElement (right, i),
				                      builder.CreateExtractElement (left, i)),
				i);
		}

		builder.CreateRet (result);
		return llvm::Error::success ();
	}

	/// Select PBLENDVB, BLENDVPS, or BLENDVPD from the element type.
	static BuiltinResult blend_variable (MethodLLVMEmitter &emitter, llvm::IRBuilder<> &builder,
	                                     MonoMethod *)
	{
		llvm::Value *left = argument (emitter, 0);
		llvm::Value *right = argument (emitter, 1);
		llvm::Value *mask = argument (emitter, 2);
		llvm::Type *elem =
			llvm::cast<llvm::FixedVectorType> (left->getType ())->getElementType ();
		llvm::Intrinsic::ID id = elem->isDoubleTy ()  ? llvm::Intrinsic::x86_sse41_blendvpd
		                         : elem->isFloatTy () ? llvm::Intrinsic::x86_sse41_blendvps
		                                              : llvm::Intrinsic::x86_sse41_pblendvb;

		builder.CreateRet (builder.CreateIntrinsic (id, {}, { left, right, mask }));
		return llvm::Error::success ();
	}

	/// Values 8 through 11 select a rounding mode and suppress precision exceptions;
	/// value 4 uses the current MXCSR rounding mode.
	template <int imm8>
	static BuiltinResult round (MethodLLVMEmitter &emitter, llvm::IRBuilder<> &builder, MonoMethod *)
	{
		llvm::Value *value = argument (emitter, 0);

		builder.CreateRet (builder.CreateIntrinsic (
			is_double_vector (value) ? llvm::Intrinsic::x86_sse41_round_pd : llvm::Intrinsic::x86_sse41_round_ps, {},
			{ value, builder.getInt32 (imm8) }));
		return llvm::Error::success ();
	}

	/// Pass the value as both scalar-round operands to preserve its upper lanes.
	template <int imm8>
	static BuiltinResult round_scalar1 (MethodLLVMEmitter &emitter, llvm::IRBuilder<> &builder,
	                                    MonoMethod *)
	{
		llvm::Value *value = argument (emitter, 0);

		builder.CreateRet (builder.CreateIntrinsic (
			is_double_vector (value) ? llvm::Intrinsic::x86_sse41_round_sd : llvm::Intrinsic::x86_sse41_round_ss, {},
			{ value, value, builder.getInt32 (imm8) }));
		return llvm::Error::success ();
	}

	/// Preserve the upper lanes from the first scalar-round operand.
	template <int imm8>
	static BuiltinResult round_scalar2 (MethodLLVMEmitter &emitter, llvm::IRBuilder<> &builder,
	                                    MonoMethod *)
	{
		llvm::Value *upper = argument (emitter, 0);
		llvm::Value *value = argument (emitter, 1);

		builder.CreateRet (builder.CreateIntrinsic (
			is_double_vector (upper) ? llvm::Intrinsic::x86_sse41_round_sd : llvm::Intrinsic::x86_sse41_round_ss, {},
			{ upper, value, builder.getInt32 (imm8) }));
		return llvm::Error::success ();
	}

	/// Extend the required low lanes to the return type. Signedness comes from the
	/// managed signature because LLVM vector types do not encode it.
	static BuiltinResult convert_widen (MethodLLVMEmitter &emitter, llvm::IRBuilder<> &builder,
	                                    MonoMethod *method)
	{
		llvm::Value *value = argument (emitter, 0);
		llvm::Type *dest = return_type (emitter);
		unsigned lanes = llvm::cast<llvm::FixedVectorType> (dest)->getNumElements ();
		llvm::Value *narrow = builder.CreateShuffleVector (value, low_lanes_mask (lanes));

		builder.CreateRet (param_is_unsigned (method, 0) ? builder.CreateZExt (narrow, dest)
		                                                 : builder.CreateSExt (narrow, dest));
		return llvm::Error::success ();
	}

	/// Build the dot product explicitly because its control byte is a runtime value
	/// in the managed intrinsic body.
	static BuiltinResult dot_product (MethodLLVMEmitter &emitter, llvm::IRBuilder<> &builder,
	                                  MonoMethod *)
	{
		llvm::Value *left = argument (emitter, 0);
		llvm::Value *right = argument (emitter, 1);
		llvm::Value *control = builder.CreateZExt (argument (emitter, 2), builder.getInt32Ty ());
		llvm::Type *vector_type = left->getType ();
		llvm::Type *elem = llvm::cast<llvm::FixedVectorType> (vector_type)->getElementType ();
		unsigned lanes = llvm::cast<llvm::FixedVectorType> (vector_type)->getNumElements ();
		llvm::Value *zero = llvm::ConstantFP::get (elem, 0.0);
		llvm::Value *product = builder.CreateFMul (left, right);
		llvm::Value *sum = zero;

		for (unsigned i = 0; i < lanes; i++) {
			llvm::Value *bit = builder.CreateICmpNE (
				builder.CreateAnd (control, builder.getInt32 (0x10u << i)), builder.getInt32 (0));
			sum = builder.CreateFAdd (
				sum, builder.CreateSelect (bit, builder.CreateExtractElement (product, i), zero));
		}

		llvm::Value *result = llvm::Constant::getNullValue (vector_type);

		for (unsigned i = 0; i < lanes; i++) {
			llvm::Value *bit = builder.CreateICmpNE (
				builder.CreateAnd (control, builder.getInt32 (1u << i)), builder.getInt32 (0));
			result = builder.CreateInsertElement (result, builder.CreateSelect (bit, sum, zero), i);
		}

		builder.CreateRet (result);
		return llvm::Error::success ();
	}

	/// Mask the index to reproduce the hardware instruction's lane wraparound.
	static BuiltinResult extract (MethodLLVMEmitter &emitter, llvm::IRBuilder<> &builder, MonoMethod *)
	{
		llvm::Value *value = argument (emitter, 0);
		unsigned lanes = llvm::cast<llvm::FixedVectorType> (value->getType ())->getNumElements ();
		llvm::Value *index = builder.CreateAnd (
			builder.CreateZExt (argument (emitter, 1), builder.getInt32Ty ()), lanes - 1);

		builder.CreateRet (builder.CreateExtractElement (value, index));
		return llvm::Error::success ();
	}

	/// Mask the index to reproduce the hardware instruction's lane wraparound.
	static BuiltinResult insert (MethodLLVMEmitter &emitter, llvm::IRBuilder<> &builder, MonoMethod *)
	{
		llvm::Value *value = argument (emitter, 0);
		llvm::Value *data = argument (emitter, 1);
		unsigned lanes = llvm::cast<llvm::FixedVectorType> (value->getType ())->getNumElements ();
		llvm::Value *index = builder.CreateAnd (
			builder.CreateZExt (argument (emitter, 2), builder.getInt32Ty ()), lanes - 1);

		builder.CreateRet (builder.CreateInsertElement (value, data, index));
		return llvm::Error::success ();
	}

	/// Decode INSERTPS explicitly because its source lane, destination lane, and
	/// zero mask come from a runtime control byte.
	static BuiltinResult insert_ps (MethodLLVMEmitter &emitter, llvm::IRBuilder<> &builder,
	                                MonoMethod *)
	{
		llvm::Value *value = argument (emitter, 0);
		llvm::Value *data = argument (emitter, 1);
		llvm::Value *control = builder.CreateZExt (argument (emitter, 2), builder.getInt32Ty ());
		llvm::Value *src_lane = builder.CreateAnd (builder.CreateLShr (control, 6), 3);
		llvm::Value *dst_lane = builder.CreateAnd (builder.CreateLShr (control, 4), 3);
		llvm::Value *scalar = builder.CreateExtractElement (data, src_lane);
		llvm::Value *inserted = builder.CreateInsertElement (value, scalar, dst_lane);
		llvm::Value *zero = llvm::Constant::getNullValue (inserted->getType ());
		llvm::Value *result = inserted;

		for (unsigned i = 0; i < 4; i++) {
			llvm::Value *bit = builder.CreateICmpNE (
				builder.CreateAnd (control, builder.getInt32 (1u << i)), builder.getInt32 (0));
			result = builder.CreateInsertElement (
				result,
				builder.CreateSelect (bit, builder.CreateExtractElement (zero, i),
				                      builder.CreateExtractElement (result, i)),
				i);
		}

		builder.CreateRet (result);
		return llvm::Error::success ();
	}

	/// Select signed or unsigned min/max from the managed parameter type because
	/// LLVM vector types do not encode signedness.
	template <bool want_max>
	static BuiltinResult extremum (MethodLLVMEmitter &emitter, llvm::IRBuilder<> &builder,
	                               MonoMethod *method)
	{
		llvm::Value *lhs = argument (emitter, 0);
		llvm::Value *rhs = argument (emitter, 1);
		llvm::Intrinsic::ID id = param_is_unsigned (method, 0)
			? (want_max ? llvm::Intrinsic::umax : llvm::Intrinsic::umin)
			: (want_max ? llvm::Intrinsic::smax : llvm::Intrinsic::smin);

		builder.CreateRet (builder.CreateIntrinsic (id, { lhs->getType () }, { lhs, rhs }));
		return llvm::Error::success ();
	}

	/// Decode MPSADBW's runtime control byte explicitly. Bit 2 selects the left
	/// sliding-window base, while bits 1:0 select the right four-byte block.
	static BuiltinResult multiple_sum_abs_diff (MethodLLVMEmitter &emitter,
	                                            llvm::IRBuilder<> &builder, MonoMethod *)
	{
		llvm::Value *left = argument (emitter, 0);
		llvm::Value *right = argument (emitter, 1);
		llvm::Value *control = builder.CreateZExt (argument (emitter, 2), builder.getInt32Ty ());
		llvm::Value *left_block =
			builder.CreateShl (builder.CreateAnd (builder.CreateLShr (control, 2), 1), 2);
		llvm::Value *right_block = builder.CreateShl (builder.CreateAnd (control, 3), 2);
		llvm::Type *i16 = builder.getInt16Ty ();
		llvm::Value *result = llvm::Constant::getNullValue (return_type (emitter));

		for (unsigned i = 0; i < 8; i++) {
			llvm::Value *sum = builder.getInt16 (0);

			for (unsigned j = 0; j < 4; j++) {
				llvm::Value *left_index =
					builder.CreateAdd (left_block, builder.getInt32 (i + j));
				llvm::Value *right_index = builder.CreateAdd (right_block, builder.getInt32 (j));
				llvm::Value *a =
					builder.CreateZExt (builder.CreateExtractElement (left, left_index), i16);
				llvm::Value *b =
					builder.CreateZExt (builder.CreateExtractElement (right, right_index), i16);
				llvm::Value *diff = builder.CreateSub (a, b);

				sum = builder.CreateAdd (
					sum,
					builder.CreateIntrinsic (llvm::Intrinsic::abs, { i16 }, { diff, builder.getFalse () }));
			}

			result = builder.CreateInsertElement (result, sum, i);
		}

		builder.CreateRet (result);
		return llvm::Error::success ();
	}

	static BuiltinResult multiply_low (MethodLLVMEmitter &emitter, llvm::IRBuilder<> &builder,
	                                   MonoMethod *)
	{
		builder.CreateRet (builder.CreateMul (argument (emitter, 0), argument (emitter, 1)));
		return llvm::Error::success ();
	}

	/// LLVM has no PMULDQ intrinsic, so extend and multiply the even-numbered lanes.
	static BuiltinResult multiply_widen (MethodLLVMEmitter &emitter, llvm::IRBuilder<> &builder,
	                                     MonoMethod *)
	{
		llvm::Value *lhs = argument (emitter, 0);
		llvm::Value *rhs = argument (emitter, 1);
		llvm::Type *wide = return_type (emitter);
		llvm::Value *lhs_even =
			builder.CreateSExt (builder.CreateShuffleVector (lhs, { 0, 2 }), wide);
		llvm::Value *rhs_even =
			builder.CreateSExt (builder.CreateShuffleVector (rhs, { 0, 2 }), wide);

		builder.CreateRet (builder.CreateMul (lhs_even, rhs_even));
		return llvm::Error::success ();
	}

	/// Cast operands to the <2 x i64> type required by LLVM's PTEST intrinsics.
	template <llvm::Intrinsic::ID id>
	static BuiltinResult test (MethodLLVMEmitter &emitter, llvm::IRBuilder<> &builder, MonoMethod *)
	{
		llvm::Type *i64x2 =
			llvm::FixedVectorType::get (llvm::Type::getInt64Ty (context (emitter)), 2);
		llvm::Value *left = builder.CreateBitCast (argument (emitter, 0), i64x2);
		llvm::Value *right = builder.CreateBitCast (argument (emitter, 1), i64x2);

		builder.CreateRet (builder.CreateTrunc (
			builder.CreateIntrinsic (id, {}, { left, right }), return_type (emitter)));
		return llvm::Error::success ();
	}

	/// Implement TestAllOnes as PTESTC against an all-ones mask.
	static BuiltinResult test_all_ones (MethodLLVMEmitter &emitter, llvm::IRBuilder<> &builder,
	                                    MonoMethod *)
	{
		llvm::Type *i64x2 =
			llvm::FixedVectorType::get (llvm::Type::getInt64Ty (context (emitter)), 2);
		llvm::Value *value = builder.CreateBitCast (argument (emitter, 0), i64x2);
		llvm::Value *all_ones = llvm::Constant::getAllOnesValue (i64x2);

		builder.CreateRet (builder.CreateTrunc (
			builder.CreateIntrinsic (llvm::Intrinsic::x86_sse41_ptestc, {}, { value, all_ones }),
			return_type (emitter)));
		return llvm::Error::success ();
	}

	/// Lower LoadDquVector128 with LDDQU rather than a generic unaligned load.
	static BuiltinResult load_dqu (MethodLLVMEmitter &emitter, llvm::IRBuilder<> &builder,
	                               MonoMethod *)
	{
		bool is256 = llvm::cast<llvm::FixedVectorType> (return_type (emitter))
			             ->getPrimitiveSizeInBits ().getFixedValue () == 256;
		llvm::Value *loaded = builder.CreateIntrinsic (
			is256 ? llvm::Intrinsic::x86_avx_ldu_dq_256 : llvm::Intrinsic::x86_sse3_ldu_dq, {},
			{ argument (emitter, 0) });

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

	static BuiltinResult crc32 (MethodLLVMEmitter &emitter, llvm::IRBuilder<> &builder, MonoMethod *)
	{
		llvm::Value *crc = argument (emitter, 0);
		llvm::Value *data = argument (emitter, 1);
		unsigned data_width = data->getType ()->getIntegerBitWidth ();
		llvm::Intrinsic::ID id = data_width == 64  ? llvm::Intrinsic::x86_sse42_crc32_64_64
		                         : data_width == 32 ? llvm::Intrinsic::x86_sse42_crc32_32_32
		                         : data_width == 16 ? llvm::Intrinsic::x86_sse42_crc32_32_16
		                                            : llvm::Intrinsic::x86_sse42_crc32_32_8;

		builder.CreateRet (builder.CreateIntrinsic (id, {}, { crc, data }));
		return llvm::Error::success ();
	}

	static BuiltinResult popcount (MethodLLVMEmitter &emitter, llvm::IRBuilder<> &builder, MonoMethod *)
	{
		llvm::Value *value = argument (emitter, 0);

		builder.CreateRet (
			builder.CreateIntrinsic (llvm::Intrinsic::ctpop, { value->getType () }, { value }));
		return llvm::Error::success ();
	}

	/// LZCNT's result for a zero operand is the operand width, not poison.
	static BuiltinResult leading_zero_count (MethodLLVMEmitter &emitter, llvm::IRBuilder<> &builder,
	                                         MonoMethod *)
	{
		llvm::Value *value = argument (emitter, 0);

		builder.CreateRet (builder.CreateIntrinsic (
			llvm::Intrinsic::ctlz, { value->getType () }, { value, builder.getFalse () }));
		return llvm::Error::success ();
	}

	static BuiltinResult and_not_scalar (MethodLLVMEmitter &emitter, llvm::IRBuilder<> &builder,
	                                     MonoMethod *)
	{
		llvm::Value *left = argument (emitter, 0);
		llvm::Value *right = argument (emitter, 1);

		builder.CreateRet (builder.CreateAnd (builder.CreateNot (left), right));
		return llvm::Error::success ();
	}

	static llvm::Intrinsic::ID bextr_id (unsigned width)
	{
		return width == 64 ? llvm::Intrinsic::x86_bmi_bextr_64 : llvm::Intrinsic::x86_bmi_bextr_32;
	}

	static BuiltinResult bit_field_extract_start_length (MethodLLVMEmitter &emitter,
	                                                     llvm::IRBuilder<> &builder, MonoMethod *)
	{
		llvm::Value *value = argument (emitter, 0);
		llvm::Type *width = value->getType ();
		llvm::Value *start = builder.CreateZExt (argument (emitter, 1), width);
		llvm::Value *length = builder.CreateZExt (argument (emitter, 2), width);
		llvm::Value *control =
			builder.CreateOr (start, builder.CreateShl (length, llvm::ConstantInt::get (width, 8)));

		builder.CreateRet (builder.CreateIntrinsic (bextr_id (width->getIntegerBitWidth ()), {},
		                                            { value, control }));
		return llvm::Error::success ();
	}

	static BuiltinResult bit_field_extract_control (MethodLLVMEmitter &emitter,
	                                                llvm::IRBuilder<> &builder, MonoMethod *)
	{
		llvm::Value *value = argument (emitter, 0);
		llvm::Type *width = value->getType ();
		llvm::Value *control = builder.CreateZExt (argument (emitter, 1), width);

		builder.CreateRet (builder.CreateIntrinsic (bextr_id (width->getIntegerBitWidth ()), {},
		                                            { value, control }));
		return llvm::Error::success ();
	}

	static BuiltinResult extract_lowest_set_bit (MethodLLVMEmitter &emitter,
	                                             llvm::IRBuilder<> &builder, MonoMethod *)
	{
		llvm::Value *value = argument (emitter, 0);

		builder.CreateRet (builder.CreateAnd (value, builder.CreateNeg (value)));
		return llvm::Error::success ();
	}

	static BuiltinResult get_mask_up_to_lowest_set_bit (MethodLLVMEmitter &emitter,
	                                                    llvm::IRBuilder<> &builder, MonoMethod *)
	{
		llvm::Value *value = argument (emitter, 0);
		llvm::Value *one = llvm::ConstantInt::get (value->getType (), 1);

		builder.CreateRet (builder.CreateXor (value, builder.CreateSub (value, one)));
		return llvm::Error::success ();
	}

	static BuiltinResult reset_lowest_set_bit (MethodLLVMEmitter &emitter,
	                                           llvm::IRBuilder<> &builder, MonoMethod *)
	{
		llvm::Value *value = argument (emitter, 0);
		llvm::Value *one = llvm::ConstantInt::get (value->getType (), 1);

		builder.CreateRet (builder.CreateAnd (value, builder.CreateSub (value, one)));
		return llvm::Error::success ();
	}

	static BuiltinResult trailing_zero_count (MethodLLVMEmitter &emitter, llvm::IRBuilder<> &builder,
	                                          MonoMethod *)
	{
		llvm::Value *value = argument (emitter, 0);

		builder.CreateRet (builder.CreateIntrinsic (
			llvm::Intrinsic::cttz, { value->getType () }, { value, builder.getFalse () }));
		return llvm::Error::success ();
	}

	static BuiltinResult zero_high_bits (MethodLLVMEmitter &emitter, llvm::IRBuilder<> &builder,
	                                     MonoMethod *)
	{
		llvm::Value *value = argument (emitter, 0);
		llvm::Value *index = argument (emitter, 1);
		llvm::Intrinsic::ID id = value->getType ()->getIntegerBitWidth () == 64
			? llvm::Intrinsic::x86_bmi_bzhi_64
			: llvm::Intrinsic::x86_bmi_bzhi_32;

		builder.CreateRet (builder.CreateIntrinsic (id, {}, { value, index }));
		return llvm::Error::success ();
	}

	/// LLVM has no MULX intrinsic; widen, multiply, and split the result instead.
	static BuiltinResult multiply_no_flags (MethodLLVMEmitter &emitter, llvm::IRBuilder<> &builder,
	                                        MonoMethod *)
	{
		llvm::Value *left = argument (emitter, 0);
		llvm::Value *right = argument (emitter, 1);
		llvm::Value *high_out = argument (emitter, 2);
		llvm::Type *narrow = left->getType ();
		unsigned bits = narrow->getIntegerBitWidth ();
		llvm::Type *wide = builder.getIntNTy (bits * 2);
		llvm::Value *product =
			builder.CreateMul (builder.CreateZExt (left, wide), builder.CreateZExt (right, wide));
		llvm::Value *high = builder.CreateTrunc (
			builder.CreateLShr (product, llvm::ConstantInt::get (wide, bits)), narrow);

		builder.CreateStore (high, high_out);
		builder.CreateRet (builder.CreateTrunc (product, narrow));
		return llvm::Error::success ();
	}

	static BuiltinResult parallel_bit_deposit (MethodLLVMEmitter &emitter, llvm::IRBuilder<> &builder,
	                                           MonoMethod *)
	{
		llvm::Value *value = argument (emitter, 0);
		llvm::Value *mask = argument (emitter, 1);

		builder.CreateRet (
			builder.CreateIntrinsic (llvm::Intrinsic::pdep, { value->getType () }, { value, mask }));
		return llvm::Error::success ();
	}

	static BuiltinResult parallel_bit_extract (MethodLLVMEmitter &emitter, llvm::IRBuilder<> &builder,
	                                           MonoMethod *)
	{
		llvm::Value *value = argument (emitter, 0);
		llvm::Value *mask = argument (emitter, 1);

		builder.CreateRet (
			builder.CreateIntrinsic (llvm::Intrinsic::pext, { value->getType () }, { value, mask }));
		return llvm::Error::success ();
	}

	template <llvm::Intrinsic::ID id>
	static BuiltinResult aes_binary (MethodLLVMEmitter &emitter, llvm::IRBuilder<> &builder,
	                                 MonoMethod *)
	{
		llvm::Value *value = as_v2i64 (builder, argument (emitter, 0));
		llvm::Value *round_key = as_v2i64 (builder, argument (emitter, 1));
		llvm::Value *result = builder.CreateIntrinsic (id, {}, { value, round_key });

		builder.CreateRet (as_v16i8 (builder, result));
		return llvm::Error::success ();
	}

	static BuiltinResult aes_inverse_mix_columns (MethodLLVMEmitter &emitter,
	                                              llvm::IRBuilder<> &builder, MonoMethod *)
	{
		llvm::Value *value = as_v2i64 (builder, argument (emitter, 0));
		llvm::Value *result =
			builder.CreateIntrinsic (llvm::Intrinsic::x86_aesni_aesimc, {}, { value });

		builder.CreateRet (as_v16i8 (builder, result));
		return llvm::Error::success ();
	}

	static BuiltinResult aes_keygen_assist (MethodLLVMEmitter &emitter, llvm::IRBuilder<> &builder,
	                                        MonoMethod *)
	{
		llvm::Value *value = as_v2i64 (builder, argument (emitter, 0));
		llvm::Value *control = argument (emitter, 1);
		llvm::Value *result = dispatch_control (builder, control, 256, value->getType (), [&] (unsigned c) {
			return builder.CreateIntrinsic (llvm::Intrinsic::x86_aesni_aeskeygenassist, {},
			                                { value, builder.getInt8 (c) });
		});

		builder.CreateRet (as_v16i8 (builder, result));
		return llvm::Error::success ();
	}

	/// Extract the selected comparison-mode bits and normalize them for dispatch.
	static llvm::Value *aggregation_selector (llvm::IRBuilder<> &builder, llvm::Value *mode,
	                                          uint8_t mask)
	{
		return builder.CreateLShr (builder.CreateAnd (mode, builder.getInt8 (mask)),
		                           builder.getInt8 (2));
	}

	/// Dispatch runtime flag and comparison-mode values to PCMPxSTR calls with a
	/// constant control byte.
	static llvm::Value *dispatch_flag_and_mode (llvm::IRBuilder<> &builder, bool explicit_length,
	                                            llvm::Value *flag, llvm::Value *mode, uint8_t format,
	                                            llvm::ArrayRef<llvm::Value *> fixed_args)
	{
		llvm::Value *selector = aggregation_selector (builder, mode, 0x7C);
		llvm::Type *i32 = builder.getInt32Ty ();
		std::vector<llvm::Value *> call_args (fixed_args.begin (), fixed_args.end ());

		call_args.push_back (nullptr);
		return dispatch_control (
			builder, flag, 5, i32, [&] (unsigned flag_case) {
				llvm::Intrinsic::ID id = *string_flag_intrinsic (explicit_length, flag_case);

				return dispatch_control (builder, selector, 32, i32, [&] (unsigned mode_case) {
					uint8_t control = (uint8_t) ((mode_case << 2) | format);

					call_args.back () = builder.getInt8 (control);
					return builder.CreateIntrinsic (id, {}, call_args);
				});
			});
	}

	static BuiltinResult compare_implicit_length (MethodLLVMEmitter &emitter,
	                                              llvm::IRBuilder<> &builder, MonoMethod *method)
	{
		llvm::Value *left = argument (emitter, 0);
		llvm::Value *left8 = as_v16i8 (builder, left);
		llvm::Value *right8 = as_v16i8 (builder, argument (emitter, 1));
		llvm::Value *result = dispatch_flag_and_mode (
			builder, false, argument (emitter, 2), argument (emitter, 3),
			string_format_bits (method, left), { left8, right8 });

		builder.CreateRet (builder.CreateTrunc (result, return_type (emitter)));
		return llvm::Error::success ();
	}

	static BuiltinResult compare_explicit_length (MethodLLVMEmitter &emitter,
	                                              llvm::IRBuilder<> &builder, MonoMethod *method)
	{
		llvm::Value *left = argument (emitter, 0);
		llvm::Value *left8 = as_v16i8 (builder, left);
		llvm::Value *left_length = builder.CreateZExt (argument (emitter, 1), builder.getInt32Ty ());
		llvm::Value *right8 = as_v16i8 (builder, argument (emitter, 2));
		llvm::Value *right_length = builder.CreateZExt (argument (emitter, 3), builder.getInt32Ty ());
		llvm::Value *result = dispatch_flag_and_mode (
			builder, true, argument (emitter, 4), argument (emitter, 5),
			string_format_bits (method, left), { left8, left_length, right8, right_length });

		builder.CreateRet (builder.CreateTrunc (result, return_type (emitter)));
		return llvm::Error::success ();
	}

	static BuiltinResult compare_implicit_length_index (MethodLLVMEmitter &emitter,
	                                                     llvm::IRBuilder<> &builder,
	                                                     MonoMethod *method)
	{
		llvm::Value *left = argument (emitter, 0);
		llvm::Value *left8 = as_v16i8 (builder, left);
		llvm::Value *right8 = as_v16i8 (builder, argument (emitter, 1));
		uint8_t format = string_format_bits (method, left);
		llvm::Value *selector = aggregation_selector (builder, argument (emitter, 2), 0x7C);

		builder.CreateRet (dispatch_control (
			builder, selector, 32, builder.getInt32Ty (), [&] (unsigned mode_case) {
				uint8_t control = (uint8_t) ((mode_case << 2) | format);

				return builder.CreateIntrinsic (llvm::Intrinsic::x86_sse42_pcmpistri128, {},
				                                { left8, right8, builder.getInt8 (control) });
			}));
		return llvm::Error::success ();
	}

	static BuiltinResult compare_explicit_length_index (MethodLLVMEmitter &emitter,
	                                                     llvm::IRBuilder<> &builder,
	                                                     MonoMethod *method)
	{
		llvm::Value *left = argument (emitter, 0);
		llvm::Value *left8 = as_v16i8 (builder, left);
		llvm::Value *left_length = builder.CreateZExt (argument (emitter, 1), builder.getInt32Ty ());
		llvm::Value *right8 = as_v16i8 (builder, argument (emitter, 2));
		llvm::Value *right_length = builder.CreateZExt (argument (emitter, 3), builder.getInt32Ty ());
		uint8_t format = string_format_bits (method, left);
		llvm::Value *selector = aggregation_selector (builder, argument (emitter, 4), 0x7C);

		builder.CreateRet (dispatch_control (
			builder, selector, 32, builder.getInt32Ty (), [&] (unsigned mode_case) {
				uint8_t control = (uint8_t) ((mode_case << 2) | format);

				return builder.CreateIntrinsic (
					llvm::Intrinsic::x86_sse42_pcmpestri128, {},
					{ left8, left_length, right8, right_length, builder.getInt8 (control) });
			}));
		return llvm::Error::success ();
	}

	/// PCMPxSTRM uses control bit 6 to select bit or unit masks. Derive that bit
	/// from the managed method rather than the runtime comparison mode.
	template <bool unit>
	static BuiltinResult compare_implicit_length_mask (MethodLLVMEmitter &emitter,
	                                                    llvm::IRBuilder<> &builder,
	                                                    MonoMethod *method)
	{
		llvm::Value *left = argument (emitter, 0);
		llvm::Value *left8 = as_v16i8 (builder, left);
		llvm::Value *right8 = as_v16i8 (builder, argument (emitter, 1));
		uint8_t format = string_format_bits (method, left) | (unit ? 0x40 : 0x00);
		llvm::Value *selector = aggregation_selector (builder, argument (emitter, 2), 0x3C);
		llvm::Type *v16i8 = left8->getType ();
		llvm::Value *result = dispatch_control (
			builder, selector, 16, v16i8, [&] (unsigned mode_case) {
				uint8_t control = (uint8_t) ((mode_case << 2) | format);

				return builder.CreateIntrinsic (llvm::Intrinsic::x86_sse42_pcmpistrm128, {},
				                                { left8, right8, builder.getInt8 (control) });
			});

		builder.CreateRet (builder.CreateBitCast (result, return_type (emitter)));
		return llvm::Error::success ();
	}

	template <bool unit>
	static BuiltinResult compare_explicit_length_mask (MethodLLVMEmitter &emitter,
	                                                    llvm::IRBuilder<> &builder,
	                                                    MonoMethod *method)
	{
		llvm::Value *left = argument (emitter, 0);
		llvm::Value *left8 = as_v16i8 (builder, left);
		llvm::Value *left_length = builder.CreateZExt (argument (emitter, 1), builder.getInt32Ty ());
		llvm::Value *right8 = as_v16i8 (builder, argument (emitter, 2));
		llvm::Value *right_length = builder.CreateZExt (argument (emitter, 3), builder.getInt32Ty ());
		uint8_t format = string_format_bits (method, left) | (unit ? 0x40 : 0x00);
		llvm::Value *selector = aggregation_selector (builder, argument (emitter, 4), 0x3C);
		llvm::Type *v16i8 = left8->getType ();
		llvm::Value *result = dispatch_control (
			builder, selector, 16, v16i8, [&] (unsigned mode_case) {
				uint8_t control = (uint8_t) ((mode_case << 2) | format);

				return builder.CreateIntrinsic (
					llvm::Intrinsic::x86_sse42_pcmpestrm128, {},
					{ left8, left_length, right8, right_length, builder.getInt8 (control) });
			});

		builder.CreateRet (builder.CreateBitCast (result, return_type (emitter)));
		return llvm::Error::success ();
	}

	/// Loads one scalar and broadcasts it to every result lane.
	static BuiltinResult broadcast_scalar (MethodLLVMEmitter &emitter, llvm::IRBuilder<> &builder,
	                                       MonoMethod *)
	{
		llvm::Type *vector_type = return_type (emitter);
		llvm::Type *elem = llvm::cast<llvm::FixedVectorType> (vector_type)->getElementType ();
		unsigned lanes = llvm::cast<llvm::FixedVectorType> (vector_type)->getNumElements ();
		llvm::Value *scalar = builder.CreateLoad (elem, argument (emitter, 0));

		builder.CreateRet (builder.CreateVectorSplat (lanes, scalar));
		return llvm::Error::success ();
	}

	/// Loads one 128-bit vector and copies it to both result halves.
	static BuiltinResult broadcast_vector128 (MethodLLVMEmitter &emitter, llvm::IRBuilder<> &builder,
	                                          MonoMethod *)
	{
		llvm::Type *vector_type = return_type (emitter);
		llvm::Type *elem = llvm::cast<llvm::FixedVectorType> (vector_type)->getElementType ();
		unsigned lanes = llvm::cast<llvm::FixedVectorType> (vector_type)->getNumElements ();
		llvm::Type *half_type = llvm::FixedVectorType::get (elem, lanes / 2);
		llvm::Value *half = builder.CreateLoad (half_type, argument (emitter, 0));
		std::vector<int> mask (lanes);

		for (unsigned i = 0; i < lanes; i++)
			mask[i] = (int) (i % (lanes / 2));

		builder.CreateRet (builder.CreateShuffleVector (half, mask));
		return llvm::Error::success ();
	}

	/// Dispatches the runtime comparison mode to an intrinsic call with a
	/// constant immediate. The legacy intrinsic also represents the VEX-encoded
	/// 128-bit form.
	static BuiltinResult avx_compare (MethodLLVMEmitter &emitter, llvm::IRBuilder<> &builder,
	                                  MonoMethod *)
	{
		llvm::Value *left = argument (emitter, 0);
		llvm::Value *right = argument (emitter, 1);
		llvm::Value *mode = argument (emitter, 2);
		bool is_double = is_double_vector (left);
		llvm::Intrinsic::ID id = is_double
			? (is_256 (left) ? llvm::Intrinsic::x86_avx_cmp_pd_256 : llvm::Intrinsic::x86_sse2_cmp_pd)
			: (is_256 (left) ? llvm::Intrinsic::x86_avx_cmp_ps_256 : llvm::Intrinsic::x86_sse_cmp_ps);
		llvm::Value *result = dispatch_control (builder, mode, 32, left->getType (), [&] (unsigned p) {
			return builder.CreateIntrinsic (id, {}, { left, right, builder.getInt8 (p) });
		});

		builder.CreateRet (result);
		return llvm::Error::success ();
	}

	/// Dispatches a runtime scalar comparison mode to a constant immediate.
	static BuiltinResult avx_compare_scalar (MethodLLVMEmitter &emitter, llvm::IRBuilder<> &builder,
	                                         MonoMethod *)
	{
		llvm::Value *left = argument (emitter, 0);
		llvm::Value *right = argument (emitter, 1);
		llvm::Value *mode = argument (emitter, 2);
		llvm::Intrinsic::ID id = is_double_vector (left) ? llvm::Intrinsic::x86_sse2_cmp_sd
		                                                 : llvm::Intrinsic::x86_sse_cmp_ss;
		llvm::Value *result = dispatch_control (builder, mode, 32, left->getType (), [&] (unsigned p) {
			return builder.CreateIntrinsic (id, {}, { left, right, builder.getInt8 (p) });
		});

		builder.CreateRet (result);
		return llvm::Error::success ();
	}

	/// Emits the 256-bit AVX rounding intrinsic for imm8.
	template <int imm8>
	static BuiltinResult avx_round (MethodLLVMEmitter &emitter, llvm::IRBuilder<> &builder,
	                                MonoMethod *)
	{
		llvm::Value *value = argument (emitter, 0);

		builder.CreateRet (builder.CreateIntrinsic (
			is_double_vector (value) ? llvm::Intrinsic::x86_avx_round_pd_256
			                         : llvm::Intrinsic::x86_avx_round_ps_256,
			{}, { value, builder.getInt32 (imm8) }));
		return llvm::Error::success ();
	}

	static BuiltinResult avx_blend_variable (MethodLLVMEmitter &emitter, llvm::IRBuilder<> &builder,
	                                         MonoMethod *)
	{
		llvm::Value *left = argument (emitter, 0);
		llvm::Value *right = argument (emitter, 1);
		llvm::Value *mask = argument (emitter, 2);
		llvm::Intrinsic::ID id = is_double_vector (left) ? llvm::Intrinsic::x86_avx_blendv_pd_256
		                                                 : llvm::Intrinsic::x86_avx_blendv_ps_256;

		builder.CreateRet (builder.CreateIntrinsic (id, {}, { left, right, mask }));
		return llvm::Error::success ();
	}

	static BuiltinResult to_single (MethodLLVMEmitter &emitter, llvm::IRBuilder<> &builder,
	                                MonoMethod *)
	{
		builder.CreateRet (builder.CreateExtractElement (argument (emitter, 0), (uint64_t) 0));
		return llvm::Error::success ();
	}

	/// VCVTDQ2PS is equivalent to a signed integer-to-float conversion.
	static BuiltinResult int_to_float (MethodLLVMEmitter &emitter, llvm::IRBuilder<> &builder,
	                                   MonoMethod *)
	{
		builder.CreateRet (builder.CreateSIToFP (argument (emitter, 0), return_type (emitter)));
		return llvm::Error::success ();
	}

	/// Widens integer or float lanes to double using generic LLVM conversions.
	static BuiltinResult widen_to_double (MethodLLVMEmitter &emitter, llvm::IRBuilder<> &builder,
	                                      MonoMethod *)
	{
		llvm::Value *value = argument (emitter, 0);
		llvm::Type *dest = return_type (emitter);
		llvm::Type *elem =
			llvm::cast<llvm::FixedVectorType> (value->getType ())->getElementType ();

		builder.CreateRet (elem->isIntegerTy () ? builder.CreateSIToFP (value, dest)
		                                        : builder.CreateFPExt (value, dest));
		return llvm::Error::success ();
	}

	/// Implements VDPPS with a runtime control byte. Each 128-bit half computes
	/// its own four-element sum using the same control bits.
	static BuiltinResult avx_dot_product (MethodLLVMEmitter &emitter, llvm::IRBuilder<> &builder,
	                                      MonoMethod *)
	{
		llvm::Value *left = argument (emitter, 0);
		llvm::Value *right = argument (emitter, 1);
		llvm::Value *control = builder.CreateZExt (argument (emitter, 2), builder.getInt32Ty ());
		llvm::Type *vector_type = left->getType ();
		llvm::Type *elem = llvm::cast<llvm::FixedVectorType> (vector_type)->getElementType ();
		llvm::Value *zero = llvm::ConstantFP::get (elem, 0.0);
		llvm::Value *product = builder.CreateFMul (left, right);
		llvm::Value *result = llvm::Constant::getNullValue (vector_type);

		for (unsigned half = 0; half < 2; half++) {
			llvm::Value *sum = zero;

			for (unsigned j = 0; j < 4; j++) {
				unsigned i = half * 4 + j;
				llvm::Value *bit = builder.CreateICmpNE (
					builder.CreateAnd (control, builder.getInt32 (0x10u << j)),
					builder.getInt32 (0));

				sum = builder.CreateFAdd (
					sum,
					builder.CreateSelect (bit, builder.CreateExtractElement (product, i), zero));
			}

			for (unsigned j = 0; j < 4; j++) {
				unsigned i = half * 4 + j;
				llvm::Value *bit = builder.CreateICmpNE (
					builder.CreateAnd (control, builder.getInt32 (1u << j)), builder.getInt32 (0));

				result =
					builder.CreateInsertElement (result, builder.CreateSelect (bit, sum, zero), i);
			}
		}

		builder.CreateRet (result);
		return llvm::Error::success ();
	}

	static BuiltinResult extract_vector128 (MethodLLVMEmitter &emitter, llvm::IRBuilder<> &builder,
	                                        MonoMethod *)
	{
		builder.CreateRet (
			avx_low_or_high (builder, argument (emitter, 0), argument (emitter, 1)));
		return llvm::Error::success ();
	}

	static BuiltinResult extract_vector128_store (MethodLLVMEmitter &emitter,
	                                              llvm::IRBuilder<> &builder, MonoMethod *)
	{
		llvm::Value *address = argument (emitter, 0);
		llvm::Value *selected =
			avx_low_or_high (builder, argument (emitter, 1), argument (emitter, 2));

		builder.CreateAlignedStore (selected, address, llvm::Align (4));
		builder.CreateRetVoid ();
		return llvm::Error::success ();
	}

	/// Extends to 256 bits while leaving the upper half unspecified.
	static BuiltinResult extend_to_vector256 (MethodLLVMEmitter &emitter, llvm::IRBuilder<> &builder,
	                                          MonoMethod *)
	{
		llvm::Value *value = argument (emitter, 0);
		unsigned half = llvm::cast<llvm::FixedVectorType> (value->getType ())->getNumElements ();
		std::vector<int> mask (half * 2, -1);

		for (unsigned i = 0; i < half; i++)
			mask[i] = (int) i;

		builder.CreateRet (builder.CreateShuffleVector (value, mask));
		return llvm::Error::success ();
	}

	static BuiltinResult get_lower_half (MethodLLVMEmitter &emitter, llvm::IRBuilder<> &builder,
	                                     MonoMethod *)
	{
		llvm::Value *value = argument (emitter, 0);
		unsigned half = llvm::cast<llvm::FixedVectorType> (return_type (emitter))->getNumElements ();

		builder.CreateRet (builder.CreateShuffleVector (value, low_lanes_mask (half)));
		return llvm::Error::success ();
	}

	static BuiltinResult insert_vector128 (MethodLLVMEmitter &emitter, llvm::IRBuilder<> &builder,
	                                       MonoMethod *)
	{
		builder.CreateRet (avx_insert_half (builder, argument (emitter, 0), argument (emitter, 1),
		                                    argument (emitter, 2)));
		return llvm::Error::success ();
	}

	static BuiltinResult insert_vector128_load (MethodLLVMEmitter &emitter,
	                                            llvm::IRBuilder<> &builder, MonoMethod *)
	{
		llvm::Value *value = argument (emitter, 0);
		llvm::Value *address = argument (emitter, 1);
		llvm::Value *index = argument (emitter, 2);
		llvm::Type *value_type = value->getType ();
		llvm::Type *half_type = llvm::FixedVectorType::get (
			llvm::cast<llvm::FixedVectorType> (value_type)->getElementType (),
			llvm::cast<llvm::FixedVectorType> (value_type)->getNumElements () / 2);
		llvm::Value *data = builder.CreateAlignedLoad (half_type, address, llvm::Align (4));

		builder.CreateRet (avx_insert_half (builder, value, data, index));
		return llvm::Error::success ();
	}

	static BuiltinResult load_aligned256 (MethodLLVMEmitter &emitter, llvm::IRBuilder<> &builder,
	                                      MonoMethod *)
	{
		return load (emitter, builder, llvm::Align (32));
	}

	static BuiltinResult store_aligned256 (MethodLLVMEmitter &emitter, llvm::IRBuilder<> &builder,
	                                       MonoMethod *)
	{
		return store (emitter, builder, llvm::Align (32));
	}

	static llvm::Intrinsic::ID mask_load_id (bool wide, bool is_double)
	{
		return is_double ? (wide ? llvm::Intrinsic::x86_avx_maskload_pd_256
		                        : llvm::Intrinsic::x86_avx_maskload_pd)
		                 : (wide ? llvm::Intrinsic::x86_avx_maskload_ps_256
		                        : llvm::Intrinsic::x86_avx_maskload_ps);
	}

	static llvm::Intrinsic::ID mask_store_id (bool wide, bool is_double)
	{
		return is_double ? (wide ? llvm::Intrinsic::x86_avx_maskstore_pd_256
		                        : llvm::Intrinsic::x86_avx_maskstore_pd)
		                 : (wide ? llvm::Intrinsic::x86_avx_maskstore_ps_256
		                        : llvm::Intrinsic::x86_avx_maskstore_ps);
	}

	/// VMASKMOVPS/PD use the sign bit of each mask lane.
	static BuiltinResult mask_load (MethodLLVMEmitter &emitter, llvm::IRBuilder<> &builder,
	                                MonoMethod *)
	{
		llvm::Value *address = argument (emitter, 0);
		llvm::Value *mask = argument (emitter, 1);
		unsigned lanes = llvm::cast<llvm::FixedVectorType> (mask->getType ())->getNumElements ();
		bool is_double = is_double_vector (mask);
		llvm::Type *int_type = llvm::FixedVectorType::get (
			is_double ? builder.getInt64Ty () : builder.getInt32Ty (), lanes);

		builder.CreateRet (builder.CreateIntrinsic (
			mask_load_id (is_256 (mask), is_double), {},
			{ address, builder.CreateBitCast (mask, int_type) }));
		return llvm::Error::success ();
	}

	static BuiltinResult mask_store (MethodLLVMEmitter &emitter, llvm::IRBuilder<> &builder,
	                                 MonoMethod *)
	{
		llvm::Value *address = argument (emitter, 0);
		llvm::Value *mask = argument (emitter, 1);
		llvm::Value *source = argument (emitter, 2);
		unsigned lanes = llvm::cast<llvm::FixedVectorType> (mask->getType ())->getNumElements ();
		bool is_double = is_double_vector (mask);
		llvm::Type *int_type = llvm::FixedVectorType::get (
			is_double ? builder.getInt64Ty () : builder.getInt32Ty (), lanes);

		builder.CreateIntrinsic (mask_store_id (is_256 (mask), is_double), {},
		                         { address, builder.CreateBitCast (mask, int_type), source });
		builder.CreateRetVoid ();
		return llvm::Error::success ();
	}

	static BuiltinResult avx_movmsk (MethodLLVMEmitter &emitter, llvm::IRBuilder<> &builder,
	                                 MonoMethod *)
	{
		llvm::Value *value = argument (emitter, 0);
		llvm::Intrinsic::ID id = is_double_vector (value) ? llvm::Intrinsic::x86_avx_movmsk_pd_256
		                                                  : llvm::Intrinsic::x86_avx_movmsk_ps_256;

		builder.CreateRet (builder.CreateIntrinsic (id, {}, { value }));
		return llvm::Error::success ();
	}

	/// Permutes within each 128-bit half. Float selectors repeat for the upper
	/// half; double selectors occupy one bit per destination lane.
	static BuiltinResult permute (MethodLLVMEmitter &emitter, llvm::IRBuilder<> &builder,
	                              MonoMethod *)
	{
		llvm::Value *value = argument (emitter, 0);
		llvm::Value *control = builder.CreateZExt (argument (emitter, 1), builder.getInt32Ty ());
		unsigned lanes = llvm::cast<llvm::FixedVectorType> (value->getType ())->getNumElements ();
		unsigned half = half_lanes (value);
		bool is_double = is_double_vector (value);
		llvm::Value *result = llvm::Constant::getNullValue (value->getType ());

		for (unsigned i = 0; i < lanes; i++) {
			unsigned h = i / half;
			unsigned j = i % half;
			llvm::Value *select = is_double
				? builder.CreateAnd (builder.CreateLShr (control, i), 1)
				: builder.CreateAnd (builder.CreateLShr (control, 2 * j), 3);
			llvm::Value *source_lane =
				builder.CreateAdd (builder.getInt32 (h * half), select);

			result = builder.CreateInsertElement (
				result, builder.CreateExtractElement (value, source_lane), i);
		}

		builder.CreateRet (result);
		return llvm::Error::success ();
	}

	/// Shuffles within each 128-bit half, selecting its lower lanes from left
	/// and upper lanes from right.
	static BuiltinResult avx_shuffle (MethodLLVMEmitter &emitter, llvm::IRBuilder<> &builder,
	                                  MonoMethod *)
	{
		llvm::Value *left = argument (emitter, 0);
		llvm::Value *right = argument (emitter, 1);
		llvm::Value *control = builder.CreateZExt (argument (emitter, 2), builder.getInt32Ty ());
		llvm::Type *vector_type = left->getType ();
		unsigned lanes = llvm::cast<llvm::FixedVectorType> (vector_type)->getNumElements ();
		unsigned half = half_lanes (left);
		bool is_double = is_double_vector (left);
		llvm::Value *result = llvm::Constant::getNullValue (vector_type);

		for (unsigned i = 0; i < lanes; i++) {
			unsigned h = i / half;
			unsigned j = i % half;
			llvm::Value *operand = j >= half / 2 ? right : left;
			llvm::Value *select = is_double
				? builder.CreateAnd (builder.CreateLShr (control, i), 1)
				: builder.CreateAnd (builder.CreateLShr (control, 2 * j), 3);
			llvm::Value *source_lane =
				builder.CreateAdd (builder.getInt32 (h * half), select);

			result = builder.CreateInsertElement (
				result, builder.CreateExtractElement (operand, source_lane), i);
		}

		builder.CreateRet (result);
		return llvm::Error::success ();
	}

	/// Implements VPERM2F128 control semantics: bits 1:0 and 5:4 select each
	/// destination half, while bits 3 and 7 zero their respective halves.
	static BuiltinResult permute2x128 (MethodLLVMEmitter &emitter, llvm::IRBuilder<> &builder,
	                                   MonoMethod *)
	{
		llvm::Value *left = argument (emitter, 0);
		llvm::Value *right = argument (emitter, 1);
		llvm::Value *control = builder.CreateZExt (argument (emitter, 2), builder.getInt32Ty ());
		llvm::Type *vector_type = left->getType ();
		unsigned lanes = llvm::cast<llvm::FixedVectorType> (vector_type)->getNumElements ();
		unsigned half = lanes / 2;
		llvm::Value *left_low = builder.CreateShuffleVector (left, low_lanes_mask (half));
		llvm::Value *right_low = builder.CreateShuffleVector (right, low_lanes_mask (half));
		std::vector<int> high_mask (half);

		for (unsigned i = 0; i < half; i++)
			high_mask[i] = (int) (half + i);

		llvm::Value *left_high = builder.CreateShuffleVector (left, high_mask);
		llvm::Value *right_high = builder.CreateShuffleVector (right, high_mask);
		llvm::Value *zero = llvm::Constant::getNullValue (left_low->getType ());
		llvm::Value *result = llvm::Constant::getNullValue (vector_type);

		for (unsigned half_index = 0; half_index < 2; half_index++) {
			llvm::Value *field =
				builder.CreateAnd (builder.CreateLShr (control, half_index * 4), 3);
			llvm::Value *from_left = builder.CreateICmpULT (field, builder.getInt32 (2));
			llvm::Value *is_high =
				builder.CreateICmpNE (builder.CreateAnd (field, 1), builder.getInt32 (0));
			llvm::Value *left_choice = builder.CreateSelect (is_high, left_high, left_low);
			llvm::Value *right_choice = builder.CreateSelect (is_high, right_high, right_low);
			llvm::Value *selected = builder.CreateSelect (from_left, left_choice, right_choice);
			llvm::Value *zero_bit = builder.CreateICmpNE (
				builder.CreateAnd (control, builder.getInt32 (0x8u << (half_index * 4))),
				builder.getInt32 (0));
			llvm::Value *half_result = builder.CreateSelect (zero_bit, zero, selected);

			for (unsigned i = 0; i < half; i++)
				result = builder.CreateInsertElement (
					result, builder.CreateExtractElement (half_result, i), half_index * half + i);
		}

		builder.CreateRet (result);
		return llvm::Error::success ();
	}

	static BuiltinResult permute_var (MethodLLVMEmitter &emitter, llvm::IRBuilder<> &builder,
	                                  MonoMethod *)
	{
		llvm::Value *left = argument (emitter, 0);
		llvm::Value *control = argument (emitter, 1);
		bool is_double = is_double_vector (left);
		llvm::Intrinsic::ID id = is_double
			? (is_256 (left) ? llvm::Intrinsic::x86_avx_vpermilvar_pd_256
			                : llvm::Intrinsic::x86_avx_vpermilvar_pd)
			: (is_256 (left) ? llvm::Intrinsic::x86_avx_vpermilvar_ps_256
			                : llvm::Intrinsic::x86_avx_vpermilvar_ps);

		builder.CreateRet (builder.CreateIntrinsic (id, {}, { left, control }));
		return llvm::Error::success ();
	}

	/// The SetVector256 overloads place their last argument in lane zero.
	static BuiltinResult set_vector256 (MethodLLVMEmitter &emitter, llvm::IRBuilder<> &builder,
	                                    MonoMethod *)
	{
		llvm::Type *vector_type = return_type (emitter);
		unsigned lanes = llvm::cast<llvm::FixedVectorType> (vector_type)->getNumElements ();
		llvm::Value *result = llvm::Constant::getNullValue (vector_type);

		for (unsigned i = 0; i < lanes; i++)
			result = builder.CreateInsertElement (result, argument (emitter, i), lanes - 1 - i);

		builder.CreateRet (result);
		return llvm::Error::success ();
	}

	static BuiltinResult set_all_vector256 (MethodLLVMEmitter &emitter, llvm::IRBuilder<> &builder,
	                                        MonoMethod *)
	{
		unsigned lanes = llvm::cast<llvm::FixedVectorType> (return_type (emitter))->getNumElements ();

		builder.CreateRet (builder.CreateVectorSplat (lanes, argument (emitter, 0)));
		return llvm::Error::success ();
	}

	static BuiltinResult set_high_low (MethodLLVMEmitter &emitter, llvm::IRBuilder<> &builder,
	                                   MonoMethod *)
	{
		llvm::Value *hi = argument (emitter, 0);
		llvm::Value *lo = argument (emitter, 1);
		unsigned half = llvm::cast<llvm::FixedVectorType> (hi->getType ())->getNumElements ();

		builder.CreateRet (builder.CreateShuffleVector (lo, hi, low_lanes_mask (half * 2)));
		return llvm::Error::success ();
	}

	static BuiltinResult static_cast_vector (MethodLLVMEmitter &emitter, llvm::IRBuilder<> &builder,
	                                         MonoMethod *)
	{
		builder.CreateRet (builder.CreateBitCast (argument (emitter, 0), return_type (emitter)));
		return llvm::Error::success ();
	}

	static BuiltinResult duplicate_even_indexed (MethodLLVMEmitter &emitter,
	                                             llvm::IRBuilder<> &builder, MonoMethod *)
	{
		llvm::Value *value = argument (emitter, 0);
		unsigned lanes = llvm::cast<llvm::FixedVectorType> (value->getType ())->getNumElements ();
		std::vector<int> mask (lanes);

		for (unsigned i = 0; i < lanes; i++)
			mask[i] = (int) (i & ~1u);

		builder.CreateRet (builder.CreateShuffleVector (value, mask));
		return llvm::Error::success ();
	}

	static BuiltinResult duplicate_odd_indexed (MethodLLVMEmitter &emitter,
	                                            llvm::IRBuilder<> &builder, MonoMethod *)
	{
		llvm::Value *value = argument (emitter, 0);
		unsigned lanes = llvm::cast<llvm::FixedVectorType> (value->getType ())->getNumElements ();
		std::vector<int> mask (lanes);

		for (unsigned i = 0; i < lanes; i++)
			mask[i] = (int) (i | 1u);

		builder.CreateRet (builder.CreateShuffleVector (value, mask));
		return llvm::Error::success ();
	}

	/// Interleaves each 128-bit half independently.
	template <bool high>
	static BuiltinResult unpack (MethodLLVMEmitter &emitter, llvm::IRBuilder<> &builder, MonoMethod *)
	{
		llvm::Value *left = argument (emitter, 0);
		llvm::Value *right = argument (emitter, 1);
		llvm::Type *vector_type = left->getType ();
		unsigned lanes = llvm::cast<llvm::FixedVectorType> (vector_type)->getNumElements ();
		unsigned half = half_lanes (left);
		unsigned quarter = half / 2;
		std::vector<int> mask (lanes);

		for (unsigned i = 0; i < lanes; i++) {
			unsigned h = i / half;
			unsigned j = i % half;
			unsigned base = h * half + (high ? quarter : 0) + j / 2;

			mask[i] = j % 2 == 0 ? (int) base : (int) (lanes + base);
		}

		builder.CreateRet (builder.CreateShuffleVector (left, right, mask));
		return llvm::Error::success ();
	}

	/// Uses v4i64 for the integer PTEST form and native vector types for VTEST.
	template <llvm::Intrinsic::ID ps128, llvm::Intrinsic::ID pd128, llvm::Intrinsic::ID ps256,
	         llvm::Intrinsic::ID pd256, llvm::Intrinsic::ID i256>
	static BuiltinResult avx_test (MethodLLVMEmitter &emitter, llvm::IRBuilder<> &builder,
	                               MonoMethod *)
	{
		llvm::Value *left = argument (emitter, 0);
		llvm::Value *right = argument (emitter, 1);
		llvm::Type *elem = llvm::cast<llvm::FixedVectorType> (left->getType ())->getElementType ();
		llvm::Value *result;

		if (elem->isDoubleTy ())
			result = builder.CreateIntrinsic (is_256 (left) ? pd256 : pd128, {}, { left, right });
		else if (elem->isFloatTy ())
			result = builder.CreateIntrinsic (is_256 (left) ? ps256 : ps128, {}, { left, right });
		else {
			llvm::Type *i64x4 =
				llvm::FixedVectorType::get (builder.getInt64Ty (), 4);

			result = builder.CreateIntrinsic (
				i256, {}, { builder.CreateBitCast (left, i64x4), builder.CreateBitCast (right, i64x4) });
		}

		builder.CreateRet (builder.CreateTrunc (result, return_type (emitter)));
		return llvm::Error::success ();
	}

	template <bool add>
	static BuiltinResult saturating (MethodLLVMEmitter &emitter, llvm::IRBuilder<> &builder,
	                                 MonoMethod *method)
	{
		llvm::Value *lhs = argument (emitter, 0);
		llvm::Value *rhs = argument (emitter, 1);
		bool is_unsigned = param_is_unsigned (method, 0);
		llvm::Intrinsic::ID id = is_unsigned
			? (add ? llvm::Intrinsic::uadd_sat : llvm::Intrinsic::usub_sat)
			: (add ? llvm::Intrinsic::sadd_sat : llvm::Intrinsic::ssub_sat);

		builder.CreateRet (builder.CreateIntrinsic (id, { lhs->getType () }, { lhs, rhs }));
		return llvm::Error::success ();
	}

	/// PALIGNR operates on each 128-bit half independently.
	static BuiltinResult avx2_align_right (MethodLLVMEmitter &emitter, llvm::IRBuilder<> &builder,
	                                       MonoMethod *)
	{
		llvm::Value *left = argument (emitter, 0);
		llvm::Value *right = argument (emitter, 1);
		llvm::Value *mask = argument (emitter, 2);
		llvm::LLVMContext &ctx = context (emitter);
		llvm::Type *i128 = llvm::Type::getIntNTy (ctx, 128);
		llvm::Type *i256 = llvm::Type::getIntNTy (ctx, 256);
		llvm::Type *lane_type = llvm::FixedVectorType::get (builder.getInt8Ty (), 16);
		unsigned lanes = llvm::cast<llvm::FixedVectorType> (left->getType ())->getNumElements ();
		llvm::Value *shift =
			builder.CreateShl (builder.CreateZExt (mask, i256), llvm::ConstantInt::get (i256, 3));
		llvm::Value *result = llvm::Constant::getNullValue (left->getType ());

		for (unsigned h = 0; h < lanes / 16; h++) {
			std::vector<int> lane_mask (16);

			for (unsigned i = 0; i < 16; i++)
				lane_mask[i] = (int) (h * 16 + i);

			llvm::Value *left_lane = builder.CreateShuffleVector (left, lane_mask);
			llvm::Value *right_lane = builder.CreateShuffleVector (right, lane_mask);
			llvm::Value *hi = builder.CreateShl (
				builder.CreateZExt (builder.CreateBitCast (left_lane, i128), i256),
				llvm::ConstantInt::get (i256, 128));
			llvm::Value *lo = builder.CreateZExt (builder.CreateBitCast (right_lane, i128), i256);
			llvm::Value *concat = builder.CreateOr (hi, lo);
			llvm::Value *shifted = builder.CreateBitCast (
				builder.CreateTrunc (builder.CreateLShr (concat, shift), i128), lane_type);
			llvm::Value *lane_result = builder.CreateSelect (
				builder.CreateICmpUGE (mask, builder.getInt8 (32)),
				llvm::Constant::getNullValue (lane_type), shifted);

			for (unsigned i = 0; i < 16; i++)
				result = builder.CreateInsertElement (
					result, builder.CreateExtractElement (lane_result, i), h * 16 + i);
		}

		builder.CreateRet (result);
		return llvm::Error::success ();
	}

	/// Unlike the AVX forms, these broadcasts take a vector rather than a pointer.
	static BuiltinResult avx2_broadcast_scalar_reg (MethodLLVMEmitter &emitter,
	                                                llvm::IRBuilder<> &builder, MonoMethod *)
	{
		llvm::Value *value = argument (emitter, 0);
		unsigned lanes = llvm::cast<llvm::FixedVectorType> (return_type (emitter))->getNumElements ();
		llvm::Value *scalar = builder.CreateExtractElement (value, (uint64_t) 0);

		builder.CreateRet (builder.CreateVectorSplat (lanes, scalar));
		return llvm::Error::success ();
	}

	static BuiltinResult avx2_average (MethodLLVMEmitter &emitter, llvm::IRBuilder<> &builder,
	                                   MonoMethod *)
	{
		llvm::Value *lhs = argument (emitter, 0);
		llvm::Value *rhs = argument (emitter, 1);
		unsigned bits =
			llvm::cast<llvm::FixedVectorType> (lhs->getType ())->getElementType ()->getIntegerBitWidth ();
		llvm::Intrinsic::ID id =
			bits == 8 ? llvm::Intrinsic::x86_avx2_pavg_b : llvm::Intrinsic::x86_avx2_pavg_w;

		builder.CreateRet (builder.CreateIntrinsic (id, {}, { lhs, rhs }));
		return llvm::Error::success ();
	}

	/// The eight control bits repeat for the upper half of a 16-bit vector.
	static BuiltinResult avx2_blend (MethodLLVMEmitter &emitter, llvm::IRBuilder<> &builder,
	                                 MonoMethod *)
	{
		llvm::Value *left = argument (emitter, 0);
		llvm::Value *right = argument (emitter, 1);
		llvm::Value *control = argument (emitter, 2);
		unsigned lanes = llvm::cast<llvm::FixedVectorType> (left->getType ())->getNumElements ();
		llvm::Value *result = left;

		for (unsigned i = 0; i < lanes; i++) {
			llvm::Value *bit = builder.CreateICmpNE (
				builder.CreateAnd (control, builder.getInt8 (1u << (i % 8))), builder.getInt8 (0));

			result = builder.CreateInsertElement (
				result,
				builder.CreateSelect (bit, builder.CreateExtractElement (right, i),
				                      builder.CreateExtractElement (left, i)),
				i);
		}

		builder.CreateRet (result);
		return llvm::Error::success ();
	}

	static BuiltinResult avx2_blend_variable (MethodLLVMEmitter &emitter, llvm::IRBuilder<> &builder,
	                                          MonoMethod *)
	{
		builder.CreateRet (builder.CreateIntrinsic (
			llvm::Intrinsic::x86_avx2_pblendvb, {},
			{ argument (emitter, 0), argument (emitter, 1), argument (emitter, 2) }));
		return llvm::Error::success ();
	}

	/// Gather intrinsics are specialized by element type and index vector shape.
	static llvm::Intrinsic::ID gather_intrinsic (llvm::Value *index, llvm::Type *elem)
	{
		bool index64 =
			llvm::cast<llvm::FixedVectorType> (index->getType ())->getElementType ()->getIntegerBitWidth ()
			== 64;
		bool wide = is_256 (index);

		using ID = llvm::Intrinsic::ID;
		ID q_256, q, d_256, d;

		if (elem->isDoubleTy ()) {
			q_256 = llvm::Intrinsic::x86_avx2_gather_q_pd_256;
			q = llvm::Intrinsic::x86_avx2_gather_q_pd;
			d_256 = llvm::Intrinsic::x86_avx2_gather_d_pd_256;
			d = llvm::Intrinsic::x86_avx2_gather_d_pd;
		} else if (elem->isFloatTy ()) {
			q_256 = llvm::Intrinsic::x86_avx2_gather_q_ps_256;
			q = llvm::Intrinsic::x86_avx2_gather_q_ps;
			d_256 = llvm::Intrinsic::x86_avx2_gather_d_ps_256;
			d = llvm::Intrinsic::x86_avx2_gather_d_ps;
		} else if (elem->getIntegerBitWidth () == 64) {
			q_256 = llvm::Intrinsic::x86_avx2_gather_q_q_256;
			q = llvm::Intrinsic::x86_avx2_gather_q_q;
			d_256 = llvm::Intrinsic::x86_avx2_gather_d_q_256;
			d = llvm::Intrinsic::x86_avx2_gather_d_q;
		} else {
			q_256 = llvm::Intrinsic::x86_avx2_gather_q_d_256;
			q = llvm::Intrinsic::x86_avx2_gather_q_d;
			d_256 = llvm::Intrinsic::x86_avx2_gather_d_d_256;
			d = llvm::Intrinsic::x86_avx2_gather_d_d;
		}

		return index64 ? (wide ? q_256 : q) : (wide ? d_256 : d);
	}

	/// Model an unmasked gather with an all-ones mask and a zero source.
	template <bool masked>
	static BuiltinResult avx2_gather (MethodLLVMEmitter &emitter, llvm::IRBuilder<> &builder,
	                                  MonoMethod *)
	{
		unsigned i = 0;
		llvm::Type *ret_type = return_type (emitter);
		llvm::Value *source = masked ? argument (emitter, i++) : llvm::Constant::getNullValue (ret_type);
		llvm::Value *address = argument (emitter, i++);
		llvm::Value *index = argument (emitter, i++);
		llvm::Value *mask;

		if (masked) {
			mask = argument (emitter, i++);
		} else {
			unsigned lanes = llvm::cast<llvm::FixedVectorType> (ret_type)->getNumElements ();
			llvm::Type *int_equiv =
				llvm::FixedVectorType::get (builder.getIntNTy (ret_type->getScalarSizeInBits ()), lanes);

			mask = builder.CreateBitCast (llvm::Constant::getAllOnesValue (int_equiv), ret_type);
		}

		llvm::Value *scale = argument (emitter, i++);
		llvm::Intrinsic::ID id =
			gather_intrinsic (index, llvm::cast<llvm::FixedVectorType> (ret_type)->getElementType ());
		llvm::Value *result = dispatch_scale (builder, scale, ret_type, [&] (uint8_t s) {
			return builder.CreateIntrinsic (id, {}, { source, address, index, mask, builder.getInt8 (s) });
		});

		builder.CreateRet (result);
		return llvm::Error::success ();
	}

	static llvm::Intrinsic::ID avx2_maskload_id (bool wide64, bool wide256)
	{
		if (wide64)
			return wide256 ? llvm::Intrinsic::x86_avx2_maskload_q_256 : llvm::Intrinsic::x86_avx2_maskload_q;
		return wide256 ? llvm::Intrinsic::x86_avx2_maskload_d_256 : llvm::Intrinsic::x86_avx2_maskload_d;
	}

	static llvm::Intrinsic::ID avx2_maskstore_id (bool wide64, bool wide256)
	{
		if (wide64)
			return wide256 ? llvm::Intrinsic::x86_avx2_maskstore_q_256
			              : llvm::Intrinsic::x86_avx2_maskstore_q;
		return wide256 ? llvm::Intrinsic::x86_avx2_maskstore_d_256
		              : llvm::Intrinsic::x86_avx2_maskstore_d;
	}

	static BuiltinResult avx2_mask_load (MethodLLVMEmitter &emitter, llvm::IRBuilder<> &builder,
	                                     MonoMethod *)
	{
		llvm::Value *address = argument (emitter, 0);
		llvm::Value *mask = argument (emitter, 1);
		bool wide64 =
			llvm::cast<llvm::FixedVectorType> (mask->getType ())->getElementType ()->getIntegerBitWidth ()
			== 64;

		builder.CreateRet (
			builder.CreateIntrinsic (avx2_maskload_id (wide64, is_256 (mask)), {}, { address, mask }));
		return llvm::Error::success ();
	}

	static BuiltinResult avx2_mask_store (MethodLLVMEmitter &emitter, llvm::IRBuilder<> &builder,
	                                      MonoMethod *)
	{
		llvm::Value *address = argument (emitter, 0);
		llvm::Value *mask = argument (emitter, 1);
		llvm::Value *source = argument (emitter, 2);
		bool wide64 =
			llvm::cast<llvm::FixedVectorType> (mask->getType ())->getElementType ()->getIntegerBitWidth ()
			== 64;

		builder.CreateIntrinsic (avx2_maskstore_id (wide64, is_256 (mask)), {},
		                         { address, mask, source });
		builder.CreateRetVoid ();
		return llvm::Error::success ();
	}

	/// LLVM has no PMULDQ/PMULUDQ intrinsic, so widen and multiply the even lanes.
	static BuiltinResult avx2_multiply_widen (MethodLLVMEmitter &emitter, llvm::IRBuilder<> &builder,
	                                          MonoMethod *method)
	{
		llvm::Value *lhs = argument (emitter, 0);
		llvm::Value *rhs = argument (emitter, 1);
		llvm::Type *wide = return_type (emitter);
		unsigned lanes = llvm::cast<llvm::FixedVectorType> (wide)->getNumElements ();
		std::vector<int> even_mask (lanes);

		for (unsigned i = 0; i < lanes; i++)
			even_mask[i] = (int) (2 * i);

		llvm::Value *lhs_even = builder.CreateShuffleVector (lhs, even_mask);
		llvm::Value *rhs_even = builder.CreateShuffleVector (rhs, even_mask);
		bool is_unsigned = param_is_unsigned (method, 0);
		llvm::Value *lhs_wide =
			is_unsigned ? builder.CreateZExt (lhs_even, wide) : builder.CreateSExt (lhs_even, wide);
		llvm::Value *rhs_wide =
			is_unsigned ? builder.CreateZExt (rhs_even, wide) : builder.CreateSExt (rhs_even, wide);

		builder.CreateRet (builder.CreateMul (lhs_wide, rhs_wide));
		return llvm::Error::success ();
	}

	static BuiltinResult avx2_multiply_high (MethodLLVMEmitter &emitter, llvm::IRBuilder<> &builder,
	                                         MonoMethod *method)
	{
		llvm::Value *lhs = argument (emitter, 0);
		llvm::Value *rhs = argument (emitter, 1);
		llvm::Intrinsic::ID id = param_is_unsigned (method, 0) ? llvm::Intrinsic::x86_avx2_pmulhu_w
		                                                       : llvm::Intrinsic::x86_avx2_pmulh_w;

		builder.CreateRet (builder.CreateIntrinsic (id, {}, { lhs, rhs }));
		return llvm::Error::success ();
	}

	static BuiltinResult avx2_multiply_add_adjacent (MethodLLVMEmitter &emitter,
	                                                 llvm::IRBuilder<> &builder, MonoMethod *)
	{
		llvm::Value *lhs = argument (emitter, 0);
		llvm::Value *rhs = argument (emitter, 1);
		unsigned bits =
			llvm::cast<llvm::FixedVectorType> (lhs->getType ())->getElementType ()->getIntegerBitWidth ();
		llvm::Intrinsic::ID id = bits == 8 ? llvm::Intrinsic::x86_avx2_pmadd_ub_sw
		                                   : llvm::Intrinsic::x86_avx2_pmadd_wd;

		builder.CreateRet (builder.CreateIntrinsic (id, {}, { lhs, rhs }));
		return llvm::Error::success ();
	}

	static BuiltinResult avx2_pack_signed_saturate (MethodLLVMEmitter &emitter,
	                                                llvm::IRBuilder<> &builder, MonoMethod *)
	{
		llvm::Value *left = argument (emitter, 0);
		llvm::Value *right = argument (emitter, 1);
		unsigned bits =
			llvm::cast<llvm::FixedVectorType> (left->getType ())->getElementType ()->getIntegerBitWidth ();
		llvm::Intrinsic::ID id = bits == 16 ? llvm::Intrinsic::x86_avx2_packsswb
		                                    : llvm::Intrinsic::x86_avx2_packssdw;

		builder.CreateRet (builder.CreateIntrinsic (id, {}, { left, right }));
		return llvm::Error::success ();
	}

	static BuiltinResult avx2_pack_unsigned_saturate (MethodLLVMEmitter &emitter,
	                                                  llvm::IRBuilder<> &builder, MonoMethod *)
	{
		llvm::Value *left = argument (emitter, 0);
		llvm::Value *right = argument (emitter, 1);
		unsigned bits =
			llvm::cast<llvm::FixedVectorType> (left->getType ())->getElementType ()->getIntegerBitWidth ();
		llvm::Intrinsic::ID id = bits == 16 ? llvm::Intrinsic::x86_avx2_packuswb
		                                    : llvm::Intrinsic::x86_avx2_packusdw;

		builder.CreateRet (builder.CreateIntrinsic (id, {}, { left, right }));
		return llvm::Error::success ();
	}

	/// VPERMQ/VPERMPD select from the full register, not within 128-bit lanes.
	static BuiltinResult permute4x64 (MethodLLVMEmitter &emitter, llvm::IRBuilder<> &builder,
	                                  MonoMethod *)
	{
		llvm::Value *value = argument (emitter, 0);
		llvm::Value *control = builder.CreateZExt (argument (emitter, 1), builder.getInt32Ty ());
		llvm::Value *result = llvm::Constant::getNullValue (value->getType ());

		for (unsigned i = 0; i < 4; i++) {
			llvm::Value *select = builder.CreateAnd (builder.CreateLShr (control, 2 * i), 3);

			result =
				builder.CreateInsertElement (result, builder.CreateExtractElement (value, select), i);
		}

		builder.CreateRet (result);
		return llvm::Error::success ();
	}

	static BuiltinResult avx2_permute_var8x32 (MethodLLVMEmitter &emitter, llvm::IRBuilder<> &builder,
	                                           MonoMethod *)
	{
		llvm::Value *left = argument (emitter, 0);
		llvm::Value *control = argument (emitter, 1);
		llvm::Intrinsic::ID id = is_float (left) ? llvm::Intrinsic::x86_avx2_permps
		                                         : llvm::Intrinsic::x86_avx2_permd;

		builder.CreateRet (builder.CreateIntrinsic (id, {}, { left, control }));
		return llvm::Error::success ();
	}

	template <llvm::Intrinsic::ID w_id, llvm::Intrinsic::ID d_id, llvm::Intrinsic::ID q_id>
	static BuiltinResult avx2_shift_count (MethodLLVMEmitter &emitter, llvm::IRBuilder<> &builder,
	                                       MonoMethod *)
	{
		llvm::Value *value = argument (emitter, 0);
		llvm::Value *count = argument (emitter, 1);
		unsigned bits =
			llvm::cast<llvm::FixedVectorType> (value->getType ())->getElementType ()->getIntegerBitWidth ();
		llvm::Intrinsic::ID id = bits == 16 ? w_id : (bits == 32 ? d_id : q_id);

		builder.CreateRet (builder.CreateIntrinsic (id, {}, { value, count }));
		return llvm::Error::success ();
	}

	template <llvm::Intrinsic::ID w_id, llvm::Intrinsic::ID d_id>
	static BuiltinResult avx2_shift_count_arith (MethodLLVMEmitter &emitter,
	                                             llvm::IRBuilder<> &builder, MonoMethod *)
	{
		llvm::Value *value = argument (emitter, 0);
		llvm::Value *count = argument (emitter, 1);
		unsigned bits =
			llvm::cast<llvm::FixedVectorType> (value->getType ())->getElementType ()->getIntegerBitWidth ();

		builder.CreateRet (
			builder.CreateIntrinsic (bits == 16 ? w_id : d_id, {}, { value, count }));
		return llvm::Error::success ();
	}

	/// LLVM models the count as i32 rather than requiring a constant.
	template <llvm::Intrinsic::ID w_id, llvm::Intrinsic::ID d_id, llvm::Intrinsic::ID q_id>
	static BuiltinResult avx2_shift_immediate (MethodLLVMEmitter &emitter, llvm::IRBuilder<> &builder,
	                                           MonoMethod *)
	{
		llvm::Value *value = argument (emitter, 0);
		llvm::Value *count = builder.CreateZExt (argument (emitter, 1), builder.getInt32Ty ());
		unsigned bits =
			llvm::cast<llvm::FixedVectorType> (value->getType ())->getElementType ()->getIntegerBitWidth ();
		llvm::Intrinsic::ID id = bits == 16 ? w_id : (bits == 32 ? d_id : q_id);

		builder.CreateRet (builder.CreateIntrinsic (id, {}, { value, count }));
		return llvm::Error::success ();
	}

	template <llvm::Intrinsic::ID w_id, llvm::Intrinsic::ID d_id>
	static BuiltinResult avx2_shift_immediate_arith (MethodLLVMEmitter &emitter,
	                                                 llvm::IRBuilder<> &builder, MonoMethod *)
	{
		llvm::Value *value = argument (emitter, 0);
		llvm::Value *count = builder.CreateZExt (argument (emitter, 1), builder.getInt32Ty ());
		unsigned bits =
			llvm::cast<llvm::FixedVectorType> (value->getType ())->getElementType ()->getIntegerBitWidth ();

		builder.CreateRet (
			builder.CreateIntrinsic (bits == 16 ? w_id : d_id, {}, { value, count }));
		return llvm::Error::success ();
	}

	/// Shift one 128-bit lane by a runtime byte count.
	static llvm::Value *lane_byte_shift (llvm::IRBuilder<> &builder, llvm::Value *lane,
	                                     llvm::Value *count, bool left)
	{
		llvm::Type *i128 = llvm::Type::getIntNTy (builder.getContext (), 128);
		llvm::Value *wide = builder.CreateBitCast (lane, i128);
		llvm::Value *shift = builder.CreateShl (builder.CreateZExt (count, i128),
		                                        llvm::ConstantInt::get (i128, 3));
		llvm::Value *shifted = left ? builder.CreateShl (wide, shift) : builder.CreateLShr (wide, shift);
		llvm::Value *result = builder.CreateBitCast (shifted, lane->getType ());

		return builder.CreateSelect (builder.CreateICmpUGE (count, builder.getInt8 (16)),
		                             llvm::Constant::getNullValue (lane->getType ()), result);
	}

	/// PSLLDQ/PSRLDQ operate on each 128-bit half independently.
	template <bool left>
	static BuiltinResult shift_128_bit_lane (MethodLLVMEmitter &emitter, llvm::IRBuilder<> &builder,
	                                         MonoMethod *)
	{
		llvm::Value *value = argument (emitter, 0);
		llvm::Value *count = argument (emitter, 1);
		llvm::Type *original = value->getType ();
		llvm::Type *byte_vec = llvm::FixedVectorType::get (
			builder.getInt8Ty (),
			llvm::cast<llvm::FixedVectorType> (original)->getPrimitiveSizeInBits ().getFixedValue () / 8);
		llvm::Value *bytes = builder.CreateBitCast (value, byte_vec);
		unsigned total_bytes = llvm::cast<llvm::FixedVectorType> (byte_vec)->getNumElements ();
		llvm::Value *result = llvm::Constant::getNullValue (byte_vec);

		for (unsigned h = 0; h < total_bytes / 16; h++) {
			std::vector<int> lane_mask (16);

			for (unsigned i = 0; i < 16; i++)
				lane_mask[i] = (int) (h * 16 + i);

			llvm::Value *lane = builder.CreateShuffleVector (bytes, lane_mask);
			llvm::Value *shifted = lane_byte_shift (builder, lane, count, left);

			for (unsigned i = 0; i < 16; i++)
				result = builder.CreateInsertElement (
					result, builder.CreateExtractElement (shifted, i), h * 16 + i);
		}

		builder.CreateRet (builder.CreateBitCast (result, original));
		return llvm::Error::success ();
	}

	template <llvm::Intrinsic::ID d128, llvm::Intrinsic::ID d256, llvm::Intrinsic::ID q128,
	         llvm::Intrinsic::ID q256>
	static BuiltinResult avx2_shift_variable (MethodLLVMEmitter &emitter, llvm::IRBuilder<> &builder,
	                                          MonoMethod *)
	{
		llvm::Value *value = argument (emitter, 0);
		llvm::Value *count = argument (emitter, 1);
		bool wide64 =
			llvm::cast<llvm::FixedVectorType> (value->getType ())->getElementType ()->getIntegerBitWidth ()
			== 64;
		bool wide256 = is_256 (value);
		llvm::Intrinsic::ID id =
			wide64 ? (wide256 ? q256 : q128) : (wide256 ? d256 : d128);

		builder.CreateRet (builder.CreateIntrinsic (id, {}, { value, count }));
		return llvm::Error::success ();
	}

	template <llvm::Intrinsic::ID d128, llvm::Intrinsic::ID d256>
	static BuiltinResult avx2_shift_variable_arith (MethodLLVMEmitter &emitter,
	                                                llvm::IRBuilder<> &builder, MonoMethod *)
	{
		llvm::Value *value = argument (emitter, 0);
		llvm::Value *count = argument (emitter, 1);

		builder.CreateRet (
			builder.CreateIntrinsic (is_256 (value) ? d256 : d128, {}, { value, count }));
		return llvm::Error::success ();
	}

	/// PSHUFD applies its control byte independently to each 128-bit half.
	static BuiltinResult avx2_shuffle_epi32 (MethodLLVMEmitter &emitter, llvm::IRBuilder<> &builder,
	                                         MonoMethod *)
	{
		llvm::Value *value = argument (emitter, 0);
		llvm::Value *control = builder.CreateZExt (argument (emitter, 1), builder.getInt32Ty ());
		llvm::Type *vector_type = value->getType ();
		unsigned lanes = llvm::cast<llvm::FixedVectorType> (vector_type)->getNumElements ();
		llvm::Value *result = llvm::Constant::getNullValue (vector_type);

		for (unsigned i = 0; i < lanes; i++) {
			unsigned h = i / 4;
			unsigned j = i % 4;
			llvm::Value *select = builder.CreateAnd (builder.CreateLShr (control, 2 * j), 3);
			llvm::Value *source_lane = builder.CreateAdd (builder.getInt32 (h * 4), select);

			result = builder.CreateInsertElement (
				result, builder.CreateExtractElement (value, source_lane), i);
		}

		builder.CreateRet (result);
		return llvm::Error::success ();
	}

	/// Shuffle four words in each half while preserving the other four.
	template <bool high>
	static BuiltinResult avx2_shuffle_words (MethodLLVMEmitter &emitter, llvm::IRBuilder<> &builder,
	                                         MonoMethod *)
	{
		llvm::Value *value = argument (emitter, 0);
		llvm::Value *control = builder.CreateZExt (argument (emitter, 1), builder.getInt32Ty ());
		llvm::Type *vector_type = value->getType ();
		unsigned lanes = llvm::cast<llvm::FixedVectorType> (vector_type)->getNumElements ();
		unsigned base_offset = high ? 4 : 0;
		llvm::Value *result = value;

		for (unsigned h = 0; h < lanes / 8; h++) {
			for (unsigned j = 0; j < 4; j++) {
				unsigned i = h * 8 + base_offset + j;
				llvm::Value *select = builder.CreateAnd (builder.CreateLShr (control, 2 * j), 3);
				llvm::Value *source_lane =
					builder.CreateAdd (builder.getInt32 (h * 8 + base_offset), select);

				result = builder.CreateInsertElement (
					result, builder.CreateExtractElement (value, source_lane), i);
			}
		}

		builder.CreateRet (result);
		return llvm::Error::success ();
	}

	/// LLVM returns four 64-bit sums; the managed API exposes the same bits as ushort lanes.
	static BuiltinResult avx2_sum_abs_diff (MethodLLVMEmitter &emitter, llvm::IRBuilder<> &builder,
	                                        MonoMethod *)
	{
		llvm::Value *result = builder.CreateIntrinsic (
			llvm::Intrinsic::x86_avx2_psad_bw, {}, { argument (emitter, 0), argument (emitter, 1) });

		builder.CreateRet (builder.CreateBitCast (result, return_type (emitter)));
		return llvm::Error::success ();
	}

	/// Dispatch the runtime control byte to the immediate-only LLVM intrinsic.
	static BuiltinResult avx2_multiple_sum_abs_diff (MethodLLVMEmitter &emitter,
	                                                 llvm::IRBuilder<> &builder, MonoMethod *)
	{
		llvm::Value *left = argument (emitter, 0);
		llvm::Value *right = argument (emitter, 1);
		llvm::Value *control = builder.CreateAnd (argument (emitter, 2), builder.getInt8 (7));
		llvm::Value *result =
			dispatch_control (builder, control, 8, return_type (emitter), [&] (unsigned c) {
				return builder.CreateIntrinsic (llvm::Intrinsic::x86_avx2_mpsadbw, {},
				                                { left, right, builder.getInt8 (c) });
			});

		builder.CreateRet (result);
		return llvm::Error::success ();
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
const ClassKey sse41 = { nullptr, "System.Runtime.Intrinsics.X86", "Sse41" };
const ClassKey sse42 = { nullptr, "System.Runtime.Intrinsics.X86", "Sse42" };
const ClassKey avx = { nullptr, "System.Runtime.Intrinsics.X86", "Avx" };
const ClassKey avx2 = { nullptr, "System.Runtime.Intrinsics.X86", "Avx2" };
const ClassKey popcnt = { nullptr, "System.Runtime.Intrinsics.X86", "Popcnt" };
const ClassKey lzcnt = { nullptr, "System.Runtime.Intrinsics.X86", "Lzcnt" };
const ClassKey bmi1 = { nullptr, "System.Runtime.Intrinsics.X86", "Bmi1" };
const ClassKey bmi2 = { nullptr, "System.Runtime.Intrinsics.X86", "Bmi2" };
const ClassKey aes = { nullptr, "System.Runtime.Intrinsics.X86", "Aes" };

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

const BuiltinBody sse41_table[] = {
	{ sse41, "get_IsSupported", "", false, nullptr, SseEmitters::sse41_is_supported },

	{ sse41, "Blend", "VVS", false, sse41_lowering, SseEmitters::blend },
	{ sse41, "BlendVariable", "VVV", false, sse41_lowering, SseEmitters::blend_variable },

	{ sse41, "Ceiling", "V", false, sse41_lowering, SseEmitters::round<10> },
	{ sse41, "CeilingScalar", "V", false, sse41_lowering, SseEmitters::round_scalar1<10> },
	{ sse41, "CeilingScalar", "VV", false, sse41_lowering, SseEmitters::round_scalar2<10> },
	{ sse41, "Floor", "V", false, sse41_lowering, SseEmitters::round<9> },
	{ sse41, "FloorScalar", "V", false, sse41_lowering, SseEmitters::round_scalar1<9> },
	{ sse41, "FloorScalar", "VV", false, sse41_lowering, SseEmitters::round_scalar2<9> },
	{ sse41, "RoundToNearestInteger", "V", false, sse41_lowering, SseEmitters::round<8> },
	{ sse41, "RoundToNegativeInfinity", "V", false, sse41_lowering, SseEmitters::round<9> },
	{ sse41, "RoundToPositiveInfinity", "V", false, sse41_lowering, SseEmitters::round<10> },
	{ sse41, "RoundToZero", "V", false, sse41_lowering, SseEmitters::round<11> },
	{ sse41, "RoundCurrentDirection", "V", false, sse41_lowering, SseEmitters::round<4> },
	{ sse41, "RoundCurrentDirectionScalar", "V", false, sse41_lowering,
	  SseEmitters::round_scalar1<4> },
	{ sse41, "RoundToNearestIntegerScalar", "V", false, sse41_lowering,
	  SseEmitters::round_scalar1<8> },
	{ sse41, "RoundToNegativeInfinityScalar", "V", false, sse41_lowering,
	  SseEmitters::round_scalar1<9> },
	{ sse41, "RoundToPositiveInfinityScalar", "V", false, sse41_lowering,
	  SseEmitters::round_scalar1<10> },
	{ sse41, "RoundToZeroScalar", "V", false, sse41_lowering, SseEmitters::round_scalar1<11> },
	{ sse41, "RoundCurrentDirectionScalar", "VV", false, sse41_lowering,
	  SseEmitters::round_scalar2<4> },
	{ sse41, "RoundToNearestIntegerScalar", "VV", false, sse41_lowering,
	  SseEmitters::round_scalar2<8> },
	{ sse41, "RoundToNegativeInfinityScalar", "VV", false, sse41_lowering,
	  SseEmitters::round_scalar2<9> },
	{ sse41, "RoundToPositiveInfinityScalar", "VV", false, sse41_lowering,
	  SseEmitters::round_scalar2<10> },
	{ sse41, "RoundToZeroScalar", "VV", false, sse41_lowering, SseEmitters::round_scalar2<11> },

	{ sse41, "CompareEqual", "VV", false, sse41_lowering,
	  SseEmitters::compare2<0, llvm::CmpInst::ICMP_EQ> },

	{ sse41, "ConvertToVector128Int16", "V", false, sse41_lowering, SseEmitters::convert_widen },
	{ sse41, "ConvertToVector128Int32", "V", false, sse41_lowering, SseEmitters::convert_widen },
	{ sse41, "ConvertToVector128Int64", "V", false, sse41_lowering, SseEmitters::convert_widen },

	{ sse41, "DotProduct", "VVS", false, sse41_lowering, SseEmitters::dot_product },

	{ sse41, "Extract", "VS", false, sse41_lowering, SseEmitters::extract },
	{ sse41, "Insert", "VSS", false, sse41_lowering, SseEmitters::insert },
	{ sse41, "Insert", "VVS", false, sse41_lowering, SseEmitters::insert_ps },

	{ sse41, "Max", "VV", false, sse41_lowering, SseEmitters::extremum<true> },
	{ sse41, "Min", "VV", false, sse41_lowering, SseEmitters::extremum<false> },
	{ sse41, "MinHorizontal", "V", false, sse41_lowering,
	  SseEmitters::unary_intrinsic<Intr::x86_sse41_phminposuw> },

	{ sse41, "MultipleSumAbsoluteDifferences", "VVS", false, sse41_lowering,
	  SseEmitters::multiple_sum_abs_diff },

	{ sse41, "Multiply", "VV", false, sse41_lowering, SseEmitters::multiply_widen },
	{ sse41, "MultiplyLow", "VV", false, sse41_lowering, SseEmitters::multiply_low },

	{ sse41, "PackUnsignedSaturate", "VV", false, sse41_lowering,
	  SseEmitters::binary_intrinsic<Intr::x86_sse41_packusdw> },

	{ sse41, "LoadAlignedVector128NonTemporal", "S", false, sse41_lowering,
	  SseEmitters::load_aligned },

	{ sse41, "TestAllOnes", "V", false, sse41_lowering, SseEmitters::test_all_ones },
	{ sse41, "TestAllZeros", "VV", false, sse41_lowering,
	  SseEmitters::test<Intr::x86_sse41_ptestz> },
	{ sse41, "TestC", "VV", false, sse41_lowering, SseEmitters::test<Intr::x86_sse41_ptestc> },
	{ sse41, "TestMixOnesZeros", "VV", false, sse41_lowering,
	  SseEmitters::test<Intr::x86_sse41_ptestnzc> },
	{ sse41, "TestNotZAndNotC", "VV", false, sse41_lowering,
	  SseEmitters::test<Intr::x86_sse41_ptestnzc> },
	{ sse41, "TestZ", "VV", false, sse41_lowering, SseEmitters::test<Intr::x86_sse41_ptestz> },

	{ sse41, {}, any_signature, false, nullptr, SseEmitters::unimplemented },
};

const BuiltinBody sse42_table[] = {
	{ sse42, "get_IsSupported", "", false, nullptr, SseEmitters::sse42_is_supported },

	{ sse42, "CompareImplicitLength", "VVSS", false, sse42_lowering,
	  SseEmitters::compare_implicit_length },
	{ sse42, "CompareExplicitLength", "VSVSSS", false, sse42_lowering,
	  SseEmitters::compare_explicit_length },
	{ sse42, "CompareImplicitLengthIndex", "VVS", false, sse42_lowering,
	  SseEmitters::compare_implicit_length_index },
	{ sse42, "CompareExplicitLengthIndex", "VSVSS", false, sse42_lowering,
	  SseEmitters::compare_explicit_length_index },
	{ sse42, "CompareImplicitLengthBitMask", "VVS", false, sse42_lowering,
	  SseEmitters::compare_implicit_length_mask<false> },
	{ sse42, "CompareImplicitLengthUnitMask", "VVS", false, sse42_lowering,
	  SseEmitters::compare_implicit_length_mask<true> },
	{ sse42, "CompareExplicitLengthBitMask", "VSVSS", false, sse42_lowering,
	  SseEmitters::compare_explicit_length_mask<false> },
	{ sse42, "CompareExplicitLengthUnitMask", "VSVSS", false, sse42_lowering,
	  SseEmitters::compare_explicit_length_mask<true> },

	{ sse42, "CompareGreaterThan", "VV", false, sse42_lowering,
	  SseEmitters::compare2<6, llvm::CmpInst::ICMP_SGT> },

	{ sse42, "Crc32", "SS", false, sse42_lowering, SseEmitters::crc32 },

	{ sse42, {}, any_signature, false, nullptr, SseEmitters::unimplemented },
};

const BuiltinBody avx_table[] = {
	{ avx, "get_IsSupported", "", false, nullptr, SseEmitters::avx_is_supported },

	{ avx, "Add", "VV", false, avx_lowering, SseEmitters::binary<SseEmitters::add> },
	{ avx, "Subtract", "VV", false, avx_lowering, SseEmitters::binary<SseEmitters::sub> },
	{ avx, "Multiply", "VV", false, avx_lowering, SseEmitters::binary<SseEmitters::mul> },
	{ avx, "Divide", "VV", false, avx_lowering, SseEmitters::binary<SseEmitters::div> },

	{ avx, "And", "VV", false, avx_lowering, SseEmitters::bitwise<Ops::And> },
	{ avx, "Or", "VV", false, avx_lowering, SseEmitters::bitwise<Ops::Or> },
	{ avx, "Xor", "VV", false, avx_lowering, SseEmitters::bitwise<Ops::Xor> },
	{ avx, "AndNot", "VV", false, avx_lowering, SseEmitters::and_not },

	{ avx, "AddSubtract", "VV", false, avx_lowering,
	  SseEmitters::horizontal<Intr::x86_avx_addsub_ps_256, Intr::x86_avx_addsub_pd_256> },
	{ avx, "HorizontalAdd", "VV", false, avx_lowering,
	  SseEmitters::horizontal<Intr::x86_avx_hadd_ps_256, Intr::x86_avx_hadd_pd_256> },
	{ avx, "HorizontalSubtract", "VV", false, avx_lowering,
	  SseEmitters::horizontal<Intr::x86_avx_hsub_ps_256, Intr::x86_avx_hsub_pd_256> },
	{ avx, "Max", "VV", false, avx_lowering,
	  SseEmitters::horizontal<Intr::x86_avx_max_ps_256, Intr::x86_avx_max_pd_256> },
	{ avx, "Min", "VV", false, avx_lowering,
	  SseEmitters::horizontal<Intr::x86_avx_min_ps_256, Intr::x86_avx_min_pd_256> },

	{ avx, "Sqrt", "V", false, avx_lowering, SseEmitters::sqrt },
	{ avx, "Reciprocal", "V", false, avx_lowering,
	  SseEmitters::unary_intrinsic<Intr::x86_avx_rcp_ps_256> },
	{ avx, "ReciprocalSqrt", "V", false, avx_lowering,
	  SseEmitters::unary_intrinsic<Intr::x86_avx_rsqrt_ps_256> },

	{ avx, "Compare", "VVS", false, avx_lowering, SseEmitters::avx_compare },
	{ avx, "CompareScalar", "VVS", false, avx_lowering, SseEmitters::avx_compare_scalar },

	{ avx, "Ceiling", "V", false, avx_lowering, SseEmitters::avx_round<10> },
	{ avx, "Floor", "V", false, avx_lowering, SseEmitters::avx_round<9> },
	{ avx, "RoundToNearestInteger", "V", false, avx_lowering, SseEmitters::avx_round<8> },
	{ avx, "RoundToNegativeInfinity", "V", false, avx_lowering, SseEmitters::avx_round<9> },
	{ avx, "RoundToPositiveInfinity", "V", false, avx_lowering, SseEmitters::avx_round<10> },
	{ avx, "RoundToZero", "V", false, avx_lowering, SseEmitters::avx_round<11> },
	{ avx, "RoundCurrentDirection", "V", false, avx_lowering, SseEmitters::avx_round<4> },

	{ avx, "Blend", "VVS", false, avx_lowering, SseEmitters::blend },
	{ avx, "BlendVariable", "VVV", false, avx_lowering, SseEmitters::avx_blend_variable },

	{ avx, "BroadcastScalarToVector128", "S", false, avx_lowering, SseEmitters::broadcast_scalar },
	{ avx, "BroadcastScalarToVector256", "S", false, avx_lowering, SseEmitters::broadcast_scalar },
	{ avx, "BroadcastVector128ToVector256", "S", false, avx_lowering,
	  SseEmitters::broadcast_vector128 },

	{ avx, "ConvertToSingle", "V", false, avx_lowering, SseEmitters::to_single },
	{ avx, "ConvertToVector128Int32", "V", false, avx_lowering,
	  SseEmitters::unary_intrinsic<Intr::x86_avx_cvt_pd2dq_256> },
	{ avx, "ConvertToVector128Single", "V", false, avx_lowering,
	  SseEmitters::unary_intrinsic<Intr::x86_avx_cvt_pd2_ps_256> },
	{ avx, "ConvertToVector128Int32WithTruncation", "V", false, avx_lowering,
	  SseEmitters::unary_intrinsic<Intr::x86_avx_cvtt_pd2dq_256> },
	{ avx, "ConvertToVector256Int32", "V", false, avx_lowering,
	  SseEmitters::unary_intrinsic<Intr::x86_avx_cvt_ps2dq_256> },
	{ avx, "ConvertToVector256Int32WithTruncation", "V", false, avx_lowering,
	  SseEmitters::unary_intrinsic<Intr::x86_avx_cvtt_ps2dq_256> },
	{ avx, "ConvertToVector256Single", "V", false, avx_lowering, SseEmitters::int_to_float },
	{ avx, "ConvertToVector256Double", "V", false, avx_lowering, SseEmitters::widen_to_double },

	{ avx, "DotProduct", "VVS", false, avx_lowering, SseEmitters::avx_dot_product },

	{ avx, "DuplicateEvenIndexed", "V", false, avx_lowering,
	  SseEmitters::duplicate_even_indexed },
	{ avx, "DuplicateOddIndexed", "V", false, avx_lowering, SseEmitters::duplicate_odd_indexed },

	{ avx, "ExtractVector128", "VS", false, avx_lowering, SseEmitters::extract_vector128 },
	{ avx, "ExtractVector128", "SVS", false, avx_lowering,
	  SseEmitters::extract_vector128_store },
	{ avx, "InsertVector128", "VVS", false, avx_lowering, SseEmitters::insert_vector128 },
	{ avx, "InsertVector128", "VSS", false, avx_lowering, SseEmitters::insert_vector128_load },

	{ avx, "ExtendToVector256", "V", false, avx_lowering, SseEmitters::extend_to_vector256 },
	{ avx, "GetLowerHalf", "V", false, avx_lowering, SseEmitters::get_lower_half },

	{ avx, "LoadVector256", "S", false, avx_lowering, SseEmitters::load_unaligned },
	{ avx, "LoadAlignedVector256", "S", false, avx_lowering, SseEmitters::load_aligned256 },
	{ avx, "LoadDquVector256", "S", false, avx_lowering, SseEmitters::load_dqu },
	{ avx, "Store", "SV", false, avx_lowering, SseEmitters::store_unaligned },
	{ avx, "StoreAligned", "SV", false, avx_lowering, SseEmitters::store_aligned256 },
	{ avx, "StoreAlignedNonTemporal", "SV", false, avx_lowering, SseEmitters::store_aligned256 },

	{ avx, "MaskLoad", "SV", false, avx_lowering, SseEmitters::mask_load },
	{ avx, "MaskStore", "SVV", false, avx_lowering, SseEmitters::mask_store },

	{ avx, "MoveMask", "V", false, avx_lowering, SseEmitters::avx_movmsk },

	{ avx, "Permute", "VS", false, avx_lowering, SseEmitters::permute },
	{ avx, "PermuteVar", "VV", false, avx_lowering, SseEmitters::permute_var },
	{ avx, "Permute2x128", "VVS", false, avx_lowering, SseEmitters::permute2x128 },
	{ avx, "Shuffle", "VVS", false, avx_lowering, SseEmitters::avx_shuffle },

	{ avx, "SetVector256", "SSSS", false, avx_lowering, SseEmitters::set_vector256 },
	{ avx, "SetVector256", "SSSSSSSS", false, avx_lowering, SseEmitters::set_vector256 },
	{ avx, "SetVector256", "SSSSSSSS" "SSSSSSSS", false, avx_lowering,
	  SseEmitters::set_vector256 },
	{ avx, "SetVector256", "SSSSSSSS" "SSSSSSSS" "SSSSSSSS" "SSSSSSSS", false, avx_lowering,
	  SseEmitters::set_vector256 },
	{ avx, "SetAllVector256", "S", false, avx_lowering, SseEmitters::set_all_vector256 },
	{ avx, "SetHighLow", "VV", false, avx_lowering, SseEmitters::set_high_low },
	{ avx, "SetZeroVector256", "", false, avx_lowering, SseEmitters::set_zero },

	{ avx, "StaticCast", "V", false, avx_lowering, SseEmitters::static_cast_vector },

	{ avx, "UnpackHigh", "VV", false, avx_lowering, SseEmitters::unpack<true> },
	{ avx, "UnpackLow", "VV", false, avx_lowering, SseEmitters::unpack<false> },

	{ avx, "TestC", "VV", false, avx_lowering,
	  SseEmitters::avx_test<Intr::x86_avx_vtestc_ps, Intr::x86_avx_vtestc_pd,
	                        Intr::x86_avx_vtestc_ps_256, Intr::x86_avx_vtestc_pd_256,
	                        Intr::x86_avx_ptestc_256> },
	{ avx, "TestZ", "VV", false, avx_lowering,
	  SseEmitters::avx_test<Intr::x86_avx_vtestz_ps, Intr::x86_avx_vtestz_pd,
	                        Intr::x86_avx_vtestz_ps_256, Intr::x86_avx_vtestz_pd_256,
	                        Intr::x86_avx_ptestz_256> },
	{ avx, "TestNotZAndNotC", "VV", false, avx_lowering,
	  SseEmitters::avx_test<Intr::x86_avx_vtestnzc_ps, Intr::x86_avx_vtestnzc_pd,
	                        Intr::x86_avx_vtestnzc_ps_256, Intr::x86_avx_vtestnzc_pd_256,
	                        Intr::x86_avx_ptestnzc_256> },

	{ avx, {}, any_signature, false, nullptr, SseEmitters::unimplemented },
};

const BuiltinBody avx2_table[] = {
	{ avx2, "get_IsSupported", "", false, nullptr, SseEmitters::avx2_is_supported },

	{ avx2, "Abs", "V", false, avx2_lowering, SseEmitters::abs },

	{ avx2, "Add", "VV", false, avx2_lowering, SseEmitters::binary<SseEmitters::add> },
	{ avx2, "AddSaturate", "VV", false, avx2_lowering, SseEmitters::saturating<true> },
	{ avx2, "Subtract", "VV", false, avx2_lowering, SseEmitters::binary<SseEmitters::sub> },
	{ avx2, "SubtractSaturate", "VV", false, avx2_lowering, SseEmitters::saturating<false> },

	{ avx2, "AlignRight", "VVS", false, avx2_lowering, SseEmitters::avx2_align_right },

	{ avx2, "And", "VV", false, avx2_lowering, SseEmitters::bitwise<Ops::And> },
	{ avx2, "AndNot", "VV", false, avx2_lowering, SseEmitters::and_not },
	{ avx2, "Or", "VV", false, avx2_lowering, SseEmitters::bitwise<Ops::Or> },
	{ avx2, "Xor", "VV", false, avx2_lowering, SseEmitters::bitwise<Ops::Xor> },

	{ avx2, "Average", "VV", false, avx2_lowering, SseEmitters::avx2_average },

	{ avx2, "Blend", "VVS", false, avx2_lowering, SseEmitters::avx2_blend },
	{ avx2, "BlendVariable", "VVV", false, avx2_lowering, SseEmitters::avx2_blend_variable },

	{ avx2, "BroadcastScalarToVector128", "V", false, avx2_lowering,
	  SseEmitters::avx2_broadcast_scalar_reg },
	{ avx2, "BroadcastScalarToVector256", "V", false, avx2_lowering,
	  SseEmitters::avx2_broadcast_scalar_reg },
	{ avx2, "BroadcastVector128ToVector256", "S", false, avx2_lowering,
	  SseEmitters::broadcast_vector128 },

	{ avx2, "CompareEqual", "VV", false, avx2_lowering,
	  SseEmitters::compare2<0, llvm::CmpInst::ICMP_EQ> },
	{ avx2, "CompareGreaterThan", "VV", false, avx2_lowering,
	  SseEmitters::compare2<6, llvm::CmpInst::ICMP_SGT> },

	{ avx2, "ConvertToDouble", "V", false, avx2_lowering, SseEmitters::to_single },
	{ avx2, "ConvertToInt32", "V", false, avx2_lowering, SseEmitters::to_single },
	{ avx2, "ConvertToUInt32", "V", false, avx2_lowering, SseEmitters::to_single },

	{ avx2, "ConvertToVector256Int16", "V", false, avx2_lowering, SseEmitters::convert_widen },
	{ avx2, "ConvertToVector256UInt16", "V", false, avx2_lowering, SseEmitters::convert_widen },
	{ avx2, "ConvertToVector256Int32", "V", false, avx2_lowering, SseEmitters::convert_widen },
	{ avx2, "ConvertToVector256UInt32", "V", false, avx2_lowering, SseEmitters::convert_widen },
	{ avx2, "ConvertToVector256Int64", "V", false, avx2_lowering, SseEmitters::convert_widen },
	{ avx2, "ConvertToVector256UInt64", "V", false, avx2_lowering, SseEmitters::convert_widen },

	{ avx2, "ExtractVector128", "VS", false, avx2_lowering, SseEmitters::extract_vector128 },
	{ avx2, "ExtractVector128", "SVS", false, avx2_lowering, SseEmitters::extract_vector128_store },
	{ avx2, "InsertVector128", "VVS", false, avx2_lowering, SseEmitters::insert_vector128 },
	{ avx2, "InsertVector128", "VSS", false, avx2_lowering, SseEmitters::insert_vector128_load },

	{ avx2, "GatherVector128", "SVS", false, avx2_lowering, SseEmitters::avx2_gather<false> },
	{ avx2, "GatherVector256", "SVS", false, avx2_lowering, SseEmitters::avx2_gather<false> },
	{ avx2, "GatherMaskVector128", "VSVVS", false, avx2_lowering, SseEmitters::avx2_gather<true> },
	{ avx2, "GatherMaskVector256", "VSVVS", false, avx2_lowering, SseEmitters::avx2_gather<true> },

	{ avx2, "HorizontalAdd", "VV", false, avx2_lowering,
	  SseEmitters::horizontal_int<Intr::x86_avx2_phadd_w, Intr::x86_avx2_phadd_d> },
	{ avx2, "HorizontalAddSaturate", "VV", false, avx2_lowering,
	  SseEmitters::binary_intrinsic<Intr::x86_avx2_phadd_sw> },
	{ avx2, "HorizontalSubtract", "VV", false, avx2_lowering,
	  SseEmitters::horizontal_int<Intr::x86_avx2_phsub_w, Intr::x86_avx2_phsub_d> },
	{ avx2, "HorizontalSubtractSaturate", "VV", false, avx2_lowering,
	  SseEmitters::binary_intrinsic<Intr::x86_avx2_phsub_sw> },

	{ avx2, "LoadAlignedVector256NonTemporal", "S", false, avx2_lowering,
	  SseEmitters::load_aligned256 },

	{ avx2, "MaskLoad", "SV", false, avx2_lowering, SseEmitters::avx2_mask_load },
	{ avx2, "MaskStore", "SVV", false, avx2_lowering, SseEmitters::avx2_mask_store },

	{ avx2, "Max", "VV", false, avx2_lowering, SseEmitters::extremum<true> },
	{ avx2, "Min", "VV", false, avx2_lowering, SseEmitters::extremum<false> },

	{ avx2, "MoveMask", "V", false, avx2_lowering,
	  SseEmitters::unary_intrinsic<Intr::x86_avx2_pmovmskb> },

	{ avx2, "MultipleSumAbsoluteDifferences", "VVS", false, avx2_lowering,
	  SseEmitters::avx2_multiple_sum_abs_diff },

	{ avx2, "Multiply", "VV", false, avx2_lowering, SseEmitters::avx2_multiply_widen },
	{ avx2, "MultiplyHigh", "VV", false, avx2_lowering, SseEmitters::avx2_multiply_high },
	{ avx2, "MultiplyHighRoundScale", "VV", false, avx2_lowering,
	  SseEmitters::binary_intrinsic<Intr::x86_avx2_pmul_hr_sw> },
	{ avx2, "MultiplyLow", "VV", false, avx2_lowering, SseEmitters::multiply_low },
	{ avx2, "MultiplyAddAdjacent", "VV", false, avx2_lowering,
	  SseEmitters::avx2_multiply_add_adjacent },

	{ avx2, "PackSignedSaturate", "VV", false, avx2_lowering, SseEmitters::avx2_pack_signed_saturate },
	{ avx2, "PackUnsignedSaturate", "VV", false, avx2_lowering,
	  SseEmitters::avx2_pack_unsigned_saturate },

	{ avx2, "Permute2x128", "VVS", false, avx2_lowering, SseEmitters::permute2x128 },
	{ avx2, "Permute4x64", "VS", false, avx2_lowering, SseEmitters::permute4x64 },
	{ avx2, "PermuteVar8x32", "VV", false, avx2_lowering, SseEmitters::avx2_permute_var8x32 },

	{ avx2, "ShiftLeftLogical", "VV", false, avx2_lowering,
	  SseEmitters::avx2_shift_count<Intr::x86_avx2_psll_w, Intr::x86_avx2_psll_d,
	                                Intr::x86_avx2_psll_q> },
	{ avx2, "ShiftLeftLogical", "VS", false, avx2_lowering,
	  SseEmitters::avx2_shift_immediate<Intr::x86_avx2_pslli_w, Intr::x86_avx2_pslli_d,
	                                    Intr::x86_avx2_pslli_q> },
	{ avx2, "ShiftRightLogical", "VV", false, avx2_lowering,
	  SseEmitters::avx2_shift_count<Intr::x86_avx2_psrl_w, Intr::x86_avx2_psrl_d,
	                                Intr::x86_avx2_psrl_q> },
	{ avx2, "ShiftRightLogical", "VS", false, avx2_lowering,
	  SseEmitters::avx2_shift_immediate<Intr::x86_avx2_psrli_w, Intr::x86_avx2_psrli_d,
	                                    Intr::x86_avx2_psrli_q> },
	{ avx2, "ShiftRightArithmetic", "VV", false, avx2_lowering,
	  SseEmitters::avx2_shift_count_arith<Intr::x86_avx2_psra_w, Intr::x86_avx2_psra_d> },
	{ avx2, "ShiftRightArithmetic", "VS", false, avx2_lowering,
	  SseEmitters::avx2_shift_immediate_arith<Intr::x86_avx2_psrai_w, Intr::x86_avx2_psrai_d> },

	{ avx2, "ShiftLeftLogical128BitLane", "VS", false, avx2_lowering,
	  SseEmitters::shift_128_bit_lane<true> },
	{ avx2, "ShiftRightLogical128BitLane", "VS", false, avx2_lowering,
	  SseEmitters::shift_128_bit_lane<false> },

	{ avx2, "ShiftLeftLogicalVariable", "VV", false, avx2_lowering,
	  SseEmitters::avx2_shift_variable<Intr::x86_avx2_psllv_d, Intr::x86_avx2_psllv_d_256,
	                                   Intr::x86_avx2_psllv_q, Intr::x86_avx2_psllv_q_256> },
	{ avx2, "ShiftRightLogicalVariable", "VV", false, avx2_lowering,
	  SseEmitters::avx2_shift_variable<Intr::x86_avx2_psrlv_d, Intr::x86_avx2_psrlv_d_256,
	                                   Intr::x86_avx2_psrlv_q, Intr::x86_avx2_psrlv_q_256> },
	{ avx2, "ShiftRightArithmeticVariable", "VV", false, avx2_lowering,
	  SseEmitters::avx2_shift_variable_arith<Intr::x86_avx2_psrav_d, Intr::x86_avx2_psrav_d_256> },

	{ avx2, "Shuffle", "VV", false, avx2_lowering,
	  SseEmitters::binary_intrinsic<Intr::x86_avx2_pshuf_b> },
	{ avx2, "Shuffle", "VS", false, avx2_lowering, SseEmitters::avx2_shuffle_epi32 },
	{ avx2, "ShuffleHigh", "VS", false, avx2_lowering, SseEmitters::avx2_shuffle_words<true> },
	{ avx2, "ShuffleLow", "VS", false, avx2_lowering, SseEmitters::avx2_shuffle_words<false> },

	{ avx2, "Sign", "VV", false, avx2_lowering,
	  SseEmitters::sign<Intr::x86_avx2_psign_b, Intr::x86_avx2_psign_w, Intr::x86_avx2_psign_d> },

	{ avx2, "SumAbsoluteDifferences", "VV", false, avx2_lowering,
	  SseEmitters::avx2_sum_abs_diff },

	{ avx2, "UnpackHigh", "VV", false, avx2_lowering, SseEmitters::unpack<true> },
	{ avx2, "UnpackLow", "VV", false, avx2_lowering, SseEmitters::unpack<false> },

	{ avx2, {}, any_signature, false, nullptr, SseEmitters::unimplemented },
};

const BuiltinBody popcnt_table[] = {
	{ popcnt, "get_IsSupported", "", false, nullptr, SseEmitters::popcnt_is_supported },

	{ popcnt, "PopCount", "S", false, popcnt_lowering, SseEmitters::popcount },
};

const BuiltinBody lzcnt_table[] = {
	{ lzcnt, "get_IsSupported", "", false, nullptr, SseEmitters::lzcnt_is_supported },

	{ lzcnt, "LeadingZeroCount", "S", false, lzcnt_lowering, SseEmitters::leading_zero_count },
};

const BuiltinBody bmi1_table[] = {
	{ bmi1, "get_IsSupported", "", false, nullptr, SseEmitters::bmi1_is_supported },

	{ bmi1, "AndNot", "SS", false, bmi1_lowering, SseEmitters::and_not_scalar },
	{ bmi1, "BitFieldExtract", "SSS", false, bmi1_lowering,
	  SseEmitters::bit_field_extract_start_length },
	{ bmi1, "BitFieldExtract", "SS", false, bmi1_lowering, SseEmitters::bit_field_extract_control },
	{ bmi1, "ExtractLowestSetBit", "S", false, bmi1_lowering, SseEmitters::extract_lowest_set_bit },
	{ bmi1, "GetMaskUpToLowestSetBit", "S", false, bmi1_lowering,
	  SseEmitters::get_mask_up_to_lowest_set_bit },
	{ bmi1, "ResetLowestSetBit", "S", false, bmi1_lowering, SseEmitters::reset_lowest_set_bit },
	{ bmi1, "TrailingZeroCount", "S", false, bmi1_lowering, SseEmitters::trailing_zero_count },
};

const BuiltinBody bmi2_table[] = {
	{ bmi2, "get_IsSupported", "", false, nullptr, SseEmitters::bmi2_is_supported },

	{ bmi2, "ZeroHighBits", "SS", false, bmi2_lowering, SseEmitters::zero_high_bits },
	{ bmi2, "MultiplyNoFlags", "SSS", false, bmi2_lowering, SseEmitters::multiply_no_flags },
	{ bmi2, "ParallelBitDeposit", "SS", false, bmi2_lowering, SseEmitters::parallel_bit_deposit },
	{ bmi2, "ParallelBitExtract", "SS", false, bmi2_lowering, SseEmitters::parallel_bit_extract },
};

const BuiltinBody aes_table[] = {
	{ aes, "get_IsSupported", "", false, nullptr, SseEmitters::aes_is_supported },

	{ aes, "Decrypt", "VV", false, aes_lowering,
	  SseEmitters::aes_binary<Intr::x86_aesni_aesdec> },
	{ aes, "DecryptLast", "VV", false, aes_lowering,
	  SseEmitters::aes_binary<Intr::x86_aesni_aesdeclast> },
	{ aes, "Encrypt", "VV", false, aes_lowering,
	  SseEmitters::aes_binary<Intr::x86_aesni_aesenc> },
	{ aes, "EncryptLast", "VV", false, aes_lowering,
	  SseEmitters::aes_binary<Intr::x86_aesni_aesenclast> },
	{ aes, "InverseMixColumns", "V", false, aes_lowering, SseEmitters::aes_inverse_mix_columns },
	{ aes, "KeygenAssist", "VS", false, aes_lowering, SseEmitters::aes_keygen_assist },
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
		made.insert (made.end (), std::begin (sse41_table), std::end (sse41_table));
		made.insert (made.end (), std::begin (sse42_table), std::end (sse42_table));
		made.insert (made.end (), std::begin (avx_table), std::end (avx_table));
		made.insert (made.end (), std::begin (avx2_table), std::end (avx2_table));
		made.insert (made.end (), std::begin (popcnt_table), std::end (popcnt_table));
		made.insert (made.end (), std::begin (lzcnt_table), std::end (lzcnt_table));
		made.insert (made.end (), std::begin (bmi1_table), std::end (bmi1_table));
		made.insert (made.end (), std::begin (bmi2_table), std::end (bmi2_table));
		made.insert (made.end (), std::begin (aes_table), std::end (aes_table));
		return made;
	} ();

	return all;
}

} // namespace mono

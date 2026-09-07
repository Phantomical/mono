/**
 * \file
 * \brief The bodies the backend writes for a SIMD type's operations.
 *
 * Each row must reproduce the managed body lane for lane. Tier 0 runs that body,
 * and a method's answer must not change when it promotes. Write a row from the
 * body, never from the SSE instruction it is expected to select.
 *
 * il_agrees false does not buy a way out of that rule here. It reaches
 * runs_at_tier0 (), which decides only for methods the backend is asked about,
 * and the interpreter never asks about a callee it reached itself. So a row
 * computing something its own IL does not answers one way under an interpreted
 * caller and another under a compiled one. SimdRuntime.get_AccelMode is the
 * method that wants such a row, and it is left on its IL for this reason.
 */

#include "intrinsics.hpp"

#include "../runtime/options.hpp"
#include "float-convert.hpp"
#include "method-to-llvm.hpp"
#include "simd-emit.hpp"

#include "mono/metadata/class-internals.h"

#include <llvm/ADT/SmallVector.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/InstrTypes.h>
#include <llvm/IR/Intrinsics.h>
#include <llvm/IR/Type.h>
#include <llvm/IR/Value.h>

#include <algorithm>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

namespace mono {

namespace {

/// The element a Mono.Simd struct holds.
///
/// The LLVM type does not carry it: Vector4i and Vector4ui both arrive as
/// <4 x i32>, and their shifts, compares, min and max differ.
enum class Lane { f32, f64, i8, u8, i16, u16, i32, u32, i64, u64 };

bool
lane_is_signed (Lane lane)
{
	switch (lane) {
	case Lane::i8:
	case Lane::i16:
	case Lane::i32:
	case Lane::i64:
		return true;
	default:
		return false;
	}
}

bool
lane_is_float (Lane lane)
{
	return lane == Lane::f32 || lane == Lane::f64;
}

std::optional<Lane>
lane_of_class (MonoClass *klass)
{
	static const struct {
		std::string_view name;
		Lane lane;
	} named[] = {
		{ "Vector4f", Lane::f32 },	{ "Vector2d", Lane::f64 },
		{ "Vector16sb", Lane::i8 },	{ "Vector16b", Lane::u8 },
		{ "Vector8s", Lane::i16 },	{ "Vector8us", Lane::u16 },
		{ "Vector4i", Lane::i32 },	{ "Vector4ui", Lane::u32 },
		{ "Vector2l", Lane::i64 },	{ "Vector2ul", Lane::u64 },
	};

	std::string_view name = m_class_get_name (klass);

	for (const auto &entry : named)
		if (entry.name == name)
			return entry.lane;

	return std::nullopt;
}

/// The element of the first Mono.Simd struct method takes.
///
/// One row answers every overload of a VectorOperations method, so the element
/// has to come off the method rather than off the row.
std::optional<Lane>
lane_of (MonoMethod *method)
{
	MonoMethodSignature *sig = mono_method_signature_internal (method);

	if (sig == nullptr)
		return std::nullopt;

	for (int i = 0; i < sig->param_count; ++i) {
		if (sig->params[i]->byref)
			continue;

		MonoClass *klass = mono_class_from_mono_type_internal (sig->params[i]);

		if (klass == nullptr || !m_class_is_simd_type (klass))
			continue;
		if (std::optional<Lane> lane = lane_of_class (klass))
			return lane;
	}

	return std::nullopt;
}

/// The integer vector a bitwise operation over type works on.
llvm::FixedVectorType *
bit_vector (llvm::FixedVectorType *type)
{
	llvm::Type *element = type->getElementType ();

	if (element->isIntegerTy ())
		return type;

	return llvm::FixedVectorType::get (
		llvm::Type::getIntNTy (element->getContext (),
	                               element->getScalarSizeInBits ()),
		type->getNumElements ());
}

/// The vector type with the same lane count and lanes of width bits.
llvm::FixedVectorType *
integer_lanes (llvm::FixedVectorType *type, unsigned width)
{
	return llvm::FixedVectorType::get (
		llvm::Type::getIntNTy (type->getContext (), width),
		type->getNumElements ());
}

} // namespace

/// The emitters the table below points at, written against SimdEmit.
struct SimdEmitters : SimdEmit {
	/// Which way a shift moves its lanes.
	enum class Direction { left, right };

	/// What a ConvertTo row makes of each lane.
	enum class LaneConvert {
		/// A float of the answer's own width, from an integer or another float.
		floating,
		/// An integer, through System.Math.Round.
		rounded,
		/// An integer, truncated toward zero.
		truncated,
	};

	/// Which lanes a one-operand shuffle selects, leaving the rest alone.
	enum class ShuffleWindow { whole, low, high };

	/// The first two arguments where both arrived as one vector type, and
	/// nothing otherwise, which leaves the managed body to be translated.
	/// Only a class the loader marked simd_type converts to a vector.
	static std::optional<std::pair<llvm::Value *, llvm::Value *>>
	operands (MethodLLVMEmitter &emitter)
	{
		llvm::Value *lhs = argument (emitter, 0);
		llvm::Value *rhs = argument (emitter, 1);

		if (!llvm::isa<llvm::FixedVectorType> (lhs->getType ()))
			return std::nullopt;
		if (lhs->getType () != rhs->getType ())
			return std::nullopt;

		return std::make_pair (lhs, rhs);
	}

	static llvm::FixedVectorType *type_of (llvm::Value *value)
	{
		return llvm::cast<llvm::FixedVectorType> (value->getType ());
	}

	/// Applies op to the callee's two arguments and returns the result.
	template <BinaryOp op>
	static BuiltinResult binary (MethodLLVMEmitter &emitter, llvm::IRBuilder<> &builder,
	                             MonoMethod *)
	{
		std::optional<std::pair<llvm::Value *, llvm::Value *>> args =
			operands (emitter);

		if (!args)
			return std::nullopt;

		builder.CreateRet (op (builder, args->first, args->second));
		return llvm::Error::success ();
	}

	/// Writes a lane-wise integer add. It wraps, because the managed body
	/// widens each lane, adds, and casts the sum back.
	static llvm::Value *add (llvm::IRBuilder<> &builder, llvm::Value *lhs,
	                         llvm::Value *rhs)
	{
		return builder.CreateAdd (lhs, rhs);
	}

	static llvm::Value *sub (llvm::IRBuilder<> &builder, llvm::Value *lhs,
	                         llvm::Value *rhs)
	{
		return builder.CreateSub (lhs, rhs);
	}

	static llvm::Value *mul (llvm::IRBuilder<> &builder, llvm::Value *lhs,
	                         llvm::Value *rhs)
	{
		return builder.CreateMul (lhs, rhs);
	}

	/// Applies opc to the two arguments' bits.
	///
	/// A float struct's bitwise operators read their operands through an int
	/// pointer, so the operation runs on the bit pattern.
	template <llvm::Instruction::BinaryOps opc>
	static BuiltinResult bitwise (MethodLLVMEmitter &emitter, llvm::IRBuilder<> &builder,
	                              MonoMethod *)
	{
		std::optional<std::pair<llvm::Value *, llvm::Value *>> args =
			operands (emitter);

		if (!args)
			return std::nullopt;

		llvm::FixedVectorType *type = type_of (args->first);
		llvm::FixedVectorType *bits = bit_vector (type);
		llvm::Value *result =
			builder.CreateBinOp (opc, builder.CreateBitCast (args->first, bits),
		                             builder.CreateBitCast (args->second, bits));

		builder.CreateRet (builder.CreateBitCast (result, type));
		return llvm::Error::success ();
	}

	/// Writes `~v1 & v2`, which is the order VectorOperations.AndNot uses.
	static BuiltinResult and_not (MethodLLVMEmitter &emitter, llvm::IRBuilder<> &builder,
	                              MonoMethod *)
	{
		std::optional<std::pair<llvm::Value *, llvm::Value *>> args =
			operands (emitter);

		if (!args)
			return std::nullopt;

		llvm::FixedVectorType *type = type_of (args->first);
		llvm::FixedVectorType *bits = bit_vector (type);
		llvm::Value *result = builder.CreateAnd (
			builder.CreateNot (builder.CreateBitCast (args->first, bits)),
			builder.CreateBitCast (args->second, bits));

		builder.CreateRet (builder.CreateBitCast (result, type));
		return llvm::Error::success ();
	}

	/// Multiplies every lane by a scalar the site passes either side of it.
	static BuiltinResult scalar_multiply (MethodLLVMEmitter &emitter,
	                                      llvm::IRBuilder<> &builder, MonoMethod *)
	{
		llvm::Value *vector = argument (emitter, 0);
		llvm::Value *scalar = argument (emitter, 1);

		if (!vector->getType ()->isVectorTy ())
			std::swap (vector, scalar);

		auto *type = llvm::dyn_cast<llvm::FixedVectorType> (vector->getType ());

		if (type == nullptr || scalar->getType () != type->getElementType ())
			return std::nullopt;

		llvm::Value *splat =
			builder.CreateVectorSplat (type->getNumElements (), scalar);

		builder.CreateRet (fmul (builder, splat, vector));
		return llvm::Error::success ();
	}

	/**
	 * Shifts every lane by a scalar count.
	 *
	 * C# shifts a lane narrower than int as an int and casts the answer back.
	 * The count is masked to 31, not to the lane's own width. A short shifted
	 * right by 17 therefore collapses to its sign in every bit. An i16 ashr
	 * instead masks the count to 1, which mostly keeps the original value.
	 *
	 * signed_lanes picks the widening and the right shift together. The two
	 * named shifts in VectorOperations ask for the arm their own name says
	 * rather than the one the struct's element would give.
	 */
	template <Direction direction, bool signed_lanes>
	static BuiltinResult shift (MethodLLVMEmitter &emitter, llvm::IRBuilder<> &builder,
	                            MonoMethod *)
	{
		llvm::Value *value = argument (emitter, 0);
		llvm::Value *amount = argument (emitter, 1);
		auto *type = llvm::dyn_cast<llvm::FixedVectorType> (value->getType ());

		if (type == nullptr || !type->getElementType ()->isIntegerTy ())
			return std::nullopt;
		if (!amount->getType ()->isIntegerTy (32))
			return std::nullopt;

		unsigned width = std::max (type->getScalarSizeInBits (), 32u);
		llvm::FixedVectorType *work = integer_lanes (type, width);
		llvm::Value *widened = signed_lanes ? builder.CreateSExt (value, work)
		                                    : builder.CreateZExt (value, work);
		llvm::Value *count = builder.CreateZExtOrTrunc (
			builder.CreateAnd (amount, builder.getInt32 (width - 1)),
			work->getElementType ());
		llvm::Value *splat =
			builder.CreateVectorSplat (type->getNumElements (), count);
		llvm::Value *shifted =
			direction == Direction::left ? builder.CreateShl (widened, splat)
			: signed_lanes		     ? builder.CreateAShr (widened, splat)
						     : builder.CreateLShr (widened, splat);

		builder.CreateRet (builder.CreateTrunc (shifted, type));
		return llvm::Error::success ();
	}

	/// Reinterprets the argument as the struct the conversion answers with.
	static BuiltinResult reinterpret (MethodLLVMEmitter &emitter,
	                                  llvm::IRBuilder<> &builder, MonoMethod *)
	{
		llvm::Value *value = argument (emitter, 0);
		auto *from = llvm::dyn_cast<llvm::FixedVectorType> (value->getType ());
		auto *to = llvm::dyn_cast<llvm::FixedVectorType> (
			return_type (emitter));

		if (from == nullptr || to == nullptr)
			return std::nullopt;
		if (from->getNumElements () * from->getScalarSizeInBits ()
		    != to->getNumElements () * to->getScalarSizeInBits ())
			return std::nullopt;

		builder.CreateRet (builder.CreateBitCast (value, to));
		return llvm::Error::success ();
	}

	/// Answers whether every lane compares equal, or whether any lane does not.
	template <bool equal>
	static BuiltinResult total_compare (MethodLLVMEmitter &emitter,
	                                    llvm::IRBuilder<> &builder, MonoMethod *)
	{
		std::optional<std::pair<llvm::Value *, llvm::Value *>> args =
			operands (emitter);
		llvm::Type *answer = return_type (emitter);

		if (!args || !answer->isIntegerTy ())
			return std::nullopt;

		llvm::Value *lhs = args->first;
		llvm::Value *rhs = args->second;
		llvm::Value *lanes;

		if (type_of (lhs)->getElementType ()->isFloatingPointTy ())
			lanes = equal ? builder.CreateFCmpOEQ (lhs, rhs)
			              : builder.CreateFCmpUNE (lhs, rhs);
		else
			lanes = equal ? builder.CreateICmpEQ (lhs, rhs)
			              : builder.CreateICmpNE (lhs, rhs);

		llvm::Value *reduced = builder.CreateIntrinsic (
			equal ? llvm::Intrinsic::vector_reduce_and
			      : llvm::Intrinsic::vector_reduce_or,
			{ lanes->getType () }, { lanes });

		builder.CreateRet (builder.CreateZExt (reduced, answer));
		return llvm::Error::success ();
	}

	/**
	 * Answers each lane with every bit set where the comparison holds.
	 *
	 * integer_predicate is the signed spelling. An unsigned struct takes the
	 * unsigned one, and BAD_ICMP_PREDICATE leaves an integer struct's overload
	 * on its own IL.
	 */
	template <llvm::CmpInst::Predicate float_predicate,
	          llvm::CmpInst::Predicate integer_predicate>
	static BuiltinResult compare_mask (MethodLLVMEmitter &emitter,
	                                   llvm::IRBuilder<> &builder, MonoMethod *method)
	{
		std::optional<std::pair<llvm::Value *, llvm::Value *>> args =
			operands (emitter);
		std::optional<Lane> lane = lane_of (method);

		if (!args || !lane)
			return std::nullopt;

		llvm::CmpInst::Predicate predicate = integer_predicate;

		if (lane_is_float (*lane)) {
			predicate = float_predicate;
		} else if (llvm::CmpInst::isSigned (predicate) && !lane_is_signed (*lane)) {
			predicate = llvm::ICmpInst::getUnsignedPredicate (predicate);
		}

		if (predicate == llvm::CmpInst::BAD_FCMP_PREDICATE
		    || predicate == llvm::CmpInst::BAD_ICMP_PREDICATE)
			return std::nullopt;

		llvm::FixedVectorType *type = type_of (args->first);
		llvm::Value *mask = builder.CreateSExt (
			builder.CreateCmp (predicate, args->first, args->second),
			bit_vector (type));

		builder.CreateRet (builder.CreateBitCast (mask, type));
		return llvm::Error::success ();
	}

	/**
	 * Answers each lane with System.Math's own Min or Max of the two.
	 *
	 * Math.Min (a, b) is `a < b ? a : (IsNaN (a) ? a : b)`. It answers with b
	 * for two zeros of opposite sign, and it propagates whichever operand is
	 * NaN rather than suppressing it the way llvm.minnum does.
	 */
	template <bool maximum>
	static BuiltinResult min_max (MethodLLVMEmitter &emitter, llvm::IRBuilder<> &builder,
	                              MonoMethod *method)
	{
		std::optional<std::pair<llvm::Value *, llvm::Value *>> args =
			operands (emitter);
		std::optional<Lane> lane = lane_of (method);

		if (!args || !lane)
			return std::nullopt;

		llvm::Value *lhs = args->first;
		llvm::Value *rhs = args->second;
		llvm::Value *result;

		if (lane_is_float (*lane)) {
			llvm::Value *takes_lhs = maximum ? builder.CreateFCmpOGT (lhs, rhs)
			                                 : builder.CreateFCmpOLT (lhs, rhs);
			llvm::Value *not_a_number = builder.CreateFCmpUNO (lhs, lhs);

			result = builder.CreateSelect (
				takes_lhs, lhs,
				builder.CreateSelect (not_a_number, lhs, rhs));
		} else {
			llvm::Intrinsic::ID id =
				maximum ? (lane_is_signed (*lane) ? llvm::Intrinsic::smax
			                                          : llvm::Intrinsic::umax)
			                : (lane_is_signed (*lane) ? llvm::Intrinsic::smin
			                                          : llvm::Intrinsic::umin);

			result = builder.CreateIntrinsic (id, { type_of (lhs) }, { lhs, rhs });
		}

		builder.CreateRet (result);
		return llvm::Error::success ();
	}

	/// Adds or subtracts each lane, clamped to the lane's range.
	///
	/// The managed body widens each lane, adds, then clamps with Math.Min and
	/// Math.Max, which is what this intrinsic computes.
	template <bool adding>
	static BuiltinResult saturating (MethodLLVMEmitter &emitter,
	                                 llvm::IRBuilder<> &builder, MonoMethod *method)
	{
		std::optional<std::pair<llvm::Value *, llvm::Value *>> args =
			operands (emitter);
		std::optional<Lane> lane = lane_of (method);

		if (!args || !lane || lane_is_float (*lane))
			return std::nullopt;

		llvm::Intrinsic::ID id =
			adding ? (lane_is_signed (*lane) ? llvm::Intrinsic::sadd_sat
		                                         : llvm::Intrinsic::uadd_sat)
		               : (lane_is_signed (*lane) ? llvm::Intrinsic::ssub_sat
		                                         : llvm::Intrinsic::usub_sat);

		builder.CreateRet (builder.CreateIntrinsic (
			id, { type_of (args->first) }, { args->first, args->second }));
		return llvm::Error::success ();
	}

	/// Answers each lane with `(a + b + 1) >> 1`, computed where the sum fits.
	static BuiltinResult average (MethodLLVMEmitter &emitter, llvm::IRBuilder<> &builder,
	                              MonoMethod *method)
	{
		std::optional<std::pair<llvm::Value *, llvm::Value *>> args =
			operands (emitter);
		std::optional<Lane> lane = lane_of (method);

		if (!args || !lane || lane_is_float (*lane) || lane_is_signed (*lane))
			return std::nullopt;

		llvm::FixedVectorType *type = type_of (args->first);
		llvm::FixedVectorType *wide =
			integer_lanes (type, type->getScalarSizeInBits () * 2);
		llvm::Value *sum = builder.CreateAdd (builder.CreateZExt (args->first, wide),
		                                      builder.CreateZExt (args->second, wide));

		sum = builder.CreateAdd (sum, llvm::ConstantInt::get (wide, 1));

		builder.CreateRet (builder.CreateTrunc (
			builder.CreateLShr (sum, llvm::ConstantInt::get (wide, 1)), type));
		return llvm::Error::success ();
	}

	/// Answers each lane with the top half of the widened product.
	static BuiltinResult multiply_store_high (MethodLLVMEmitter &emitter,
	                                          llvm::IRBuilder<> &builder,
	                                          MonoMethod *method)
	{
		std::optional<std::pair<llvm::Value *, llvm::Value *>> args =
			operands (emitter);
		std::optional<Lane> lane = lane_of (method);

		if (!args || !lane || lane_is_float (*lane))
			return std::nullopt;

		llvm::FixedVectorType *type = type_of (args->first);
		unsigned bits = type->getScalarSizeInBits ();
		llvm::FixedVectorType *wide = integer_lanes (type, bits * 2);
		bool is_signed = lane_is_signed (*lane);
		llvm::Value *lhs = is_signed ? builder.CreateSExt (args->first, wide)
		                             : builder.CreateZExt (args->first, wide);
		llvm::Value *rhs = is_signed ? builder.CreateSExt (args->second, wide)
		                             : builder.CreateZExt (args->second, wide);
		llvm::Value *product = builder.CreateMul (lhs, rhs);
		llvm::Constant *count = llvm::ConstantInt::get (wide, bits);
		llvm::Value *top = is_signed ? builder.CreateAShr (product, count)
		                             : builder.CreateLShr (product, count);

		builder.CreateRet (builder.CreateTrunc (top, type));
		return llvm::Error::success ();
	}

	/// Gathers the sign bit of each byte lane into the answer's low bits.
	static BuiltinResult byte_mask (MethodLLVMEmitter &emitter,
	                                llvm::IRBuilder<> &builder, MonoMethod *)
	{
		llvm::Value *value = argument (emitter, 0);
		auto *type = llvm::dyn_cast<llvm::FixedVectorType> (value->getType ());
		llvm::Type *answer = return_type (emitter);

		if (type == nullptr || !type->getElementType ()->isIntegerTy (8))
			return std::nullopt;
		if (!answer->isIntegerTy ())
			return std::nullopt;

		llvm::Value *signs = builder.CreateICmpSLT (
			value, llvm::Constant::getNullValue (type));
		llvm::Value *bits = builder.CreateBitCast (
			signs, llvm::Type::getIntNTy (context (emitter),
		                                      type->getNumElements ()));

		builder.CreateRet (builder.CreateZExt (bits, answer));
		return llvm::Error::success ();
	}

	/// Answers each lane with its square root.
	///
	/// Vector4f's body takes each lane through System.Math.Sqrt, which is a
	/// double. Rounding a float's square root through double and back gives the
	/// float result, so one float square root answers the same.
	static BuiltinResult square_root (MethodLLVMEmitter &emitter,
	                                  llvm::IRBuilder<> &builder, MonoMethod *)
	{
		llvm::Value *value = argument (emitter, 0);
		auto *type = llvm::dyn_cast<llvm::FixedVectorType> (value->getType ());

		if (type == nullptr || !type->getElementType ()->isFloatingPointTy ())
			return std::nullopt;

		builder.CreateRet (relax (
			builder.CreateIntrinsic (llvm::Intrinsic::sqrt, { type }, { value })));
		return llvm::Error::success ();
	}

	/// Answers each lane with the reciprocal of its square root.
	///
	/// The managed body divides a double 1.0 by a double square root and rounds
	/// the quotient to float. Dividing at float width instead divides by a
	/// square root that has already been rounded, which is a different answer,
	/// so the double stays.
	static BuiltinResult inverse_square_root (MethodLLVMEmitter &emitter,
	                                          llvm::IRBuilder<> &builder, MonoMethod *)
	{
		llvm::Value *value = argument (emitter, 0);
		auto *type = llvm::dyn_cast<llvm::FixedVectorType> (value->getType ());

		if (type == nullptr || !type->getElementType ()->isFloatTy ())
			return std::nullopt;

		llvm::FixedVectorType *wide = llvm::FixedVectorType::get (
			llvm::Type::getDoubleTy (context (emitter)),
			type->getNumElements ());
		llvm::Value *root = relax (builder.CreateIntrinsic (
			llvm::Intrinsic::sqrt, { wide },
			{ builder.CreateFPExt (value, wide) }));
		llvm::Value *quotient =
			fdiv (builder, llvm::ConstantFP::get (wide, 1.0), root);

		builder.CreateRet (builder.CreateFPTrunc (quotient, type));
		return llvm::Error::success ();
	}

	/// Answers each lane with a float 1.0 divided by it.
	static BuiltinResult reciprocal (MethodLLVMEmitter &emitter,
	                                 llvm::IRBuilder<> &builder, MonoMethod *)
	{
		llvm::Value *value = argument (emitter, 0);
		auto *type = llvm::dyn_cast<llvm::FixedVectorType> (value->getType ());

		if (type == nullptr || !type->getElementType ()->isFloatTy ())
			return std::nullopt;

		builder.CreateRet (
			fdiv (builder, llvm::ConstantFP::get (type, 1.0), value));
		return llvm::Error::success ();
	}

	/// Combines the neighbouring lanes of each argument in turn.
	template <bool adding>
	static BuiltinResult horizontal (MethodLLVMEmitter &emitter,
	                                 llvm::IRBuilder<> &builder, MonoMethod *)
	{
		std::optional<std::pair<llvm::Value *, llvm::Value *>> args =
			operands (emitter);

		if (!args)
			return std::nullopt;

		llvm::FixedVectorType *type = type_of (args->first);

		if (!type->getElementType ()->isFloatingPointTy ())
			return std::nullopt;

		unsigned lanes = type->getNumElements ();
		llvm::SmallVector<int, 8> evens;
		llvm::SmallVector<int, 8> odds;

		for (unsigned i = 0; i < lanes; ++i) {
			evens.push_back ((int) (2 * i));
			odds.push_back ((int) (2 * i + 1));
		}

		llvm::Value *lhs =
			builder.CreateShuffleVector (args->first, args->second, evens);
		llvm::Value *rhs =
			builder.CreateShuffleVector (args->first, args->second, odds);

		builder.CreateRet (adding ? fadd (builder, lhs, rhs)
		                          : fsub (builder, lhs, rhs));
		return llvm::Error::success ();
	}

	/// Subtracts in the even lanes and adds in the odd ones.
	static BuiltinResult add_sub (MethodLLVMEmitter &emitter, llvm::IRBuilder<> &builder,
	                              MonoMethod *)
	{
		std::optional<std::pair<llvm::Value *, llvm::Value *>> args =
			operands (emitter);

		if (!args)
			return std::nullopt;

		llvm::FixedVectorType *type = type_of (args->first);

		if (!type->getElementType ()->isFloatingPointTy ())
			return std::nullopt;

		unsigned lanes = type->getNumElements ();
		llvm::Value *difference = fsub (builder, args->first, args->second);
		llvm::Value *sum = fadd (builder, args->first, args->second);
		llvm::SmallVector<int, 4> mask;

		for (unsigned i = 0; i < lanes; ++i)
			mask.push_back (i % 2 == 0 ? (int) i : (int) (lanes + i));

		builder.CreateRet (builder.CreateShuffleVector (difference, sum, mask));
		return llvm::Error::success ();
	}

	/// Interleaves one half of each argument's lanes.
	template <bool high>
	static BuiltinResult unpack (MethodLLVMEmitter &emitter, llvm::IRBuilder<> &builder,
	                             MonoMethod *)
	{
		std::optional<std::pair<llvm::Value *, llvm::Value *>> args =
			operands (emitter);

		if (!args)
			return std::nullopt;

		unsigned lanes = type_of (args->first)->getNumElements ();

		if (lanes % 2 != 0)
			return std::nullopt;

		unsigned first = high ? lanes / 2 : 0;
		llvm::SmallVector<int, 16> mask;

		for (unsigned i = 0; i < lanes / 2; ++i) {
			mask.push_back ((int) (first + i));
			mask.push_back ((int) (lanes + first + i));
		}

		builder.CreateRet (
			builder.CreateShuffleVector (args->first, args->second, mask));
		return llvm::Error::success ();
	}

	/// Answers with the argument's lanes in the order the row names.
	template <int... selection>
	static BuiltinResult permute (MethodLLVMEmitter &emitter, llvm::IRBuilder<> &builder,
	                              MonoMethod *)
	{
		llvm::Value *value = argument (emitter, 0);
		auto *type = llvm::dyn_cast<llvm::FixedVectorType> (value->getType ());

		if (type == nullptr || type->getNumElements () != sizeof... (selection))
			return std::nullopt;

		static const int mask[] = { selection... };

		builder.CreateRet (builder.CreateShuffleVector (
			value, llvm::ArrayRef<int> (mask, sizeof... (selection))));
		return llvm::Error::success ();
	}

	/**
	 * Narrows both arguments' lanes into one vector, clamping each first.
	 *
	 * Every one of these bodies reads its source through a signed pointer,
	 * whatever the struct's own element is. The clamp is therefore signed on
	 * both arms, and unsigned_range moves only where it clamps to.
	 */
	template <bool unsigned_range>
	static BuiltinResult pack (MethodLLVMEmitter &emitter, llvm::IRBuilder<> &builder,
	                           MonoMethod *)
	{
		std::optional<std::pair<llvm::Value *, llvm::Value *>> args =
			operands (emitter);
		auto *narrow = llvm::dyn_cast<llvm::FixedVectorType> (
			return_type (emitter));

		if (!args || narrow == nullptr)
			return std::nullopt;

		llvm::FixedVectorType *type = type_of (args->first);
		unsigned lanes = type->getNumElements ();
		unsigned bits = type->getScalarSizeInBits () / 2;

		if (!type->getElementType ()->isIntegerTy ())
			return std::nullopt;
		if (narrow->getNumElements () != lanes * 2
		    || narrow->getScalarSizeInBits () != bits)
			return std::nullopt;

		int64_t highest = unsigned_range ? (int64_t) ((1ull << bits) - 1)
		                                 : (int64_t) ((1ull << (bits - 1)) - 1);
		int64_t lowest = unsigned_range ? 0 : -(int64_t) (1ull << (bits - 1));
		llvm::Constant *ceiling =
			llvm::ConstantInt::get (type, (uint64_t) highest, true);
		llvm::Constant *floor =
			llvm::ConstantInt::get (type, (uint64_t) lowest, true);
		auto clamp = [&] (llvm::Value *value) {
			value = builder.CreateIntrinsic (llvm::Intrinsic::smin, { type },
			                                 { value, ceiling });
			value = builder.CreateIntrinsic (llvm::Intrinsic::smax, { type },
			                                 { value, floor });
			return builder.CreateTrunc (value, integer_lanes (type, bits));
		};
		llvm::SmallVector<int, 16> mask;

		for (unsigned i = 0; i < lanes * 2; ++i)
			mask.push_back ((int) i);

		builder.CreateRet (builder.CreateShuffleVector (clamp (args->first),
		                                                clamp (args->second), mask));
		return llvm::Error::success ();
	}

	/// value's first count lanes.
	static llvm::Value *first_lanes (llvm::IRBuilder<> &builder, llvm::Value *value,
	                                 unsigned count)
	{
		if (type_of (value)->getNumElements () == count)
			return value;

		llvm::SmallVector<int, 4> mask;

		for (unsigned i = 0; i < count; ++i)
			mask.push_back ((int) i);

		return builder.CreateShuffleVector (value, mask);
	}

	/// value's lanes, with zeros filling the answer out to count of them.
	///
	/// The mask is the identity: index i past value's own lanes already names
	/// a lane of the zeros standing beside them.
	static llvm::Value *zero_filled (llvm::IRBuilder<> &builder, llvm::Value *value,
	                                 unsigned count)
	{
		llvm::FixedVectorType *type = type_of (value);

		if (type->getNumElements () == count)
			return value;

		llvm::SmallVector<int, 4> mask;

		for (unsigned i = 0; i < count; ++i)
			mask.push_back ((int) i);

		return builder.CreateShuffleVector (value, llvm::Constant::getNullValue (type),
		                                    mask);
	}

	/**
	 * Converts as many lanes as the narrower of the two types holds.
	 *
	 * Every ConvertTo body reads its operand's lanes lowest first. A lane of
	 * the answer it has nothing for gets a zero. One rule therefore covers the
	 * widening bodies and the narrowing ones alike.
	 *
	 * constrained_float_to_int () converts a lane going to an integer, so an
	 * operand out of range gives the same bytes at every tier.
	 */
	template <LaneConvert kind>
	static BuiltinResult convert_lanes (MethodLLVMEmitter &emitter,
	                                    llvm::IRBuilder<> &builder, MonoMethod *method)
	{
		llvm::Value *value = argument (emitter, 0);
		auto *from = llvm::dyn_cast<llvm::FixedVectorType> (value->getType ());
		auto *to = llvm::dyn_cast<llvm::FixedVectorType> (return_type (emitter));
		std::optional<Lane> lane = lane_of (method);

		if (from == nullptr || to == nullptr || !lane)
			return std::nullopt;
		if (to->getElementType ()->isFloatingPointTy ()
		    != (kind == LaneConvert::floating))
			return std::nullopt;
		if (kind != LaneConvert::floating && !lane_is_float (*lane))
			return std::nullopt;

		unsigned lanes =
			std::min (from->getNumElements (), to->getNumElements ());
		llvm::Value *source = first_lanes (builder, value, lanes);
		llvm::FixedVectorType *narrow =
			llvm::FixedVectorType::get (to->getElementType (), lanes);
		llvm::Value *converted = nullptr;

		switch (kind) {
		case LaneConvert::floating:
			converted = lane_is_float (*lane)
			                    ? builder.CreateFPCast (source, narrow)
			            : lane_is_signed (*lane)
			                    ? builder.CreateSIToFP (source, narrow)
			                    : builder.CreateUIToFP (source, narrow);
			break;

		case LaneConvert::rounded: {
			// System.Math.Round takes a double, so a float lane widens
			// before it rounds and the answer comes off the double.
			llvm::FixedVectorType *wide = llvm::FixedVectorType::get (
				llvm::Type::getDoubleTy (context (emitter)), lanes);
			llvm::Value *rounded = builder.CreateIntrinsic (
				llvm::Intrinsic::roundeven, { wide },
				{ builder.CreateFPCast (source, wide) });

			converted = constrained_float_to_int (builder, rounded, narrow, true);
			break;
		}

		case LaneConvert::truncated:
			converted = constrained_float_to_int (builder, source, narrow, true);
			break;
		}

		builder.CreateRet (zero_filled (builder, converted, to->getNumElements ()));
		return llvm::Error::success ();
	}

	/**
	 * Sums the absolute differences of each half into lanes 0 and 4.
	 *
	 * This is not psadbw. The body reads its first operand through a byte
	 * pointer and its second through an sbyte one. A lane of 0xff is therefore
	 * 255 on the left and -1 on the right.
	 *
	 * The lanes the body never assigns keep the zero the answer was made with.
	 */
	static BuiltinResult absolute_differences (MethodLLVMEmitter &emitter,
	                                           llvm::IRBuilder<> &builder, MonoMethod *)
	{
		std::optional<std::pair<llvm::Value *, llvm::Value *>> args =
			operands (emitter);
		auto *to = llvm::dyn_cast<llvm::FixedVectorType> (return_type (emitter));

		if (!args || to == nullptr)
			return std::nullopt;

		llvm::FixedVectorType *type = type_of (args->first);

		if (!type->getElementType ()->isIntegerTy (8) || type->getNumElements () != 16)
			return std::nullopt;
		if (!to->getElementType ()->isIntegerTy (16) || to->getNumElements () != 8)
			return std::nullopt;

		// The widest difference is 383, so llvm.abs never meets the int32 whose
		// negation does not fit and its poison operand stays false.
		unsigned width = type->getNumElements () / 2;
		llvm::FixedVectorType *wide = integer_lanes (type, 32);
		llvm::Value *difference =
			builder.CreateSub (builder.CreateZExt (args->first, wide),
		                           builder.CreateSExt (args->second, wide));
		llvm::Value *absolute = builder.CreateIntrinsic (
			llvm::Intrinsic::abs, { wide }, { difference, builder.getInt1 (false) });
		llvm::Value *answer = llvm::Constant::getNullValue (to);
		llvm::FixedVectorType *half =
			llvm::FixedVectorType::get (builder.getInt32Ty (), width);

		for (unsigned i = 0; i < 2; ++i) {
			llvm::SmallVector<int, 8> mask;

			for (unsigned lane = 0; lane < width; ++lane)
				mask.push_back ((int) (i * width + lane));

			llvm::Value *sum = builder.CreateIntrinsic (
				llvm::Intrinsic::vector_reduce_add, { half },
				{ builder.CreateShuffleVector (absolute, mask) });

			answer = builder.CreateInsertElement (
				answer, builder.CreateTrunc (sum, to->getElementType ()),
				i * (to->getNumElements () / 2));
		}

		builder.CreateRet (answer);
		return llvm::Error::success ();
	}

	/// The lane that field of selector names, offset by base.
	static llvm::Value *selected_lane (llvm::IRBuilder<> &builder, llvm::Value *selector,
	                                   unsigned field, unsigned width, unsigned base)
	{
		llvm::Type *type = selector->getType ();
		llvm::Value *index = builder.CreateAnd (
			builder.CreateLShr (selector,
		                            llvm::ConstantInt::get (type, field * width)),
			llvm::ConstantInt::get (type, (1u << width) - 1));

		if (base == 0)
			return index;

		return builder.CreateAdd (index, llvm::ConstantInt::get (type, base));
	}

	/**
	 * Answers each lane with the one a field of the selector names. The low
	 * half comes from the first operand and the high half from the second.
	 *
	 * The selector is a parameter rather than a constant, so this is a read at
	 * a computed index. That is what the managed body does through its element
	 * pointer. A caller passing a constant leaves one shufflevector once the
	 * body folds into it.
	 *
	 * The field is two bits wide for a four-lane operand and one for a
	 * two-lane one. The mask in front of it keeps every index in range.
	 */
	static BuiltinResult shuffle_pair (MethodLLVMEmitter &emitter,
	                                   llvm::IRBuilder<> &builder, MonoMethod *)
	{
		std::optional<std::pair<llvm::Value *, llvm::Value *>> args =
			operands (emitter);
		llvm::Value *selector = argument (emitter, 2);

		if (!args || !selector->getType ()->isIntegerTy (32))
			return std::nullopt;

		llvm::FixedVectorType *type = type_of (args->first);
		unsigned lanes = type->getNumElements ();

		if (lanes != 2 && lanes != 4)
			return std::nullopt;

		unsigned width = lanes == 4 ? 2u : 1u;
		llvm::Value *answer = llvm::PoisonValue::get (type);

		for (unsigned i = 0; i < lanes; ++i) {
			llvm::Value *from = i < lanes / 2 ? args->first : args->second;

			answer = builder.CreateInsertElement (
				answer,
				builder.CreateExtractElement (
					from,
					selected_lane (builder, selector, i, width, 0)),
				i);
		}

		builder.CreateRet (answer);
		return llvm::Error::success ();
	}

	/// Answers the window's four lanes with the ones the selector names, and
	/// every other lane with the operand's own.
	template <ShuffleWindow window>
	static BuiltinResult shuffle_window (MethodLLVMEmitter &emitter,
	                                     llvm::IRBuilder<> &builder, MonoMethod *)
	{
		llvm::Value *value = argument (emitter, 0);
		llvm::Value *selector = argument (emitter, 1);
		auto *type = llvm::dyn_cast<llvm::FixedVectorType> (value->getType ());

		if (type == nullptr || !selector->getType ()->isIntegerTy (32))
			return std::nullopt;
		if (type->getNumElements () != (window == ShuffleWindow::whole ? 4u : 8u))
			return std::nullopt;

		unsigned base = window == ShuffleWindow::high ? 4u : 0u;
		llvm::Value *answer = window == ShuffleWindow::whole
		                              ? (llvm::Value *) llvm::PoisonValue::get (type)
		                              : value;

		for (unsigned i = 0; i < 4; ++i)
			answer = builder.CreateInsertElement (
				answer,
				builder.CreateExtractElement (
					value,
					selected_lane (builder, selector, i, 2, base)),
				base + i);

		builder.CreateRet (answer);
		return llvm::Error::success ();
	}

	/**
	 * Asks the cache for the line the argument points at.
	 *
	 * The managed body is empty, and these methods exist for a compiler to
	 * recognize rather than for anything the body does. A prefetch reaches no
	 * value the program can read, so the empty body and this one answer alike.
	 *
	 * locality is llvm.prefetch's, where 3 is the whole hierarchy and 0 is
	 * non-temporal, which is the order the four names run in.
	 */
	template <unsigned locality>
	static BuiltinResult prefetch (MethodLLVMEmitter &emitter, llvm::IRBuilder<> &builder,
	                               MonoMethod *)
	{
		llvm::Value *address = argument (emitter, 0);

		if (!address->getType ()->isPointerTy () || !return_type (emitter)->isVoidTy ())
			return std::nullopt;

		builder.CreateIntrinsic (llvm::Intrinsic::prefetch, { address->getType () },
		                         { address, builder.getInt32 (0),
		                           builder.getInt32 (locality), builder.getInt32 (1) });
		builder.CreateRetVoid ();
		return llvm::Error::success ();
	}

};

namespace {

using S = SimdEmitters;

constexpr llvm::Instruction::BinaryOps bit_and = llvm::Instruction::And;
constexpr llvm::Instruction::BinaryOps bit_or = llvm::Instruction::Or;
constexpr llvm::Instruction::BinaryOps bit_xor = llvm::Instruction::Xor;

constexpr llvm::CmpInst::Predicate no_integer_form = llvm::CmpInst::BAD_ICMP_PREDICATE;
constexpr llvm::CmpInst::Predicate no_float_form = llvm::CmpInst::BAD_FCMP_PREDICATE;

constexpr ClassKey vector4f { "Mono.Simd", "Mono.Simd", "Vector4f" };
constexpr ClassKey vector2d { "Mono.Simd", "Mono.Simd", "Vector2d" };
constexpr ClassKey vector4i { "Mono.Simd", "Mono.Simd", "Vector4i" };
constexpr ClassKey vector4ui { "Mono.Simd", "Mono.Simd", "Vector4ui" };
constexpr ClassKey vector2l { "Mono.Simd", "Mono.Simd", "Vector2l" };
constexpr ClassKey vector2ul { "Mono.Simd", "Mono.Simd", "Vector2ul" };
constexpr ClassKey vector8s { "Mono.Simd", "Mono.Simd", "Vector8s" };
constexpr ClassKey vector8us { "Mono.Simd", "Mono.Simd", "Vector8us" };
constexpr ClassKey vector16sb { "Mono.Simd", "Mono.Simd", "Vector16sb" };
constexpr ClassKey vector16b { "Mono.Simd", "Mono.Simd", "Vector16b" };
constexpr ClassKey operations { "Mono.Simd", "Mono.Simd", "VectorOperations" };

/// The ten structs, each of which declares the prefetch surface itself.
constexpr ClassKey structs[] = { vector4f,  vector2d, vector4i,  vector4ui, vector2l,
	                         vector2ul, vector8s, vector8us, vector16sb, vector16b };

// il_agrees is true throughout, because every managed body here computes what
// its row computes and tier 0 goes on running it.
const BuiltinBody simd_table[] = {
	{ vector4f, "op_Addition", "VV", true, simd_lowering, S::binary<S::fadd> },
	{ vector4f, "op_Subtraction", "VV", true, simd_lowering, S::binary<S::fsub> },
	{ vector4f, "op_Multiply", "VV", true, simd_lowering, S::binary<S::fmul> },
	{ vector4f, "op_Multiply", "VS", true, simd_lowering, S::scalar_multiply },
	{ vector4f, "op_Multiply", "SV", true, simd_lowering, S::scalar_multiply },
	{ vector4f, "op_Division", "VV", true, simd_lowering, S::binary<S::fdiv> },
	{ vector4f, "op_BitwiseAnd", "VV", true, simd_lowering, S::bitwise<bit_and> },
	{ vector4f, "op_BitwiseOr", "VV", true, simd_lowering, S::bitwise<bit_or> },
	{ vector4f, "op_ExclusiveOr", "VV", true, simd_lowering, S::bitwise<bit_xor> },
	{ vector4f, "op_Equality", "VV", true, simd_lowering, S::total_compare<true> },
	{ vector4f, "op_Inequality", "VV", true, simd_lowering, S::total_compare<false> },
	{ vector4f, "op_Explicit", "V", true, simd_lowering, S::reinterpret },

	{ vector2d, "op_Addition", "VV", true, simd_lowering, S::binary<S::fadd> },
	{ vector2d, "op_Subtraction", "VV", true, simd_lowering, S::binary<S::fsub> },
	{ vector2d, "op_Multiply", "VV", true, simd_lowering, S::binary<S::fmul> },
	{ vector2d, "op_Division", "VV", true, simd_lowering, S::binary<S::fdiv> },
	{ vector2d, "op_BitwiseAnd", "VV", true, simd_lowering, S::bitwise<bit_and> },
	{ vector2d, "op_BitwiseOr", "VV", true, simd_lowering, S::bitwise<bit_or> },
	{ vector2d, "op_ExclusiveOr", "VV", true, simd_lowering, S::bitwise<bit_xor> },
	{ vector2d, "op_Explicit", "V", true, simd_lowering, S::reinterpret },

	{ vector4i, "op_Addition", "VV", true, simd_lowering, S::binary<S::add> },
	{ vector4i, "op_Subtraction", "VV", true, simd_lowering, S::binary<S::sub> },
	{ vector4i, "op_Multiply", "VV", true, simd_lowering, S::binary<S::mul> },
	{ vector4i, "op_LeftShift", "VS", true, simd_lowering,
	  S::shift<S::Direction::left, true> },
	{ vector4i, "op_RightShift", "VS", true, simd_lowering,
	  S::shift<S::Direction::right, true> },
	{ vector4i, "op_BitwiseAnd", "VV", true, simd_lowering, S::bitwise<bit_and> },
	{ vector4i, "op_BitwiseOr", "VV", true, simd_lowering, S::bitwise<bit_or> },
	{ vector4i, "op_ExclusiveOr", "VV", true, simd_lowering, S::bitwise<bit_xor> },
	{ vector4i, "op_Equality", "VV", true, simd_lowering, S::total_compare<true> },
	{ vector4i, "op_Inequality", "VV", true, simd_lowering, S::total_compare<false> },
	{ vector4i, "op_Explicit", "V", true, simd_lowering, S::reinterpret },

	{ vector4ui, "op_Addition", "VV", true, simd_lowering, S::binary<S::add> },
	{ vector4ui, "op_Subtraction", "VV", true, simd_lowering, S::binary<S::sub> },
	{ vector4ui, "op_Multiply", "VV", true, simd_lowering, S::binary<S::mul> },
	{ vector4ui, "op_LeftShift", "VS", true, simd_lowering,
	  S::shift<S::Direction::left, false> },
	{ vector4ui, "op_RightShift", "VS", true, simd_lowering,
	  S::shift<S::Direction::right, false> },
	{ vector4ui, "op_BitwiseAnd", "VV", true, simd_lowering, S::bitwise<bit_and> },
	{ vector4ui, "op_BitwiseOr", "VV", true, simd_lowering, S::bitwise<bit_or> },
	{ vector4ui, "op_ExclusiveOr", "VV", true, simd_lowering, S::bitwise<bit_xor> },
	{ vector4ui, "op_Equality", "VV", true, simd_lowering, S::total_compare<true> },
	{ vector4ui, "op_Inequality", "VV", true, simd_lowering, S::total_compare<false> },
	{ vector4ui, "op_Explicit", "V", true, simd_lowering, S::reinterpret },

	{ vector2l, "op_Addition", "VV", true, simd_lowering, S::binary<S::add> },
	{ vector2l, "op_Subtraction", "VV", true, simd_lowering, S::binary<S::sub> },
	{ vector2l, "op_LeftShift", "VS", true, simd_lowering,
	  S::shift<S::Direction::left, true> },
	{ vector2l, "op_BitwiseAnd", "VV", true, simd_lowering, S::bitwise<bit_and> },
	{ vector2l, "op_BitwiseOr", "VV", true, simd_lowering, S::bitwise<bit_or> },
	{ vector2l, "op_ExclusiveOr", "VV", true, simd_lowering, S::bitwise<bit_xor> },
	{ vector2l, "op_Explicit", "V", true, simd_lowering, S::reinterpret },

	{ vector2ul, "op_Addition", "VV", true, simd_lowering, S::binary<S::add> },
	{ vector2ul, "op_Subtraction", "VV", true, simd_lowering, S::binary<S::sub> },
	{ vector2ul, "op_LeftShift", "VS", true, simd_lowering,
	  S::shift<S::Direction::left, false> },
	{ vector2ul, "op_RightShift", "VS", true, simd_lowering,
	  S::shift<S::Direction::right, false> },
	{ vector2ul, "op_BitwiseAnd", "VV", true, simd_lowering, S::bitwise<bit_and> },
	{ vector2ul, "op_BitwiseOr", "VV", true, simd_lowering, S::bitwise<bit_or> },
	{ vector2ul, "op_ExclusiveOr", "VV", true, simd_lowering, S::bitwise<bit_xor> },
	{ vector2ul, "op_Explicit", "V", true, simd_lowering, S::reinterpret },

	{ vector8s, "op_Addition", "VV", true, simd_lowering, S::binary<S::add> },
	{ vector8s, "op_Subtraction", "VV", true, simd_lowering, S::binary<S::sub> },
	{ vector8s, "op_Multiply", "VV", true, simd_lowering, S::binary<S::mul> },
	{ vector8s, "op_LeftShift", "VS", true, simd_lowering,
	  S::shift<S::Direction::left, true> },
	{ vector8s, "op_RightShift", "VS", true, simd_lowering,
	  S::shift<S::Direction::right, true> },
	{ vector8s, "op_BitwiseAnd", "VV", true, simd_lowering, S::bitwise<bit_and> },
	{ vector8s, "op_BitwiseOr", "VV", true, simd_lowering, S::bitwise<bit_or> },
	{ vector8s, "op_ExclusiveOr", "VV", true, simd_lowering, S::bitwise<bit_xor> },
	{ vector8s, "op_Equality", "VV", true, simd_lowering, S::total_compare<true> },
	{ vector8s, "op_Inequality", "VV", true, simd_lowering, S::total_compare<false> },
	{ vector8s, "op_Explicit", "V", true, simd_lowering, S::reinterpret },

	{ vector8us, "op_Addition", "VV", true, simd_lowering, S::binary<S::add> },
	{ vector8us, "op_Subtraction", "VV", true, simd_lowering, S::binary<S::sub> },
	{ vector8us, "op_Multiply", "VV", true, simd_lowering, S::binary<S::mul> },
	{ vector8us, "op_LeftShift", "VS", true, simd_lowering,
	  S::shift<S::Direction::left, false> },
	{ vector8us, "op_RightShift", "VS", true, simd_lowering,
	  S::shift<S::Direction::right, false> },
	{ vector8us, "op_BitwiseAnd", "VV", true, simd_lowering, S::bitwise<bit_and> },
	{ vector8us, "op_BitwiseOr", "VV", true, simd_lowering, S::bitwise<bit_or> },
	{ vector8us, "op_ExclusiveOr", "VV", true, simd_lowering, S::bitwise<bit_xor> },
	{ vector8us, "op_Equality", "VV", true, simd_lowering, S::total_compare<true> },
	{ vector8us, "op_Inequality", "VV", true, simd_lowering, S::total_compare<false> },
	{ vector8us, "op_Explicit", "V", true, simd_lowering, S::reinterpret },

	{ vector16sb, "op_Addition", "VV", true, simd_lowering, S::binary<S::add> },
	{ vector16sb, "op_Subtraction", "VV", true, simd_lowering, S::binary<S::sub> },
	{ vector16sb, "op_BitwiseAnd", "VV", true, simd_lowering, S::bitwise<bit_and> },
	{ vector16sb, "op_BitwiseOr", "VV", true, simd_lowering, S::bitwise<bit_or> },
	{ vector16sb, "op_ExclusiveOr", "VV", true, simd_lowering, S::bitwise<bit_xor> },
	{ vector16sb, "op_Equality", "VV", true, simd_lowering, S::total_compare<true> },
	{ vector16sb, "op_Inequality", "VV", true, simd_lowering, S::total_compare<false> },
	{ vector16sb, "op_Explicit", "V", true, simd_lowering, S::reinterpret },

	{ vector16b, "op_Addition", "VV", true, simd_lowering, S::binary<S::add> },
	{ vector16b, "op_Subtraction", "VV", true, simd_lowering, S::binary<S::sub> },
	{ vector16b, "op_BitwiseAnd", "VV", true, simd_lowering, S::bitwise<bit_and> },
	{ vector16b, "op_BitwiseOr", "VV", true, simd_lowering, S::bitwise<bit_or> },
	{ vector16b, "op_ExclusiveOr", "VV", true, simd_lowering, S::bitwise<bit_xor> },
	{ vector16b, "op_Equality", "VV", true, simd_lowering, S::total_compare<true> },
	{ vector16b, "op_Inequality", "VV", true, simd_lowering, S::total_compare<false> },
	{ vector16b, "op_Explicit", "V", true, simd_lowering, S::reinterpret },

	{ operations, "AndNot", "VV", true, simd_lowering, S::and_not },
	{ operations, "ArithmeticRightShift", "VS", true, simd_lowering,
	  S::shift<S::Direction::right, true> },
	{ operations, "LogicalRightShift", "VS", true, simd_lowering,
	  S::shift<S::Direction::right, false> },
	{ operations, "ExtractByteMask", "V", true, simd_lowering, S::byte_mask },

	{ operations, "AddWithSaturation", "VV", true, simd_lowering, S::saturating<true> },
	{ operations, "SubtractWithSaturation", "VV", true, simd_lowering,
	  S::saturating<false> },
	{ operations, "MultiplyStoreHigh", "VV", true, simd_lowering,
	  S::multiply_store_high },
	{ operations, "Average", "VV", true, simd_lowering, S::average },
	{ operations, "Sqrt", "V", true, simd_lowering, S::square_root },
	{ operations, "InvSqrt", "V", true, simd_lowering, S::inverse_square_root },
	{ operations, "Reciprocal", "V", true, simd_lowering, S::reciprocal },
	{ operations, "Max", "VV", true, simd_lowering, S::min_max<true> },
	{ operations, "Min", "VV", true, simd_lowering, S::min_max<false> },

	{ operations, "HorizontalAdd", "VV", true, simd_lowering, S::horizontal<true> },
	{ operations, "HorizontalSub", "VV", true, simd_lowering, S::horizontal<false> },
	{ operations, "AddSub", "VV", true, simd_lowering, S::add_sub },

	{ operations, "CompareEqual", "VV", true, simd_lowering,
	  S::compare_mask<llvm::CmpInst::FCMP_OEQ, llvm::CmpInst::ICMP_EQ> },
	{ operations, "CompareNotEqual", "VV", true, simd_lowering,
	  S::compare_mask<llvm::CmpInst::FCMP_UNE, no_integer_form> },
	{ operations, "CompareLessThan", "VV", true, simd_lowering,
	  S::compare_mask<llvm::CmpInst::FCMP_OLT, no_integer_form> },
	{ operations, "CompareLessEqual", "VV", true, simd_lowering,
	  S::compare_mask<llvm::CmpInst::FCMP_OLE, no_integer_form> },
	{ operations, "CompareNotLessThan", "VV", true, simd_lowering,
	  S::compare_mask<llvm::CmpInst::FCMP_UGE, no_integer_form> },
	{ operations, "CompareNotLessEqual", "VV", true, simd_lowering,
	  S::compare_mask<llvm::CmpInst::FCMP_UGT, no_integer_form> },
	{ operations, "CompareOrdered", "VV", true, simd_lowering,
	  S::compare_mask<llvm::CmpInst::FCMP_ORD, no_integer_form> },
	{ operations, "CompareUnordered", "VV", true, simd_lowering,
	  S::compare_mask<llvm::CmpInst::FCMP_UNO, no_integer_form> },
	{ operations, "CompareGreaterThan", "VV", true, simd_lowering,
	  S::compare_mask<no_float_form, llvm::CmpInst::ICMP_SGT> },

	{ operations, "InterleaveLow", "VV", true, simd_lowering, S::unpack<false> },
	{ operations, "InterleaveHigh", "VV", true, simd_lowering, S::unpack<true> },
	{ operations, "UnpackLow", "VV", true, simd_lowering, S::unpack<false> },
	{ operations, "UnpackHigh", "VV", true, simd_lowering, S::unpack<true> },
	{ operations, "Duplicate", "V", true, simd_lowering, S::permute<0, 0> },
	{ operations, "DuplicateLow", "V", true, simd_lowering, S::permute<0, 0, 2, 2> },
	{ operations, "DuplicateHigh", "V", true, simd_lowering, S::permute<1, 1, 3, 3> },

	// One row for each shuffle shape rather than for each overload. The lane
	// count sets the field width, and the operand's type sets the window.
	{ operations, "Shuffle", "VVS", true, simd_lowering, S::shuffle_pair },
	{ operations, "Shuffle", "VS", true, simd_lowering,
	  S::shuffle_window<S::ShuffleWindow::whole> },
	{ operations, "ShuffleLow", "VS", true, simd_lowering,
	  S::shuffle_window<S::ShuffleWindow::low> },
	{ operations, "ShuffleHigh", "VS", true, simd_lowering,
	  S::shuffle_window<S::ShuffleWindow::high> },

	{ operations, "SumOfAbsoluteDifferences", "VV", true, simd_lowering,
	  S::absolute_differences },

	// ConvertToFloat and ConvertToDouble each cover two source types, and the
	// two rows below them cover the four bodies that answer with an integer.
	{ operations, "ConvertToFloat", "V", true, simd_lowering,
	  S::convert_lanes<S::LaneConvert::floating> },
	{ operations, "ConvertToDouble", "V", true, simd_lowering,
	  S::convert_lanes<S::LaneConvert::floating> },
	{ operations, "ConvertToInt", "V", true, simd_lowering,
	  S::convert_lanes<S::LaneConvert::rounded> },
	{ operations, "ConvertToIntTruncated", "V", true, simd_lowering,
	  S::convert_lanes<S::LaneConvert::truncated> },

	{ operations, "PackWithSignedSaturation", "VV", true, simd_lowering,
	  S::pack<false> },
	{ operations, "SignedPackWithSignedSaturation", "VV", true, simd_lowering,
	  S::pack<false> },
	{ operations, "PackWithUnsignedSaturation", "VV", true, simd_lowering,
	  S::pack<true> },
	{ operations, "SignedPackWithUnsignedSaturation", "VV", true, simd_lowering,
	  S::pack<true> },
};

/// The four prefetches, which every struct above declares under one name each.
///
/// The `ref` overload and the pointer one take the same body, and a pointer is
/// an S either way. One row of each name covers both.
const struct {
	std::string_view name;
	BuiltinResult (*emit) (MethodLLVMEmitter &, llvm::IRBuilder<> &, MonoMethod *);
} prefetches[] = {
	{ "PrefetchTemporalAllCacheLevels", S::prefetch<3> },
	{ "PrefetchTemporal1stLevelCache", S::prefetch<2> },
	{ "PrefetchTemporal2ndLevelCache", S::prefetch<1> },
	{ "PrefetchNonTemporal", S::prefetch<0> },
};

} // namespace

llvm::ArrayRef<BuiltinBody>
simd_bodies ()
{
	static const std::vector<BuiltinBody> rows = [] {
		std::vector<BuiltinBody> made (std::begin (simd_table),
		                               std::end (simd_table));

		for (const ClassKey &klass : structs)
			for (const auto &entry : prefetches)
				made.push_back ({ klass, entry.name, "S", true,
				                  simd_lowering, entry.emit });

		return made;
	} ();

	return rows;
}

} // namespace mono

/**
 * \file
 * \brief The bodies the backend writes for System.Numerics.Vector4.
 *
 * Every row reproduces the managed body lane for lane, for the reason
 * method-to-llvm/simd.cpp gives.
 */

#include "intrinsics.hpp"

#include "../runtime/options.hpp"
#include "method-to-llvm.hpp"
#include "simd-emit.hpp"

#include <llvm/IR/Constant.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/InstrTypes.h>
#include <llvm/IR/Intrinsics.h>
#include <llvm/IR/Type.h>
#include <llvm/IR/Value.h>

namespace mono {

/// The emitters the table below points at, written against SimdEmit.
struct SimdNumericsEmitters : SimdEmit {
	/// The float lanes value travels in, or null where it is not a vector of
	/// floats. An emitter that gets a null answers nothing, which leaves the
	/// managed body to be translated.
	static llvm::FixedVectorType *lanes (llvm::Value *value)
	{
		auto *type = llvm::dyn_cast<llvm::FixedVectorType> (value->getType ());

		if (type == nullptr || !type->getElementType ()->isFloatTy ())
			return nullptr;

		return type;
	}

	template <BinaryOp op>
	static BuiltinResult binary (MethodLLVMEmitter &emitter, llvm::IRBuilder<> &builder,
	                             MonoMethod *)
	{
		llvm::Value *lhs = argument (emitter, 0);
		llvm::Value *rhs = argument (emitter, 1);

		if (lanes (lhs) == nullptr || lhs->getType () != rhs->getType ())
			return std::nullopt;

		builder.CreateRet (op (builder, lhs, rhs));
		return llvm::Error::success ();
	}

	/// Applies op with argument scalar spread over the other argument's lanes,
	/// which is the `new Vector4 (s)` these bodies build before the operator.
	template <BinaryOp op, unsigned scalar>
	static BuiltinResult binary_splat (MethodLLVMEmitter &emitter,
	                                   llvm::IRBuilder<> &builder, MonoMethod *)
	{
		llvm::Value *vector = argument (emitter, scalar == 0 ? 1 : 0);
		llvm::Value *value = argument (emitter, scalar);
		llvm::FixedVectorType *type = lanes (vector);

		if (type == nullptr || value->getType () != type->getElementType ())
			return std::nullopt;

		llvm::Value *spread =
			builder.CreateVectorSplat (type->getNumElements (), value);

		builder.CreateRet (scalar == 0 ? op (builder, spread, vector)
		                               : op (builder, vector, spread));
		return llvm::Error::success ();
	}

	/// Writes the `Zero - value` op_UnaryNegation's managed body computes. An
	/// fneg answers -0.0 for a lane holding +0.0, where this subtraction
	/// answers +0.0.
	static BuiltinResult negate (MethodLLVMEmitter &emitter, llvm::IRBuilder<> &builder,
	                             MonoMethod *)
	{
		llvm::Value *value = argument (emitter, 0);
		llvm::FixedVectorType *type = lanes (value);

		if (type == nullptr)
			return std::nullopt;

		builder.CreateRet (
			fsub (builder, llvm::Constant::getNullValue (type), value));
		return llvm::Error::success ();
	}

	/// Answers `(a <pred> b) ? a : b` in each lane, which is what Min and Max
	/// compute. A NaN in either operand makes that compare false, so the second
	/// operand wins. llvm.minnum and llvm.maxnum answer the other one.
	template <llvm::CmpInst::Predicate pred>
	static BuiltinResult pick (MethodLLVMEmitter &emitter, llvm::IRBuilder<> &builder,
	                           MonoMethod *)
	{
		llvm::Value *lhs = argument (emitter, 0);
		llvm::Value *rhs = argument (emitter, 1);

		if (lanes (lhs) == nullptr || lhs->getType () != rhs->getType ())
			return std::nullopt;

		builder.CreateRet (
			builder.CreateSelect (builder.CreateFCmp (pred, lhs, rhs), lhs, rhs));
		return llvm::Error::success ();
	}

	/// Applies intrinsic id to every lane at once, which is the MathF call the
	/// managed body makes on each field.
	template <llvm::Intrinsic::ID id>
	static BuiltinResult lanewise (MethodLLVMEmitter &emitter, llvm::IRBuilder<> &builder,
	                               MonoMethod *)
	{
		llvm::Value *value = argument (emitter, 0);
		llvm::FixedVectorType *type = lanes (value);

		if (type == nullptr)
			return std::nullopt;

		builder.CreateRet (relax (
			builder.CreateIntrinsic (id, { type }, { value })));
		return llvm::Error::success ();
	}

	/// Multiplies the lanes pairwise and adds the products lowest lane first,
	/// which is the association Dot's managed body has. llvm.vector.reduce.fadd
	/// from +0.0 answers +0.0 where these products sum to -0.0.
	static BuiltinResult dot (MethodLLVMEmitter &emitter, llvm::IRBuilder<> &builder,
	                          MonoMethod *)
	{
		llvm::Value *lhs = argument (emitter, 0);
		llvm::Value *rhs = argument (emitter, 1);
		llvm::FixedVectorType *type = lanes (lhs);

		if (type == nullptr || lhs->getType () != rhs->getType ())
			return std::nullopt;

		llvm::Value *sum = nullptr;

		for (unsigned lane = 0; lane < type->getNumElements (); ++lane) {
			llvm::Value *left = builder.CreateExtractElement (lhs, lane);
			llvm::Value *right = builder.CreateExtractElement (rhs, lane);
			llvm::Value *product = fmul (builder, left, right);

			sum = sum == nullptr ? product : fadd (builder, sum, product);
		}

		builder.CreateRet (sum);
		return llvm::Error::success ();
	}

	/// Ands the lane compares, which is what Equals () computes and
	/// op_Equality's managed body calls. The lanes are floats, so comparing
	/// all of them cannot differ from that body's short circuit.
	template <bool negated>
	static BuiltinResult equality (MethodLLVMEmitter &emitter, llvm::IRBuilder<> &builder,
	                               MonoMethod *)
	{
		llvm::Value *lhs = argument (emitter, 0);
		llvm::Value *rhs = argument (emitter, 1);
		llvm::FixedVectorType *type = lanes (lhs);
		llvm::Type *answer = return_type (emitter);

		if (type == nullptr || lhs->getType () != rhs->getType ()
		    || !answer->isIntegerTy ())
			return std::nullopt;

		llvm::Value *equal = builder.CreateFCmpOEQ (lhs, rhs);
		llvm::Value *all = nullptr;

		for (unsigned lane = 0; lane < type->getNumElements (); ++lane) {
			llvm::Value *one = builder.CreateExtractElement (equal, lane);

			all = all == nullptr ? one : builder.CreateAnd (all, one);
		}

		if (negated)
			all = builder.CreateNot (all);

		builder.CreateRet (builder.CreateZExtOrTrunc (all, answer));
		return llvm::Error::success ();
	}
};

namespace {

const ClassKey vector4 = { "System.Numerics", "System.Numerics", "Vector4" };

// The named twin beside each operator forwards to that operator, so its row
// names the operator's own emitter rather than one of its own.
const BuiltinBody simd_numerics_table[] = {
	// `new Vector4 (left.X + right.X, ...)`.
	{ vector4, "op_Addition", "VV", true, simd_lowering,
	  SimdNumericsEmitters::binary<SimdNumericsEmitters::fadd> },
	{ vector4, "Add", "VV", true, simd_lowering,
	  SimdNumericsEmitters::binary<SimdNumericsEmitters::fadd> },

	// `new Vector4 (left.X - right.X, ...)`.
	{ vector4, "op_Subtraction", "VV", true, simd_lowering,
	  SimdNumericsEmitters::binary<SimdNumericsEmitters::fsub> },
	{ vector4, "Subtract", "VV", true, simd_lowering,
	  SimdNumericsEmitters::binary<SimdNumericsEmitters::fsub> },

	// `new Vector4 (left.X * right.X, ...)`.
	{ vector4, "op_Multiply", "VV", true, simd_lowering,
	  SimdNumericsEmitters::binary<SimdNumericsEmitters::fmul> },
	{ vector4, "Multiply", "VV", true, simd_lowering,
	  SimdNumericsEmitters::binary<SimdNumericsEmitters::fmul> },

	// `left * new Vector4 (right)`.
	{ vector4, "op_Multiply", "VS", true, simd_lowering,
	  SimdNumericsEmitters::binary_splat<SimdNumericsEmitters::fmul, 1> },
	{ vector4, "Multiply", "VS", true, simd_lowering,
	  SimdNumericsEmitters::binary_splat<SimdNumericsEmitters::fmul, 1> },

	// `new Vector4 (left) * right`.
	{ vector4, "op_Multiply", "SV", true, simd_lowering,
	  SimdNumericsEmitters::binary_splat<SimdNumericsEmitters::fmul, 0> },
	{ vector4, "Multiply", "SV", true, simd_lowering,
	  SimdNumericsEmitters::binary_splat<SimdNumericsEmitters::fmul, 0> },

	// `new Vector4 (left.X / right.X, ...)`.
	{ vector4, "op_Division", "VV", true, simd_lowering,
	  SimdNumericsEmitters::binary<SimdNumericsEmitters::fdiv> },
	{ vector4, "Divide", "VV", true, simd_lowering,
	  SimdNumericsEmitters::binary<SimdNumericsEmitters::fdiv> },

	// `value1 / new Vector4 (value2)`, which divides each lane by the scalar
	// rather than multiplying by its reciprocal.
	{ vector4, "op_Division", "VS", true, simd_lowering,
	  SimdNumericsEmitters::binary_splat<SimdNumericsEmitters::fdiv, 1> },
	{ vector4, "Divide", "VS", true, simd_lowering,
	  SimdNumericsEmitters::binary_splat<SimdNumericsEmitters::fdiv, 1> },

	{ vector4, "op_UnaryNegation", "V", true, simd_lowering,
	  SimdNumericsEmitters::negate },
	{ vector4, "Negate", "V", true, simd_lowering, SimdNumericsEmitters::negate },

	{ vector4, "Min", "VV", true, simd_lowering,
	  SimdNumericsEmitters::pick<llvm::CmpInst::FCMP_OLT> },
	{ vector4, "Max", "VV", true, simd_lowering,
	  SimdNumericsEmitters::pick<llvm::CmpInst::FCMP_OGT> },

	// `new Vector4 (MathF.Abs (value.X), ...)`, and MathF.Sqrt for the row
	// below. method-to-llvm/math.cpp lowers each of those two calls to this
	// same intrinsic on a scalar.
	{ vector4, "Abs", "V", true, simd_lowering,
	  SimdNumericsEmitters::lanewise<llvm::Intrinsic::fabs> },
	{ vector4, "SquareRoot", "V", true, simd_lowering,
	  SimdNumericsEmitters::lanewise<llvm::Intrinsic::sqrt> },

	{ vector4, "Dot", "VV", true, simd_lowering, SimdNumericsEmitters::dot },

	{ vector4, "op_Equality", "VV", true, simd_lowering,
	  SimdNumericsEmitters::equality<false> },
	// `!(left == right)`.
	{ vector4, "op_Inequality", "VV", true, simd_lowering,
	  SimdNumericsEmitters::equality<true> },
};

} // namespace

llvm::ArrayRef<BuiltinBody>
simd_numerics_bodies ()
{
	return simd_numerics_table;
}

} // namespace mono

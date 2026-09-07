/**
 * \file
 * \brief The bodies the backend writes for System.Numerics.Vector<T>.
 *
 * Every row reproduces the managed body lane for lane, for the reason
 * method-to-llvm/simd.cpp gives. `Vector.IsHardwareAccelerated` answers false
 * unconditionally, so a body that tests it always runs the arm behind `else`. A
 * body with no such test has only one arm to run in the first place. That
 * covers an unconditional conversion, a reinterpret cast, and a forward into a
 * method that does test it.
 *
 * One row covers every instantiation. The lane type comes off the class's own
 * type argument, and a T no row lowers keeps its managed body.
 */

#include "intrinsics.hpp"

#include "../runtime/options.hpp"
#include "float-convert.hpp"
#include "method-to-llvm.hpp"
#include "simd-emit.hpp"

#include "mono/metadata/class-internals.h"

#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Intrinsics.h>
#include <llvm/IR/Type.h>
#include <llvm/IR/Value.h>

namespace mono {

namespace {

/// What one lane holds, which picks a row's arithmetic and the signedness of
/// its compares.
enum class Lane {
	signed_integer,
	unsigned_integer,
	floating,
	/// A T no row lowers.
	other,
};

Lane
lane_of_class (MonoClass *klass)
{
	if (klass == nullptr || !mono_class_is_ginst (klass))
		return Lane::other;

	MonoGenericInst *inst = mono_class_get_generic_class (klass)->context.class_inst;

	if (inst == nullptr || inst->type_argc < 1)
		return Lane::other;

	switch (inst->type_argv[0]->type) {
	case MONO_TYPE_I1:
	case MONO_TYPE_I2:
	case MONO_TYPE_I4:
	case MONO_TYPE_I8:
		return Lane::signed_integer;
	case MONO_TYPE_U1:
	case MONO_TYPE_U2:
	case MONO_TYPE_U4:
	case MONO_TYPE_U8:
		return Lane::unsigned_integer;
	case MONO_TYPE_R4:
	case MONO_TYPE_R8:
		return Lane::floating;
	default:
		return Lane::other;
	}
}

Lane
lane_of (MonoMethod *method)
{
	return lane_of_class (method->klass);
}

/**
 * The lane of the Vector<T> parameter index names.
 *
 * These conversions are declared on the non-generic Vector, so lane_of () reads
 * a class with no type argument and answers other for all of them.
 *
 * Nothing else tells two of them apart. ConvertToSingle (Vector<int>) and
 * ConvertToSingle (Vector<uint>) both take <4 x i32> and answer <4 x float>, so
 * their LLVM types are identical. The registry matches a name and a V or an S
 * per parameter, which is identical too. A row reading the types alone would
 * pick one signedness for both. Above int.MaxValue, where sitofp and uitofp
 * part company, that is a wrong answer rather than a slow one.
 */
Lane
lane_of_parameter (MonoMethod *method, int index)
{
	MonoMethodSignature *sig = mono_method_signature_internal (method);

	if (sig == nullptr || index >= sig->param_count)
		return Lane::other;

	return lane_of_class (mono_class_from_mono_type_internal (sig->params[index]));
}

/// The arithmetic a row applies to its two operands.
enum class Arithmetic {
	add,
	subtract,
	multiply,
};

/// The comparison a row applies to its two operands.
enum class Comparison {
	equal,
	less,
	less_or_equal,
	greater,
	greater_or_equal,
};

} // namespace

/// The emitters the table below points at, written against SimdEmit.
struct SimdVectorTEmitters : SimdEmit {
	/// The vector type parameter i arrived as, or null where it arrived as
	/// something else.
	static llvm::FixedVectorType *vector_argument (MethodLLVMEmitter &emitter, unsigned i)
	{
		return llvm::dyn_cast<llvm::FixedVectorType> (argument (emitter, i)->getType ());
	}

	/// The vector type the first count parameters all arrived as, or null
	/// where they did not.
	static llvm::FixedVectorType *vector_arguments (MethodLLVMEmitter &emitter,
	                                                unsigned count)
	{
		llvm::FixedVectorType *type = vector_argument (emitter, 0);

		if (type == nullptr)
			return nullptr;

		for (unsigned i = 1; i < count; ++i)
			if (vector_argument (emitter, i) != type)
				return nullptr;

		return type;
	}

	static unsigned bit_width (llvm::FixedVectorType *type)
	{
		return type->getScalarSizeInBits () * type->getNumElements ();
	}

	/// The vector of integers of type's own width and lane count.
	static llvm::FixedVectorType *integer_form (llvm::FixedVectorType *type)
	{
		llvm::Type *element = type->getElementType ();

		return llvm::FixedVectorType::get (
			llvm::Type::getIntNTy (element->getContext (),
			                       element->getScalarSizeInBits ()),
			type->getNumElements ());
	}

	/// value as a vector of integers, which is what a bitwise row operates on.
	/// The managed body reads the register's two Int64 fields whatever T is, so
	/// a float lane is the bit pattern here.
	static llvm::Value *as_integers (llvm::IRBuilder<> &builder, llvm::Value *value)
	{
		auto *type = llvm::cast<llvm::FixedVectorType> (value->getType ());

		return builder.CreateBitCast (value, integer_form (type));
	}

	static llvm::Value *compare (llvm::IRBuilder<> &builder, Lane lane, Comparison op,
	                             llvm::Value *lhs, llvm::Value *rhs)
	{
		if (lane == Lane::floating) {
			switch (op) {
			case Comparison::equal:
				return builder.CreateFCmpOEQ (lhs, rhs);
			case Comparison::less:
				return builder.CreateFCmpOLT (lhs, rhs);
			case Comparison::less_or_equal:
				return builder.CreateFCmpOLE (lhs, rhs);
			case Comparison::greater:
				return builder.CreateFCmpOGT (lhs, rhs);
			case Comparison::greater_or_equal:
				return builder.CreateFCmpOGE (lhs, rhs);
			}
		}

		bool sign = lane == Lane::signed_integer;

		switch (op) {
		case Comparison::equal:
			return builder.CreateICmpEQ (lhs, rhs);
		case Comparison::less:
			return sign ? builder.CreateICmpSLT (lhs, rhs)
			            : builder.CreateICmpULT (lhs, rhs);
		case Comparison::less_or_equal:
			return sign ? builder.CreateICmpSLE (lhs, rhs)
			            : builder.CreateICmpULE (lhs, rhs);
		case Comparison::greater:
			return sign ? builder.CreateICmpSGT (lhs, rhs)
			            : builder.CreateICmpUGT (lhs, rhs);
		case Comparison::greater_or_equal:
			return sign ? builder.CreateICmpSGE (lhs, rhs)
			            : builder.CreateICmpUGE (lhs, rhs);
		}

		return nullptr;
	}

	template <Arithmetic op>
	static BuiltinResult arithmetic (MethodLLVMEmitter &emitter,
	                                 llvm::IRBuilder<> &builder, MonoMethod *method)
	{
		Lane lane = lane_of (method);

		if (vector_arguments (emitter, 2) == nullptr || lane == Lane::other)
			return std::nullopt;

		builder.CreateRet (apply<op> (builder, lane, argument (emitter, 0),
		                              argument (emitter, 1)));
		return llvm::Error::success ();
	}

	/// A vector multiplied by a splat of the scalar parameter, whichever
	/// position each of the two is in.
	template <unsigned vector_index>
	static BuiltinResult scale (MethodLLVMEmitter &emitter, llvm::IRBuilder<> &builder,
	                            MonoMethod *method)
	{
		Lane lane = lane_of (method);
		llvm::FixedVectorType *type = vector_argument (emitter, vector_index);
		llvm::Value *scalar = argument (emitter, 1 - vector_index);

		if (type == nullptr || lane == Lane::other
		    || scalar->getType () != type->getElementType ())
			return std::nullopt;

		llvm::Value *splat = builder.CreateVectorSplat (type->getNumElements (), scalar);

		builder.CreateRet (apply<Arithmetic::multiply> (
			builder, lane, argument (emitter, vector_index), splat));
		return llvm::Error::success ();
	}

	/// A lane-wise divide, for the float lanes alone.
	///
	/// The managed body divides each lane with the CIL div, which throws
	/// DivideByZeroException on a zero divisor and OverflowException on
	/// MinValue over -1. LLVM's sdiv and udiv make both undefined, so an
	/// integer T keeps its managed body.
	static BuiltinResult divide (MethodLLVMEmitter &emitter, llvm::IRBuilder<> &builder,
	                             MonoMethod *method)
	{
		if (vector_arguments (emitter, 2) == nullptr
		    || lane_of (method) != Lane::floating)
			return std::nullopt;

		builder.CreateRet (fdiv (builder, argument (emitter, 0),
		                         argument (emitter, 1)));
		return llvm::Error::success ();
	}

	/// Zero minus the operand, which is what `-value` forwards to.
	///
	/// A float lane subtracts from +0.0 rather than flipping the sign bit, so
	/// negating +0.0 answers +0.0 where fneg would answer -0.0.
	static BuiltinResult negate (MethodLLVMEmitter &emitter, llvm::IRBuilder<> &builder,
	                             MonoMethod *method)
	{
		llvm::FixedVectorType *type = vector_argument (emitter, 0);
		Lane lane = lane_of (method);

		if (type == nullptr || lane == Lane::other)
			return std::nullopt;

		llvm::Value *zero = llvm::Constant::getNullValue (type);

		builder.CreateRet (apply<Arithmetic::subtract> (builder, lane, zero,
		                                                argument (emitter, 0)));
		return llvm::Error::success ();
	}

	template <llvm::Instruction::BinaryOps op>
	static BuiltinResult bitwise (MethodLLVMEmitter &emitter, llvm::IRBuilder<> &builder,
	                              MonoMethod *)
	{
		llvm::FixedVectorType *type = vector_arguments (emitter, 2);

		if (type == nullptr)
			return std::nullopt;

		llvm::Value *combined =
			builder.CreateBinOp (op, as_integers (builder, argument (emitter, 0)),
			                     as_integers (builder, argument (emitter, 1)));

		builder.CreateRet (builder.CreateBitCast (combined, type));
		return llvm::Error::success ();
	}

	/// The operand exclusive-ored with AllOnes, which is what `~value` forwards
	/// to. Every T's all-bits-set constant is 0xff repeated (ConstantHelper).
	static BuiltinResult complement (MethodLLVMEmitter &emitter,
	                                 llvm::IRBuilder<> &builder, MonoMethod *)
	{
		llvm::FixedVectorType *type = vector_argument (emitter, 0);

		if (type == nullptr)
			return std::nullopt;

		llvm::Value *flipped =
			builder.CreateNot (as_integers (builder, argument (emitter, 0)));

		builder.CreateRet (builder.CreateBitCast (flipped, type));
		return llvm::Error::success ();
	}

	/// A lane-wise compare, widened to the all-bits-set or zero mask the
	/// managed body writes into each lane.
	template <Comparison op>
	static BuiltinResult mask (MethodLLVMEmitter &emitter, llvm::IRBuilder<> &builder,
	                           MonoMethod *method)
	{
		llvm::FixedVectorType *type = vector_arguments (emitter, 2);
		Lane lane = lane_of (method);

		if (type == nullptr || lane == Lane::other)
			return std::nullopt;

		llvm::Value *lanes = compare (builder, lane, op, argument (emitter, 0),
		                              argument (emitter, 1));

		builder.CreateRet (builder.CreateBitCast (
			builder.CreateSExt (lanes, integer_form (type)), type));
		return llvm::Error::success ();
	}

	/// Whether every lane compares equal, which is what `left == right`
	/// answers through Equals (Vector<T>).
	template <bool negated>
	static BuiltinResult all_lanes_equal (MethodLLVMEmitter &emitter,
	                                      llvm::IRBuilder<> &builder, MonoMethod *method)
	{
		llvm::FixedVectorType *type = vector_arguments (emitter, 2);
		Lane lane = lane_of (method);
		llvm::Type *answer = return_type (emitter);

		if (type == nullptr || lane == Lane::other || !answer->isIntegerTy ())
			return std::nullopt;

		llvm::Value *lanes = compare (builder, lane, Comparison::equal,
		                              argument (emitter, 0), argument (emitter, 1));
		llvm::Value *all = builder.CreateExtractElement (lanes, uint64_t (0));

		for (unsigned i = 1; i < type->getNumElements (); ++i)
			all = builder.CreateAnd (all,
			                         builder.CreateExtractElement (lanes, i));

		if (negated)
			all = builder.CreateNot (all);

		builder.CreateRet (builder.CreateZExtOrBitCast (all, answer));
		return llvm::Error::success ();
	}

	/// The operand's 16 bytes under another lane type, which is what the
	/// private Vector (ref Register) constructor copies.
	static BuiltinResult reinterpret (MethodLLVMEmitter &emitter,
	                                  llvm::IRBuilder<> &builder, MonoMethod *)
	{
		llvm::FixedVectorType *from = vector_argument (emitter, 0);
		auto *to = llvm::dyn_cast<llvm::FixedVectorType> (
			return_type (emitter));

		if (from == nullptr || to == nullptr || bit_width (from) != bit_width (to))
			return std::nullopt;

		builder.CreateRet (builder.CreateBitCast (argument (emitter, 0), to));
		return llvm::Error::success ();
	}

	/// (left & condition) | (right & ~condition), which is what the managed
	/// body builds out of & and Vector.AndNot.
	static BuiltinResult conditional_select (MethodLLVMEmitter &emitter,
	                                         llvm::IRBuilder<> &builder, MonoMethod *)
	{
		llvm::FixedVectorType *type = vector_arguments (emitter, 3);

		if (type == nullptr)
			return std::nullopt;

		llvm::Value *condition = as_integers (builder, argument (emitter, 0));
		llvm::Value *taken =
			builder.CreateAnd (as_integers (builder, argument (emitter, 1)),
			                   condition);
		llvm::Value *left =
			builder.CreateAnd (as_integers (builder, argument (emitter, 2)),
			                   builder.CreateNot (condition));

		builder.CreateRet (builder.CreateBitCast (builder.CreateOr (taken, left), type));
		return llvm::Error::success ();
	}

	/// Math.Abs of each lane.
	///
	/// A signed integer T is left on its managed body, because Math.Abs throws
	/// OverflowException for MinValue and no arithmetic here raises.
	static BuiltinResult absolute (MethodLLVMEmitter &emitter, llvm::IRBuilder<> &builder,
	                               MonoMethod *method)
	{
		Lane lane = lane_of (method);

		if (vector_argument (emitter, 0) == nullptr)
			return std::nullopt;

		if (lane == Lane::unsigned_integer) {
			builder.CreateRet (argument (emitter, 0));
			return llvm::Error::success ();
		}
		if (lane != Lane::floating)
			return std::nullopt;

		builder.CreateRet (relax (builder.CreateUnaryIntrinsic (
			llvm::Intrinsic::fabs, argument (emitter, 0))));
		return llvm::Error::success ();
	}

	/// `left < right ? left : right` for Min, and the same with `>` for Max.
	///
	/// The compare rather than llvm.minnum: a NaN operand makes the compare
	/// false, so the managed body answers right whichever side the NaN is on.
	template <Comparison op>
	static BuiltinResult extremum (MethodLLVMEmitter &emitter, llvm::IRBuilder<> &builder,
	                               MonoMethod *method)
	{
		Lane lane = lane_of (method);

		if (vector_arguments (emitter, 2) == nullptr || lane == Lane::other)
			return std::nullopt;

		llvm::Value *lhs = argument (emitter, 0);
		llvm::Value *rhs = argument (emitter, 1);

		builder.CreateRet (
			builder.CreateSelect (compare (builder, lane, op, lhs, rhs), lhs, rhs));
		return llvm::Error::success ();
	}

	/// Math.Sqrt of each lane.
	///
	/// Math.Sqrt takes a double, so a float lane widens, takes the root and
	/// rounds back. InstCombine is what shrinks that to a float root.
	static BuiltinResult square_root (MethodLLVMEmitter &emitter,
	                                  llvm::IRBuilder<> &builder, MonoMethod *method)
	{
		llvm::FixedVectorType *type = vector_argument (emitter, 0);

		if (type == nullptr || lane_of (method) != Lane::floating)
			return std::nullopt;

		llvm::Value *value = argument (emitter, 0);
		llvm::Type *element = type->getElementType ();
		llvm::FixedVectorType *widened = llvm::FixedVectorType::get (
			llvm::Type::getDoubleTy (element->getContext ()),
			type->getNumElements ());

		if (element->isFloatTy ())
			value = builder.CreateFPExt (value, widened);

		llvm::Value *root =
			relax (builder.CreateUnaryIntrinsic (llvm::Intrinsic::sqrt, value));

		if (element->isFloatTy ())
			root = builder.CreateFPTrunc (root, type);

		builder.CreateRet (root);
		return llvm::Error::success ();
	}

	/// The lane products summed from zero, lane by lane in the order the
	/// managed body adds them.
	static BuiltinResult dot_product (MethodLLVMEmitter &emitter,
	                                  llvm::IRBuilder<> &builder, MonoMethod *method)
	{
		llvm::FixedVectorType *type = vector_arguments (emitter, 2);
		Lane lane = lane_of (method);

		if (type == nullptr || lane == Lane::other
		    || return_type (emitter) != type->getElementType ())
			return std::nullopt;

		llvm::Value *lhs = argument (emitter, 0);
		llvm::Value *rhs = argument (emitter, 1);
		llvm::Value *sum = llvm::Constant::getNullValue (type->getElementType ());

		for (unsigned i = 0; i < type->getNumElements (); ++i) {
			llvm::Value *product = apply<Arithmetic::multiply> (
				builder, lane, builder.CreateExtractElement (lhs, i),
				builder.CreateExtractElement (rhs, i));

			sum = apply<Arithmetic::add> (builder, lane, sum, product);
		}

		builder.CreateRet (sum);
		return llvm::Error::success ();
	}

	/// Each lane converted to the answer's own float type.
	static BuiltinResult to_floating (MethodLLVMEmitter &emitter, llvm::IRBuilder<> &builder,
	                                  MonoMethod *method)
	{
		llvm::FixedVectorType *from = vector_argument (emitter, 0);
		auto *to = llvm::dyn_cast<llvm::FixedVectorType> (return_type (emitter));
		Lane lane = lane_of_parameter (method, 0);

		if (from == nullptr || to == nullptr)
			return std::nullopt;
		if (!to->getElementType ()->isFloatingPointTy ()
		    || from->getNumElements () != to->getNumElements ())
			return std::nullopt;
		if (lane != Lane::signed_integer && lane != Lane::unsigned_integer)
			return std::nullopt;

		llvm::Value *value = argument (emitter, 0);

		builder.CreateRet (lane == Lane::signed_integer
		                           ? builder.CreateSIToFP (value, to)
		                           : builder.CreateUIToFP (value, to));
		return llvm::Error::success ();
	}

	/// Each lane truncated toward zero into the answer's own integer type.
	///
	/// The managed body casts inside `unchecked`, so an operand outside the
	/// answer's range gives whatever the conversion instruction leaves. The
	/// three helpers below are what make that the same value at every tier.
	template <bool is_signed>
	static BuiltinResult to_integer (MethodLLVMEmitter &emitter, llvm::IRBuilder<> &builder,
	                                 MonoMethod *)
	{
		llvm::FixedVectorType *from = vector_argument (emitter, 0);
		auto *to = llvm::dyn_cast<llvm::FixedVectorType> (return_type (emitter));

		if (from == nullptr || to == nullptr)
			return std::nullopt;
		if (!from->getElementType ()->isFloatingPointTy ()
		    || !to->getElementType ()->isIntegerTy ()
		    || from->getNumElements () != to->getNumElements ())
			return std::nullopt;

		llvm::Value *value = argument (emitter, 0);
		llvm::Value *converted;

		if (is_signed)
			converted = constrained_float_to_int (builder, value, to, true);
		else if (to->getScalarSizeInBits () == 64)
			converted = float_to_uint64 (builder, value, to);
		else
			converted = float_to_uint32_or_narrower (builder, value, to);

		builder.CreateRet (converted);
		return llvm::Error::success ();
	}

	/// Both operands' lanes cast into the answer's own element, the first
	/// operand's lanes first.
	static BuiltinResult narrow (MethodLLVMEmitter &emitter, llvm::IRBuilder<> &builder,
	                             MonoMethod *)
	{
		llvm::FixedVectorType *from = vector_arguments (emitter, 2);
		auto *to = llvm::dyn_cast<llvm::FixedVectorType> (return_type (emitter));

		if (from == nullptr || to == nullptr)
			return std::nullopt;
		if (to->getNumElements () != from->getNumElements () * 2
		    || to->getScalarSizeInBits () * 2 != from->getScalarSizeInBits ())
			return std::nullopt;

		llvm::FixedVectorType *half = llvm::FixedVectorType::get (
			to->getElementType (), from->getNumElements ());
		llvm::SmallVector<int, 16> mask;

		for (unsigned i = 0; i < to->getNumElements (); ++i)
			mask.push_back ((int) i);

		builder.CreateRet (builder.CreateShuffleVector (
			cast_lanes (builder, argument (emitter, 0), half),
			cast_lanes (builder, argument (emitter, 1), half), mask));
		return llvm::Error::success ();
	}

	/// The operand's two halves cast into the wider lane, written through the
	/// two byrefs behind it.
	static BuiltinResult widen (MethodLLVMEmitter &emitter, llvm::IRBuilder<> &builder,
	                            MonoMethod *method)
	{
		llvm::FixedVectorType *from = vector_argument (emitter, 0);
		Lane lane = lane_of_parameter (method, 0);

		if (from == nullptr || !return_type (emitter)->isVoidTy ())
			return std::nullopt;
		if (from->getNumElements () % 2 != 0 || lane == Lane::other)
			return std::nullopt;

		llvm::Type *element = from->getElementType ();
		llvm::Type *wider =
			element->isFloatTy ()
				? llvm::Type::getDoubleTy (element->getContext ())
			: element->isIntegerTy ()
				? llvm::Type::getIntNTy (element->getContext (),
			                                 element->getScalarSizeInBits () * 2)
				: nullptr;

		if (wider == nullptr)
			return std::nullopt;

		unsigned half = from->getNumElements () / 2;
		llvm::FixedVectorType *to = llvm::FixedVectorType::get (wider, half);
		llvm::Value *value = argument (emitter, 0);

		for (unsigned i = 0; i < 2; ++i) {
			llvm::SmallVector<int, 8> mask;

			for (unsigned j = 0; j < half; ++j)
				mask.push_back ((int) (i * half + j));

			llvm::Value *lanes = builder.CreateShuffleVector (value, mask);

			// A Vector<T> is aligned well past its element, so the
			// element's own alignment is a floor rather than a claim.
			builder.CreateAlignedStore (
				widen_lanes (builder, lanes, to, lane),
				argument (emitter, 1 + i),
				llvm::Align (wider->getScalarSizeInBits () / 8));
		}

		builder.CreateRetVoid ();
		return llvm::Error::success ();
	}

private:
	/// value with each lane cast into to's narrower element.
	static llvm::Value *cast_lanes (llvm::IRBuilder<> &builder, llvm::Value *value,
	                                llvm::FixedVectorType *to)
	{
		if (to->getElementType ()->isFloatingPointTy ())
			return builder.CreateFPTrunc (value, to);

		return builder.CreateTrunc (value, to);
	}

	/// value with each lane cast into to's wider element.
	static llvm::Value *widen_lanes (llvm::IRBuilder<> &builder, llvm::Value *value,
	                                 llvm::FixedVectorType *to, Lane lane)
	{
		if (lane == Lane::floating)
			return builder.CreateFPExt (value, to);
		if (lane == Lane::signed_integer)
			return builder.CreateSExt (value, to);

		return builder.CreateZExt (value, to);
	}

	/// op applied to lhs and rhs in lane's own arithmetic.
	template <Arithmetic op>
	static llvm::Value *apply (llvm::IRBuilder<> &builder, Lane lane, llvm::Value *lhs,
	                           llvm::Value *rhs)
	{
		bool floating = lane == Lane::floating;

		switch (op) {
		case Arithmetic::add:
			return floating ? fadd (builder, lhs, rhs)
			                : builder.CreateAdd (lhs, rhs);
		case Arithmetic::subtract:
			return floating ? fsub (builder, lhs, rhs)
			                : builder.CreateSub (lhs, rhs);
		case Arithmetic::multiply:
			return floating ? fmul (builder, lhs, rhs)
			                : builder.CreateMul (lhs, rhs);
		}

		return nullptr;
	}
};

namespace {

/// Vector`1 is declared in corlib. System.Numerics.Vectors only forwards it.
constexpr ClassKey vector_t = { nullptr, "System.Numerics", "Vector`1" };

/// The non-generic Vector beside it, which declares the conversions.
constexpr ClassKey vector_statics = { nullptr, "System.Numerics", "Vector" };

const BuiltinBody simd_vector_t_table[] = {
	{ vector_t, "op_Addition", "VV", true, simd_lowering,
	  SimdVectorTEmitters::arithmetic<Arithmetic::add> },
	{ vector_t, "op_Subtraction", "VV", true, simd_lowering,
	  SimdVectorTEmitters::arithmetic<Arithmetic::subtract> },
	{ vector_t, "op_Multiply", "VV", true, simd_lowering,
	  SimdVectorTEmitters::arithmetic<Arithmetic::multiply> },
	{ vector_t, "op_Multiply", "VS", true, simd_lowering,
	  SimdVectorTEmitters::scale<0> },
	{ vector_t, "op_Multiply", "SV", true, simd_lowering,
	  SimdVectorTEmitters::scale<1> },
	{ vector_t, "op_Division", "VV", true, simd_lowering, SimdVectorTEmitters::divide },
	{ vector_t, "op_UnaryNegation", "V", true, simd_lowering,
	  SimdVectorTEmitters::negate },

	{ vector_t, "op_BitwiseAnd", "VV", true, simd_lowering,
	  SimdVectorTEmitters::bitwise<llvm::Instruction::And> },
	{ vector_t, "op_BitwiseOr", "VV", true, simd_lowering,
	  SimdVectorTEmitters::bitwise<llvm::Instruction::Or> },
	{ vector_t, "op_ExclusiveOr", "VV", true, simd_lowering,
	  SimdVectorTEmitters::bitwise<llvm::Instruction::Xor> },
	{ vector_t, "op_OnesComplement", "V", true, simd_lowering,
	  SimdVectorTEmitters::complement },

	{ vector_t, "op_Equality", "VV", true, simd_lowering,
	  SimdVectorTEmitters::all_lanes_equal<false> },
	{ vector_t, "op_Inequality", "VV", true, simd_lowering,
	  SimdVectorTEmitters::all_lanes_equal<true> },
	{ vector_t, "op_Explicit", "V", true, simd_lowering,
	  SimdVectorTEmitters::reinterpret },

	// The mask-answering statics, which the public Vector.Equals<T> and its
	// siblings forward to.
	{ vector_t, "Equals", "VV", true, simd_lowering,
	  SimdVectorTEmitters::mask<Comparison::equal> },
	{ vector_t, "LessThan", "VV", true, simd_lowering,
	  SimdVectorTEmitters::mask<Comparison::less> },
	{ vector_t, "LessThanOrEqual", "VV", true, simd_lowering,
	  SimdVectorTEmitters::mask<Comparison::less_or_equal> },
	{ vector_t, "GreaterThan", "VV", true, simd_lowering,
	  SimdVectorTEmitters::mask<Comparison::greater> },
	{ vector_t, "GreaterThanOrEqual", "VV", true, simd_lowering,
	  SimdVectorTEmitters::mask<Comparison::greater_or_equal> },
	{ vector_t, "ConditionalSelect", "VVV", true, simd_lowering,
	  SimdVectorTEmitters::conditional_select },

	{ vector_t, "Abs", "V", true, simd_lowering, SimdVectorTEmitters::absolute },
	{ vector_t, "Min", "VV", true, simd_lowering,
	  SimdVectorTEmitters::extremum<Comparison::less> },
	{ vector_t, "Max", "VV", true, simd_lowering,
	  SimdVectorTEmitters::extremum<Comparison::greater> },
	{ vector_t, "SquareRoot", "V", true, simd_lowering,
	  SimdVectorTEmitters::square_root },
	{ vector_t, "DotProduct", "VV", true, simd_lowering,
	  SimdVectorTEmitters::dot_product },

	// Narrow, Widen and the conversions are declared on the non-generic
	// Vector rather than on Vector<T>, so they carry a class key of their own.
	{ vector_statics, "ConvertToSingle", "V", true, simd_lowering,
	  SimdVectorTEmitters::to_floating },
	{ vector_statics, "ConvertToDouble", "V", true, simd_lowering,
	  SimdVectorTEmitters::to_floating },
	{ vector_statics, "ConvertToInt32", "V", true, simd_lowering,
	  SimdVectorTEmitters::to_integer<true> },
	{ vector_statics, "ConvertToInt64", "V", true, simd_lowering,
	  SimdVectorTEmitters::to_integer<true> },
	{ vector_statics, "ConvertToUInt32", "V", true, simd_lowering,
	  SimdVectorTEmitters::to_integer<false> },
	{ vector_statics, "ConvertToUInt64", "V", true, simd_lowering,
	  SimdVectorTEmitters::to_integer<false> },

	{ vector_statics, "Narrow", "VV", true, simd_lowering, SimdVectorTEmitters::narrow },
	{ vector_statics, "Widen", "VSS", true, simd_lowering, SimdVectorTEmitters::widen },
};

} // namespace

llvm::ArrayRef<BuiltinBody>
simd_vector_t_bodies ()
{
	return simd_vector_t_table;
}

} // namespace mono

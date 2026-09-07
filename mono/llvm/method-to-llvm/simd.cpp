/**
 * \file
 * \brief The bodies the backend writes for a SIMD type's operations.
 *
 * Each row must reproduce the managed body lane for lane. Tier 0 runs that body,
 * and a method's answer must not change when it promotes. Write a row from the
 * body, never from the SSE instruction it is expected to select.
 */

#include "intrinsics.hpp"

#include "../runtime/options.hpp"
#include "hidden-return.hpp"
#include "method-to-llvm.hpp"

#include <llvm/IR/Function.h>
#include <llvm/IR/Type.h>
#include <llvm/IR/Value.h>

namespace mono {

/// The emitters the table below points at. MethodLLVMEmitter befriends this
/// struct, so an emitter has to be a member of it to reach the arguments and
/// the float rules.
struct SimdEmitters {
	/// The two operands of a binary operator, combined into its answer.
	using BinaryOp = llvm::Value *(*) (llvm::IRBuilder<> &, llvm::Value *, llvm::Value *);

	/// Returns the value parameter i arrives in.
	///
	/// A SIMD class converts to a vector rather than a struct, so
	/// held_in_memory () leaves it in a register and there is nothing to load.
	static llvm::Value *argument (MethodLLVMEmitter &emitter, unsigned i)
	{
		return emitter.function->getArg (
			natural_parameter_index (i, emitter.function));
	}

	/// Writes op over a binary operator's two operands and returns it.
	///
	/// Answers nothing where the operands did not arrive as one vector type,
	/// which leaves the managed body to be translated. Only a class the loader
	/// marked simd_type converts to a vector.
	static BuiltinResult binary (MethodLLVMEmitter &emitter, llvm::IRBuilder<> &builder,
	                             BinaryOp op)
	{
		llvm::Value *lhs = argument (emitter, 0);
		llvm::Value *rhs = argument (emitter, 1);

		if (!lhs->getType ()->isVectorTy () || lhs->getType () != rhs->getType ())
			return std::nullopt;

		builder.CreateRet (op (builder, lhs, rhs));
		return llvm::Error::success ();
	}

	/// Writes a lane-wise float add. It carries relax_float ()'s flags because
	/// the managed body adds each lane, which is an addition the IL asked for.
	static llvm::Value *fadd (llvm::IRBuilder<> &builder, llvm::Value *lhs,
	                          llvm::Value *rhs)
	{
		return MethodLLVMEmitter::relax_float (builder.CreateFAdd (lhs, rhs));
	}

	static BuiltinResult float_add (MethodLLVMEmitter &emitter,
	                                llvm::IRBuilder<> &builder, MonoMethod *)
	{
		return binary (emitter, builder, fadd);
	}
};

namespace {

const BuiltinBody simd_table[] = {
	// il_agrees, because the managed body is `new Vector4f (v1.x + v2.x, ...)`
	// and that is this add in each lane.
	{ { "Mono.Simd", "Mono.Simd", "Vector4f" }, "op_Addition", 2, true, simd_lowering,
	  SimdEmitters::float_add },
};

} // namespace

llvm::ArrayRef<BuiltinBody>
simd_bodies ()
{
	return simd_table;
}

} // namespace mono

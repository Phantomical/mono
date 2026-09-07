/**
 * \file
 * \brief What a SIMD row file reaches the emitter's state through.
 */

#ifndef MONO_LLVM_METHOD_TO_LLVM_SIMD_EMIT_HPP
#define MONO_LLVM_METHOD_TO_LLVM_SIMD_EMIT_HPP

#include "hidden-return.hpp"
#include "method-to-llvm.hpp"

#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Type.h>
#include <llvm/IR/Value.h>

namespace mono {

/// The emitter state a SIMD row is written against.
///
/// MethodLLVMEmitter befriends this struct alone, so a new family's emitters
/// inherit it rather than adding a friendship of their own.
struct SimdEmit {
	/// The two operands of a binary operator, combined into its answer.
	using BinaryOp = llvm::Value *(*) (llvm::IRBuilder<> &, llvm::Value *, llvm::Value *);

	/// Returns the value of parameter i.
	///
	/// A SIMD class converts to a vector rather than a struct, so
	/// held_in_memory () leaves it in a register with nothing to load.
	static llvm::Value *argument (MethodLLVMEmitter &emitter, unsigned i)
	{
		return emitter.function->getArg (
			natural_parameter_index (i, emitter.function));
	}

	static llvm::Type *return_type (MethodLLVMEmitter &emitter)
	{
		return emitter.function->getReturnType ();
	}

	static llvm::LLVMContext &context (MethodLLVMEmitter &emitter)
	{
		return emitter.context ();
	}

	static llvm::Value *relax (llvm::Value *value)
	{
		return MethodLLVMEmitter::relax_float (value);
	}

	// Each of the four carries relax_float ()'s flags: a row writes one only
	// where the managed body's own IL asked for that operation.

	static llvm::Value *fadd (llvm::IRBuilder<> &builder, llvm::Value *lhs,
	                          llvm::Value *rhs)
	{
		return relax (builder.CreateFAdd (lhs, rhs));
	}

	static llvm::Value *fsub (llvm::IRBuilder<> &builder, llvm::Value *lhs,
	                          llvm::Value *rhs)
	{
		return relax (builder.CreateFSub (lhs, rhs));
	}

	static llvm::Value *fmul (llvm::IRBuilder<> &builder, llvm::Value *lhs,
	                          llvm::Value *rhs)
	{
		return relax (builder.CreateFMul (lhs, rhs));
	}

	static llvm::Value *fdiv (llvm::IRBuilder<> &builder, llvm::Value *lhs,
	                          llvm::Value *rhs)
	{
		return relax (builder.CreateFDiv (lhs, rhs));
	}
};

} // namespace mono

#endif

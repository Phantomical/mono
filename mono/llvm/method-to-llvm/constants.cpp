#include "method-to-llvm.hpp"
#include "mono/metadata/class-internals.h"
#include "mono/metadata/metadata.h"
#include <llvm/ADT/APFloat.h>
#include <llvm/ADT/APInt.h>
#include <llvm/IR/Constants.h>

namespace mono {

/*
 * III.3.40  ldc.<type> - load numeric constant
 *
 *   Format          Assembly Format   Description
 *   20 <int32>      ldc.i4 num        Push num of type int32 onto the stack as int32.
 *   21 <int64>      ldc.i8 num        Push num of type int64 onto the stack as int64.
 *   22 <float32>    ldc.r4 num        Push num of type float32 onto the stack as F.
 *   23 <float64>    ldc.r8 num        Push num of type float64 onto the stack as F.
 *   16              ldc.i4.0          Push 0 onto the stack as int32.
 *   17              ldc.i4.1          Push 1 onto the stack as int32.
 *   18              ldc.i4.2          Push 2 onto the stack as int32.
 *   19              ldc.i4.3          Push 3 onto the stack as int32.
 *   1A              ldc.i4.4          Push 4 onto the stack as int32.
 *   1B              ldc.i4.5          Push 5 onto the stack as int32.
 *   1C              ldc.i4.6          Push 6 onto the stack as int32.
 *   1D              ldc.i4.7          Push 7 onto the stack as int32.
 *   1E              ldc.i4.8          Push 8 onto the stack as int32.
 *   15              ldc.i4.m1         Push -1 onto the stack as int32.
 *   15              ldc.i4.M1         Push -1 of type int32 onto the stack as int32
 *                                     (alias for ldc.i4.m1).
 *   1F <int8>       ldc.i4.s num      Push num onto the stack as int32, short form.
 *
 * Stack Transition:
 *
 *   ... -> ..., num
 *
 * Description:
 *
 *   The ldc num instruction pushes number num or some constant onto the stack. There
 *   are special short encodings for the integers -128 through 127 (with especially
 *   short encodings for -1 through 8). All short encodings push 4-byte integers on the
 *   stack. Longer encodings are used for 8-byte integers and 4- and 8-byte
 *   floating-point numbers, as well as 4-byte values that do not fit in the short
 *   forms.
 *
 *   There are three ways to push an 8-byte integer constant onto the stack
 *
 *     1. For constants that shall be expressed in more than 32 bits, use the ldc.i8
 *        instruction.
 *     2. For constants that require 9-32 bits, use the ldc.i4 instruction followed by
 *        a conv.i8.
 *     3. For constants that can be expressed in 8 or fewer bits, use a short form
 *        instruction followed by a conv.i8.
 *
 *   There is no way to express a floating-point constant that has a larger range or
 *   greater precision than a 64-bit IEC 60559:1989 number, since these representations
 *   are not portable across architectures.
 *
 * Exceptions:
 *
 *   None.
 *
 * Verifiability:
 *
 *   The ldc instruction is always verifiable.
 */
llvm::Error
MethodLLVMEmitter::emit_ldc_i4 (MonoIrBuilder &builder, int32_t value)
{
	push_stack (builder.getInt32 (static_cast<uint32_t> (value)), mono_get_int32_type ());
	return llvm::Error::success ();
}

llvm::Error
MethodLLVMEmitter::emit_ldc_i8 (MonoIrBuilder &builder, int64_t value)
{
	push_stack (builder.getInt64 (static_cast<uint64_t> (value)),
	            m_class_get_byval_arg (mono_defaults.int64_class));
	return llvm::Error::success ();
}

/*
 * The float constants arrive as the bit patterns the IL stream holds rather than as
 * host floats: an IL float32 is IEC 60559 whatever the machine that reads it does, and
 * an APFloat built from the bits says so without a type pun in between.
 *
 * Both keep the width they were written at. The CLI has one float type, F, but nothing
 * is gained by widening every ldc.r4 to double here - the operand tables already pick
 * the wider of the two when an R4 and an R8 meet.
 */
llvm::Error
MethodLLVMEmitter::emit_ldc_r4 (MonoIrBuilder &builder, uint32_t bits)
{
	llvm::APFloat value (llvm::APFloat::IEEEsingle (), llvm::APInt (32, bits));

	push_stack (llvm::ConstantFP::get (builder.getFloatTy (), value),
	            m_class_get_byval_arg (mono_defaults.single_class));
	return llvm::Error::success ();
}

llvm::Error
MethodLLVMEmitter::emit_ldc_r8 (MonoIrBuilder &builder, uint64_t bits)
{
	llvm::APFloat value (llvm::APFloat::IEEEdouble (), llvm::APInt (64, bits));

	push_stack (llvm::ConstantFP::get (builder.getDoubleTy (), value),
	            m_class_get_byval_arg (mono_defaults.double_class));
	return llvm::Error::success ();
}

} // namespace mono

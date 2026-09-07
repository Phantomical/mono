/**
 * \file
 * \brief The float-to-integer conversions the conv opcodes and the SIMD rows
 * both write.
 */

#ifndef MONO_LLVM_METHOD_TO_LLVM_FLOAT_CONVERT_HPP
#define MONO_LLVM_METHOD_TO_LLVM_FLOAT_CONVERT_HPP

#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Type.h>
#include <llvm/IR/Value.h>

namespace mono {

/**
 * Truncates value toward zero into to, through one of the constrained intrinsics.
 *
 * The spec leaves the out-of-range result unspecified. Any answer is legal, but every
 * path must reach the same one, and plain fptosi and fptoui cannot do that. They are
 * poison out of range, and poison is not a value. LLVM folds it to zero wherever it can
 * see the operand. A value known only at run time instead keeps what the hardware
 * conversion left behind. So one call path converts -1.0f to ushort as 65535 and
 * another converts it as 0, with only the constant between them.
 *
 * The constrained intrinsics carry no poison clause, and LLVM constant folds none of
 * them, so each one reaches the target's own conversion instruction. On amd64 that is
 * cvttsd2si, which returns the integer indefinite value. The interpreter's C cast
 * compiles to the same instruction, so the two engines agree with no range test in
 * front of either of them.
 *
 * fpexcept.ignore asks for the value and no more than the value. The strictfp attribute
 * is what a constrained intrinsic requires of the function that holds it.
 *
 * \p to is an integer type, or a vector of them the same shape as value.
 */
llvm::Value *constrained_float_to_int (llvm::IRBuilder<> &builder, llvm::Value *value,
                                       llvm::Type *to, bool is_signed);

/**
 * Truncates value toward zero into an unsigned int64.
 *
 * This is the one conversion amd64 has no instruction for. cvttsd2si is signed, so a
 * value from 2^63 up has to come back through the low half: subtract 2^63, convert, and
 * put the bit back.
 *
 * The direction of the test decides what a NaN gives, and the two directions disagree.
 * The test here is "below 2^63", which a NaN fails. A NaN therefore takes the
 * subtraction and comes out as zero. mono_fconv_u8 () (mono/mini/icalls/fconv.c) tests the
 * same way, and the interpreter reaches that helper for MINT_CONV_U8_R8. So this shape is
 * what the two engines agree on, not the one LLVM picks for itself.
 */
llvm::Value *float_to_uint64 (llvm::IRBuilder<> &builder, llvm::Value *value, llvm::Type *to);

/**
 * Truncates value toward zero into an unsigned integer of 32 bits or fewer.
 *
 * Every such target's range fits inside a signed int64, so the conversion goes through
 * that width and never asks for an unsigned one. cvttsd2si's indefinite value,
 * 0x8000000000000000, truncates to the zero that mono_fconv_u4 ()
 * (mono/mini/icalls/fconv.c) and the interpreter both give for a NaN or an out-of-range
 * operand. AVX512F's vcvttsd2usi answers the same inputs with its own indefinite value
 * instead, all-ones, which does not.
 */
llvm::Value *float_to_uint32_or_narrower (llvm::IRBuilder<> &builder, llvm::Value *value,
                                          llvm::Type *to);

} // namespace mono

#endif

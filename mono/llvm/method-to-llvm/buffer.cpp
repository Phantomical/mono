/**
 * \file
 * \brief Compiling a System.Buffer copy into one memory intrinsic.
 */

#include "method-to-llvm.hpp"

#include "mono/metadata/class-internals.h"
#include "mono/metadata/metadata.h"

#include <llvm/IR/Constants.h>
#include <llvm/IR/Type.h>
#include <llvm/Support/Alignment.h>

#include <optional>
#include <string_view>

namespace mono {

namespace {

/// Whether t is a pointer itself, rather than a managed pointer to one.
bool
is_plain_pointer (MonoType *t)
{
	return !t->byref && t->type == MONO_TYPE_PTR;
}

/// Whether t is an integer type a byte count arrives in. is_signed says which
/// of the two forms it is, and it is written only when this answers true.
bool
is_count (MonoType *t, bool &is_signed)
{
	if (t->byref)
		return false;

	switch (t->type) {
	case MONO_TYPE_I4:
	case MONO_TYPE_I8:
	case MONO_TYPE_I:
		is_signed = true;
		return true;
	case MONO_TYPE_U4:
	case MONO_TYPE_U8:
	case MONO_TYPE_U:
		is_signed = false;
		return true;
	default:
		return false;
	}
}

} // namespace

/*
 * Both copies are managed code in this corlib, so this backend compiles them
 * like any other method. Memmove tests the two ranges for overlap and then
 * takes one of two paths: the RuntimeImports:Memmove icall for the overlapping
 * case, and Memcpy for the rest. Memcpy is a byte, word and dword copy ladder
 * for a count of 32 or less, and the InternalMemcpy icall above that. One
 * intrinsic says the whole of either, and the target lowers it to the copy its
 * own hardware wants.
 *
 * The name decides which intrinsic. Memcpy names the copy whose two ranges do
 * not overlap, which is what llvm.memcpy asks of a caller. Memmove names the
 * copy that accepts an overlap, and it is the entry the BCL arrives at.
 *
 * A row matches the managed body. An internal call under one of these names is
 * a different implementation, so the match stops rather than answer from the
 * wrong premise.
 *
 * One count parts the two engines, and the intrinsic is the half that is right.
 * Memmove gives a non-overlapping copy to Memcpy as `(int) len`. A count of 2GB
 * or more arrives negative there, so the ladder copies nothing, where
 * llvm.memmove copies what the count names.
 *
 * The overlap test hides almost all of that. Two buffers less than 4GB apart
 * differ by less than such a count, so they take the icall instead. What is
 * left is a copy of 2GB or more between buffers more than 4GB apart, which
 * Buffer:MemoryCopy reaches: it passes uint.MaxValue for each pass of a copy
 * longer than 4GB. No test here covers it, because the buffers are the size of
 * the copy.
 */

std::optional<BufferCopy>
buffer_copy_for (MonoMethod *method, MonoMethodSignature *sig)
{
	// A vararg site brings a signature of its own, whose parameter list holds
	// a sentinel and whatever types the caller chose.
	if (sig == nullptr || sig->hasthis || sig->call_convention == MONO_CALL_VARARG)
		return std::nullopt;
	if ((method->iflags & METHOD_IMPL_ATTRIBUTE_INTERNAL_CALL) != 0)
		return std::nullopt;
	if (sig->param_count != 3 || sig->ret->byref || sig->ret->type != MONO_TYPE_VOID)
		return std::nullopt;

	MonoClass *klass = method->klass;

	if (m_class_get_image (klass) != mono_get_corlib ())
		return std::nullopt;
	if (std::string_view (m_class_get_name_space (klass)) != "System")
		return std::nullopt;
	if (std::string_view (m_class_get_name (klass)) != "Buffer")
		return std::nullopt;

	std::string_view name (method->name);
	BufferCopy copy {};

	if (name == "Memcpy")
		copy.may_overlap = false;
	else if (name == "Memmove")
		copy.may_overlap = true;
	else
		return std::nullopt;

	// The generic Memmove<T> takes its two ends by reference and a count in
	// between, so the shape below is what keeps it out.
	if (!is_plain_pointer (sig->params[0]) || !is_plain_pointer (sig->params[1]))
		return std::nullopt;
	if (!is_count (sig->params[2], copy.signed_count))
		return std::nullopt;

	return copy;
}

/// Compiles a call opcode as the copy in copy, in place of the call. The
/// destination, the source and the byte count come off the evaluation stack.
llvm::Error
MethodLLVMEmitter::emit_buffer_copy (MonoIrBuilder &builder, const BufferCopy &copy,
                                     MonoMethodSignature *sig)
{
	llvm::Expected<std::vector<llvm::Value *>> args = pop_call_arguments (builder, sig);
	if (!args)
		return args.takeError ();

	llvm::Value *count = (*args)[2];

	// The intrinsic reads its count as unsigned, where the managed body reads a
	// signed one as a loop bound and copies nothing below zero. A select keeps
	// this block free of branches, and it folds away on a constant count.
	if (copy.signed_count) {
		llvm::Value *zero = llvm::ConstantInt::get (count->getType (), 0);

		count = builder.CreateSelect (builder.CreateICmpSGT (count, zero), count, zero);
	}

	// A byte pointer promises no alignment, and neither body reads or writes
	// its memory as volatile. A count of zero stays defined: LLVM makes both
	// intrinsics a no-op there, which is what the IL that reaches Memcpy with
	// one needs.
	llvm::Align byte (1);

	if (copy.may_overlap)
		builder.CreateMemMove ((*args)[0], byte, (*args)[1], byte, count);
	else
		builder.CreateMemCpy ((*args)[0], byte, (*args)[1], byte, count);

	pop_stack (sig->param_count);
	return llvm::Error::success ();
}

} // namespace mono

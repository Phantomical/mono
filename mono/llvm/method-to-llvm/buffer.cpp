/**
 * \file
 * \brief Compiling a raw memory copy into one memory intrinsic.
 */

#include "method-to-llvm.hpp"

#include "mono/metadata/class-internals.h"
#include "mono/metadata/metadata.h"

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

/// Whether t is an unsigned integer type a byte count arrives in.
///
/// A signed type is refused rather than clamped. System.Buffer turns a negative
/// count into no copy at all before it reaches either icall, so a signed count
/// here would name a signature this row was not written against.
bool
is_count (MonoType *t)
{
	if (t->byref)
		return false;

	switch (t->type) {
	case MONO_TYPE_U4:
	case MONO_TYPE_U8:
	case MONO_TYPE_U:
		return true;
	default:
		return false;
	}
}

} // namespace

/*
 * System.Runtime.RuntimeImports:Memcpy and :Memmove are internal calls whose
 * whole meaning is "copy these bytes". A call site otherwise reaches one
 * through a managed-to-native wrapper, which saves five callee-saved
 * registers, pushes an LMF, calls the C function through a pointer and tests
 * the interruption flag. The intrinsic below is the copy with none of that,
 * and a count the caller wrote as a literal becomes a few instructions instead.
 *
 * The name decides which intrinsic. Memcpy is the copy whose two ranges do not
 * overlap, which is what llvm.memcpy asks of a caller, and Memmove is the one
 * that accepts an overlap. Memmove_wbarrier is neither: it counts elements
 * rather than bytes and marks cards, so it keeps its call.
 *
 * Neither intrinsic can be given a null pointer. A fault inside a copy the
 * target inlines lands in managed code and raises NullReferenceException, and a
 * fault inside the memcpy or memmove the target calls instead lands in libc and
 * kills the process. System.Buffer therefore refuses null in managed code
 * before it calls either of these, which is what makes both engines agree.
 */

std::optional<BufferCopy>
buffer_copy_for (MonoMethod *method, MonoMethodSignature *sig)
{
	// A vararg site brings a signature of its own, whose parameter list holds
	// a sentinel and whatever types the caller chose.
	if (sig == nullptr || sig->hasthis || sig->call_convention == MONO_CALL_VARARG)
		return std::nullopt;
	if ((method->iflags & METHOD_IMPL_ATTRIBUTE_INTERNAL_CALL) == 0)
		return std::nullopt;
	if (sig->param_count != 3 || sig->ret->byref || sig->ret->type != MONO_TYPE_VOID)
		return std::nullopt;

	MonoClass *klass = method->klass;

	if (m_class_get_image (klass) != mono_get_corlib ())
		return std::nullopt;
	if (std::string_view (m_class_get_name_space (klass)) != "System.Runtime")
		return std::nullopt;
	if (std::string_view (m_class_get_name (klass)) != "RuntimeImports")
		return std::nullopt;

	std::string_view name (method->name);
	BufferCopy copy {};

	if (name == "Memcpy")
		copy.may_overlap = false;
	else if (name == "Memmove")
		copy.may_overlap = true;
	else
		return std::nullopt;

	if (!is_plain_pointer (sig->params[0]) || !is_plain_pointer (sig->params[1]))
		return std::nullopt;
	if (!is_count (sig->params[2]))
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

	// A byte pointer promises no alignment, and neither copy reads or writes
	// its memory as volatile. A count of zero stays defined: LLVM makes both
	// intrinsics a no-op there.
	llvm::Align byte (1);

	if (copy.may_overlap)
		builder.CreateMemMove ((*args)[0], byte, (*args)[1], byte, (*args)[2]);
	else
		builder.CreateMemCpy ((*args)[0], byte, (*args)[1], byte, (*args)[2]);

	pop_stack (sig->param_count);
	return llvm::Error::success ();
}

} // namespace mono

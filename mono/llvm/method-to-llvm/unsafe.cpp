/** \brief Lower the System.Runtime.CompilerServices.Unsafe intrinsics. */

#include "method-to-llvm.hpp"
#include "hidden-return.hpp"

#include "mono/metadata/class-internals.h"
#include "mono/metadata/metadata.h"

#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instructions.h>

#include <optional>
#include <string_view>

namespace mono {

namespace {

/// Operations implemented by the Unsafe intrinsic lowering.
enum class UnsafeOp {
	Move, // As, AsPointer, AsRef: a pointer read back unchanged.
	AreSame,
	IsAddressGreaterThan,
	IsAddressLessThan,
	AddElements, // Add<T> (ref/void*, int): scaled by sizeof (T).
	AddBytes, // AddByteOffset<T> (ref, IntPtr).
	SubtractElements, // Subtract<T> (ref, int).
	ByteOffset,
	SizeOf,
	Read,
	ReadUnaligned,
	WriteUnaligned,
	CopyBlock,
	InitBlockUnaligned,
};

struct UnsafeCall {
	UnsafeOp op;
	/// The generic argument the op is instantiated over, already resolved
	/// past an enum and checked concrete. Null for CopyBlock and
	/// InitBlockUnaligned, which take no generic argument.
	MonoType *element;
};

/// Return the concrete generic argument, or null when it is unavailable.
MonoType *
concrete_generic_argument (MonoMethod *method)
{
	MonoGenericContext *ctx = mono_method_get_context (method);

	if (ctx == nullptr || ctx->method_inst == nullptr
	    || ctx->method_inst->type_argc != 1)
		return nullptr;

	MonoType *t = mini_get_underlying_type (ctx->method_inst->type_argv[0]);

	if (mini_is_gsharedvt_variable_type (t))
		return nullptr;

	return t;
}

/// Whether t is an unmanaged pointer (MONO_TYPE_PTR) rather than a byref.
bool
is_raw_pointer (MonoType *t)
{
	return !t->byref && t->type == MONO_TYPE_PTR;
}

/// What call classifies to, or nothing when method is one this backend
/// leaves to its own IL - the nuint AddByteOffset forwarder, or any name
/// outside this table.
std::optional<UnsafeCall>
classify_unsafe (MonoMethod *method, MonoMethodSignature *sig)
{
	std::string_view name (method->name);
	int argc = sig->param_count;

	if (name == "CopyBlock" && argc == 3)
		return UnsafeCall { UnsafeOp::CopyBlock, nullptr };
	if (name == "InitBlockUnaligned" && argc == 3)
		return UnsafeCall { UnsafeOp::InitBlockUnaligned, nullptr };

	MonoType *t = concrete_generic_argument (method);

	if (t == nullptr)
		return std::nullopt;

	if (argc == 1) {
		if (name == "As" || name == "AsPointer" || name == "AsRef")
			return UnsafeCall { UnsafeOp::Move, t };
		if (name == "SizeOf")
			return UnsafeCall { UnsafeOp::SizeOf, t };
		if (name == "Read" && is_raw_pointer (sig->params[0]))
			return UnsafeCall { UnsafeOp::Read, t };
		if (name == "ReadUnaligned"
		    && (is_raw_pointer (sig->params[0]) || sig->params[0]->byref))
			return UnsafeCall { UnsafeOp::ReadUnaligned, t };
	}

	if (argc == 2) {
		if (name == "AreSame")
			return UnsafeCall { UnsafeOp::AreSame, t };
		if (name == "IsAddressGreaterThan")
			return UnsafeCall { UnsafeOp::IsAddressGreaterThan, t };
		if (name == "IsAddressLessThan")
			return UnsafeCall { UnsafeOp::IsAddressLessThan, t };
		if (name == "ByteOffset")
			return UnsafeCall { UnsafeOp::ByteOffset, t };
		if (name == "Add" && sig->params[1]->type == MONO_TYPE_I4)
			return UnsafeCall { UnsafeOp::AddElements, t };
		// AddByteOffset<T> (ref T, nuint) is METHOD_IMPL_ATTRIBUTE_AGGRESSIVE_INLINING
		// managed IL, not an extern that throws; MONO_TYPE_U is what tells the
		// two apart from the IntPtr overload this backend has to replace.
		if (name == "AddByteOffset" && sig->params[1]->type == MONO_TYPE_I)
			return UnsafeCall { UnsafeOp::AddBytes, t };
		if (name == "Subtract" && sig->params[1]->type == MONO_TYPE_I4)
			return UnsafeCall { UnsafeOp::SubtractElements, t };
		if (name == "WriteUnaligned"
		    && (is_raw_pointer (sig->params[0]) || sig->params[0]->byref))
			return UnsafeCall { UnsafeOp::WriteUnaligned, t };
	}

	return std::nullopt;
}

} // namespace

bool
is_unsafe_body_method (MonoMethod *method)
{
	MonoMethodSignature *sig = mono_method_signature_internal (method);

	if (sig == nullptr || sig->hasthis)
		return false;

	return classify_unsafe (method, sig).has_value ();
}

/// Compiles method's own body, for whichever Unsafe overload
/// classify_unsafe () recognizes, in place of its IL.
llvm::Error
MethodLLVMEmitter::emit_unsafe_body (MonoIrBuilder &builder, MonoMethod *method)
{
	MonoMethodSignature *sig = mono_method_signature_internal (method);
	std::optional<UnsafeCall> call = classify_unsafe (method, sig);

	if (!call)
		return unsupported_il ("an unrecognized Unsafe member");

	auto argument = [&] (unsigned i) {
		return function->getArg (natural_parameter_index (i, function));
	};

	switch (call->op) {
	case UnsafeOp::Move:
		builder.CreateRet (argument (0));
		return llvm::Error::success ();

	case UnsafeOp::AreSame:
	case UnsafeOp::IsAddressGreaterThan:
	case UnsafeOp::IsAddressLessThan: {
		llvm::CmpInst::Predicate pred = call->op == UnsafeOp::AreSame
		                                        ? llvm::CmpInst::ICMP_EQ
		                                : call->op == UnsafeOp::IsAddressGreaterThan
		                                        ? llvm::CmpInst::ICMP_UGT
		                                        : llvm::CmpInst::ICMP_ULT;
		llvm::Value *cmp = builder.CreateICmp (pred, argument (0), argument (1));

		builder.CreateRet (builder.CreateZExt (cmp, builder.getInt8Ty ()));
		return llvm::Error::success ();
	}

	case UnsafeOp::ByteOffset: {
		llvm::Type *native = builder.getIntNTy (TARGET_SIZEOF_VOID_P * 8);
		llvm::Value *target = builder.CreatePtrToInt (argument (1), native);
		llvm::Value *origin = builder.CreatePtrToInt (argument (0), native);

		builder.CreateRet (builder.CreateSub (target, origin));
		return llvm::Error::success ();
	}

	case UnsafeOp::SizeOf: {
		int align;
		int size = mono_type_size (call->element, &align);

		builder.CreateRet (builder.getInt32 (size));
		return llvm::Error::success ();
	}

	case UnsafeOp::AddElements:
	case UnsafeOp::SubtractElements: {
		MonoClass *element_class = mono_class_from_mono_type_internal (call->element);
		int esize = mono_class_array_element_size (element_class);
		llvm::Value *count = argument (1);

		if (call->op == UnsafeOp::SubtractElements)
			count = builder.CreateNeg (count);

		llvm::Value *bytes = builder.CreateSExt (
			builder.CreateMul (count, builder.getInt32 (esize)),
			builder.getIntNTy (TARGET_SIZEOF_VOID_P * 8));

		builder.CreateRet (builder.CreateGEP (builder.getInt8Ty (), argument (0), bytes));
		return llvm::Error::success ();
	}

	case UnsafeOp::AddBytes:
		builder.CreateRet (
			builder.CreateGEP (builder.getInt8Ty (), argument (0), argument (1)));
		return llvm::Error::success ();

	case UnsafeOp::Read:
	case UnsafeOp::ReadUnaligned: {
		llvm::Expected<llvm::Type *> conv = convert_type (call->element, /*native=*/false);

		if (!conv)
			return conv.takeError ();

		bool aligned = call->op == UnsafeOp::Read;
		llvm::Align source_align =
			aligned ? type_alignment (call->element, false) : llvm::Align (1);
		llvm::Value *source = argument (0);

		if (llvm::Argument *hidden = hidden_return_pointer (function)) {
			builder.CreateMemCpyInline (hidden, type_alignment (call->element, false),
			                            source, source_align,
			                            builder.getInt64 (vtype_size (call->element, false)));
			builder.CreateRetVoid ();
			return llvm::Error::success ();
		}

		builder.CreateRet (builder.CreateAlignedLoad (*conv, source, source_align));
		return llvm::Error::success ();
	}

	case UnsafeOp::WriteUnaligned: {
		llvm::Expected<llvm::Type *> conv = convert_type (call->element, /*native=*/false);

		if (!conv)
			return conv.takeError ();

		llvm::Value *destination = argument (0);
		llvm::Value *value = argument (1);

		// A held-in-memory value - one convert_type () turns into a struct -
		// arrives as that struct's own SSA value (emit_arg_allocas ()).
		// emit_memory_store () below wants the address of a copy instead.
		if ((*conv)->isStructTy ()) {
			llvm::Expected<llvm::Value *> slot = vtype_slot (call->element, /*native=*/false);

			if (!slot)
				return slot.takeError ();

			builder.CreateAlignedStore (value, *slot,
			                           type_alignment (call->element, false));
			value = *slot;
		}

		// emit_memory_store () is what stfld and stobj write through too, so
		// this gets the same card mark a stored reference or a
		// reference-bearing struct owes for free. Forcing prefixes.unaligned
		// gets the same call to answer at byte alignment instead of asking it
		// to take one as a parameter.
		uint8_t saved_unaligned = prefixes.unaligned;

		prefixes.unaligned = 1;

		llvm::Error stored = emit_memory_store (builder, value, destination, call->element);

		prefixes.unaligned = saved_unaligned;

		if (stored)
			return stored;

		builder.CreateRetVoid ();
		return llvm::Error::success ();
	}

	case UnsafeOp::CopyBlock: {
		llvm::Value *destination = argument (0);
		llvm::Value *source = argument (1);
		llvm::Value *count = argument (2);
		llvm::Value *count64 = count->getType ()->getIntegerBitWidth () < 64
		                               ? builder.CreateZExt (count, builder.getInt64Ty ())
		                               : count;

		builder.CreateMemCpy (destination, llvm::Align (1), source, llvm::Align (1),
		                      count64);
		builder.CreateRetVoid ();
		return llvm::Error::success ();
	}

	case UnsafeOp::InitBlockUnaligned: {
		llvm::Value *destination = argument (0);
		llvm::Value *fill = argument (1);
		llvm::Value *count = argument (2);
		llvm::Value *count64 = count->getType ()->getIntegerBitWidth () < 64
		                               ? builder.CreateZExt (count, builder.getInt64Ty ())
		                               : count;

		builder.CreateMemSet (destination, fill, count64, llvm::Align (1));
		builder.CreateRetVoid ();
		return llvm::Error::success ();
	}
	}

	return unsupported_il ("an unrecognized Unsafe member");
}

} // namespace mono

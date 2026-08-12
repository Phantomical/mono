#include "method-to-llvm.hpp"
#include "mono/metadata/class-internals.h"
#include "mono/metadata/debug-helpers.h"
#include "mono/metadata/metadata.h"
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Type.h>

namespace mono {

/// Pushes local variable `index` onto the evaluation stack, which is what ECMA-335
/// III.3.43 calls ldloc. The pushed value has the local's intermediate type, not its
/// declared type.
llvm::Error
MethodLLVMEmitter::emit_ldloc (MonoIrBuilder &builder, uint32_t index)
{
	if (index >= locals.size ())
		return invalid_local (index);

	const Entry &local = locals[index];

	return push_from_location (builder, local.alloca, local.type);
}

/// Pushes the address of local variable `index` onto the stack as a managed
/// pointer, which is what ECMA-335 III.3.44 calls ldloca.
llvm::Error
MethodLLVMEmitter::emit_ldloca (MonoIrBuilder &builder, uint32_t index)
{
	if (index >= locals.size ())
		return invalid_local (index);

	const Entry &local = locals[index];
	MonoClass *klass = mono_class_from_mono_type_internal (local.type);

	// The alloca already uses the local's natural alignment. A class's
	// this_arg is byval_arg with byref set, which is the managed pointer
	// ldloca must push.
	push_stack (local.alloca, m_class_get_this_arg (klass));
	return llvm::Error::success ();
}

/// Converts a value on the stack so it can be stored into a location of type
/// `destination`.
///
/// The evaluation stack tracks fewer types than a location can hold. This function
/// narrows an int32 back to its byte or char width. It widens an int32 to a native
/// int, and it converts between an address and a native int. Any other combination
/// is a type mismatch that correct IL cannot produce, and the function returns an
/// error for it.
llvm::Expected<llvm::Value *>
MethodLLVMEmitter::coerce_to_location (MonoIrBuilder &builder, StackValue value,
                                       MonoType *destination, bool native)
{
	llvm::Expected<llvm::Type *> type = convert_type (destination, native);
	if (!type)
		return type.takeError ();

	llvm::Type *from = value.value->getType ();

	// A value type already sits in memory, so storing one is a copy, not a
	// conversion. Only the two layouts need to agree.
	//
	// The layouts can differ even when the MonoTypes do not: a marshalled slot
	// uses a different struct of a different size.
	if (held_in_memory (destination)) {
		llvm::Expected<llvm::Type *> source = convert_type (value.type, value.native);

		if (source && *source == *type)
			return value.value;
		if (!source)
			llvm::consumeError (source.takeError ());
	} else if (from == *type) {
		return value.value;
	}

	if (from->isIntegerTy () && (*type)->isIntegerTy ()
	    && (*type)->getIntegerBitWidth () < from->getIntegerBitWidth ())
		return builder.CreateTrunc (value.value, *type);
	// ECMA-335 I.8.7 makes int32 assignable to a native int location, so
	// `ldc.i4.0` followed by `stloc` into one is correct IL.
	//
	// This function widens the value here with a sign extension, the same as
	// conv.i. Only a native int destination widens like this. An int64
	// destination needs an explicit conv.i8 for an int32 source.
	if (from->isIntegerTy () && (*type)->isIntegerTy ()
	    && stack_type (destination) == NativeInt
	    && (*type)->getIntegerBitWidth () > from->getIntegerBitWidth ())
		return builder.CreateSExt (value.value, *type);
	if (from->isPointerTy () && (*type)->isIntegerTy ())
		return builder.CreatePtrToInt (value.value, *type);
	if (from->isIntegerTy () && (*type)->isPointerTy ())
		return builder.CreateIntToPtr (value.value, *type);
	// The stack tracks one float type. An R4 location rounds the value, and an
	// R8 location widens it.
	if (from->isFloatingPointTy () && (*type)->isFloatingPointTy ())
		return builder.CreateFPCast (value.value, *type);

	char *source_name = mono_type_full_name (value.type);
	char *destination_name = mono_type_full_name (destination);
	llvm::Error error = invalid_il (llvm::Twine ("cannot store a value of type ") + source_name
	                                + " into a location of type " + destination_name);

	g_free (source_name);
	g_free (destination_name);
	return std::move (error);
}

/// Pops a value off the stack and stores it into local variable `index`, which is
/// what ECMA-335 III.3.63 calls stloc.
llvm::Error
MethodLLVMEmitter::emit_stloc (MonoIrBuilder &builder, uint32_t index)
{
	if (index >= locals.size ())
		return invalid_local (index);
	if (stack.empty ())
		return unbalanced_stack (1);

	const Entry &local = locals[index];
	llvm::Expected<llvm::Value *> value =
		coerce_to_location (builder, get_stack (0), local.type);
	if (!value)
		return value.takeError ();

	pop_stack (1);
	if (held_in_memory (local.type))
		copy_vtype (builder, local.alloca, *value, local.type, /*native=*/false);
	else
		builder.CreateAlignedStore (*value, local.alloca, type_alignment (local.type));
	return llvm::Error::success ();
}

/// Pops a byte count off the stack, allocates that many bytes from the frame, and
/// pushes the address, which is what ECMA-335 III.3.47 calls localloc. If the
/// method header's init_locals flag is set, the allocated bytes start zeroed.
llvm::Error
MethodLLVMEmitter::emit_localloc (MonoIrBuilder &builder)
{
	if (stack.size () != 1)
		return invalid_il ("localloc needs the size as the only thing on the stack");
	if (innermost_handler (offset) >= 0)
		return invalid_il ("localloc cannot occur inside an exception handler");

	StackValue size = get_stack (0);
	StackType size_type = stack_type (size.type);

	if (size_type != Int32 && size_type != NativeInt)
		return invalid_il (llvm::Twine ("a localloc size cannot be operand type ")
		                   + describe (size.type, size_type));

	llvm::Type *native = builder.getIntNTy (TARGET_SIZEOF_VOID_P * 8);
	llvm::Value *bytes = size.value;

	if (bytes->getType ()->isPointerTy ())
		bytes = builder.CreatePtrToInt (bytes, native);
	bytes = builder.CreateZExtOrTrunc (bytes, native);

	// This alloca is dynamic, and it is deliberately not in the entry block,
	// because its size is known only here.
	//
	// The frame reclaims the memory at return, which matches the local memory
	// pool the spec describes.
	llvm::AllocaInst *block = builder.CreateAlloca (builder.getInt8Ty (), bytes, "localloc");

	block->setAlignment (llvm::Align (TARGET_SIZEOF_VOID_P));

	if (cfg->header->init_locals)
		builder.CreateMemSet (block, builder.getInt8 (0), bytes,
		                      llvm::Align (TARGET_SIZEOF_VOID_P));

	pop_stack (1);
	push_stack (block, mono_get_int_type ());
	return llvm::Error::success ();
}

} // namespace mono

#include "method-to-llvm.hpp"
#include "mono/metadata/class-internals.h"
#include "mono/metadata/debug-helpers.h"
#include "mono/metadata/metadata.h"
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Type.h>

namespace mono {

/*
 * III.3.43  ldloc - load local variable onto the stack
 *
 *   Format                  Assembly Format   Description
 *   FE 0C <unsigned int16>  ldloc indx        Load local variable of index indx onto
 *                                             stack.
 *   11 <unsigned int8>      ldloc.s indx      Load local variable of index indx onto
 *                                             stack, short form.
 *   06                      ldloc.0           Load local variable 0 onto stack.
 *   07                      ldloc.1           Load local variable 1 onto stack.
 *   08                      ldloc.2           Load local variable 2 onto stack.
 *   09                      ldloc.3           Load local variable 3 onto stack.
 *
 * Stack Transition:
 *
 *   ... -> ..., value
 *
 * Description:
 *
 *   The ldloc indx instruction pushes the contents of the local variable number indx
 *   onto the evaluation stack, where local variables are numbered 0 onwards. Local
 *   variables are initialized to 0 before entering the method only if the localsinit
 *   on the method is true (see Partition I). The ldloc.0, ldloc.1, ldloc.2, and
 *   ldloc.3 instructions provide an efficient encoding for accessing the first 4 local
 *   variables. The ldloc.s instruction provides an efficient encoding for accessing
 *   local variables 4-255.
 *
 *   The type of the value on the stack is tracked by verification as the intermediate
 *   type (§I.8.7) of the local variable type, which is specified in the method header.
 *   See Partition I.
 *
 *   If required, local variables are converted to the representation of their
 *   intermediate type (§I.8.7) when loaded onto the stack (§III.1.1.1)
 *
 *   [Note: that is local variables smaller than 4 bytes, a boolean or a character are
 *   converted to 4 bytes by sign or zero-extension as appropriate. Floating-point
 *   values are converted to their native size (type F). end note]
 *
 * Exceptions:
 *
 *   System.VerificationException is thrown if the the localsinit bit for this method
 *   has not been set, and the assembly containing this method has not been granted
 *   System.Security.Permissions.SecurityPermission.SkipVerification (and the CIL does
 *   not perform automatic definite-assignment analysis)
 *
 * Correctness:
 *
 *   Correct CIL ensures that indx is a valid local index.
 *
 *   For the ldloc indx instruction, indx shall lie in the range 0-65534 inclusive
 *   (specifically, 65535 is not valid).
 *
 * Verifiability:
 *
 *   For verifiable code, this instruction shall guarantee that it is not loading an
 *   uninitialized value - whether that initialization is done explicitly by having set
 *   the localsinit bit for the method, or by previous instructions (where the CLI
 *   performs definite-assignment analysis).
 *
 *   Verification (§III.1.8) tracks the type of the value loaded onto the stack as the
 *   intermediate type (§I.8.7) of the local variable.
 */
llvm::Error
MethodLLVMEmitter::emit_ldloc (MonoIrBuilder &builder, uint32_t index)
{
	if (index >= locals.size ())
		return invalid_local (index);

	const Entry &local = locals[index];

	return push_from_location (builder, local.alloca, local.type);
}

/*
 * III.3.44  ldloca.<length> - load local variable address
 *
 *   Format                  Assembly Format   Description
 *   FE 0D <unsigned int16>  ldloca indx       Load address of local variable with
 *                                             index indx.
 *   12 <unsigned int8>      ldloca.s indx     Load address of local variable with
 *                                             index indx, short form.
 *
 * Stack Transition:
 *
 *   ... -> ..., address
 *
 * Description:
 *
 *   The ldloca instruction pushes the address of the local variable number indx onto
 *   the stack, where local variables are numbered 0 onwards. The value pushed on the
 *   stack is already aligned correctly for use with instructions like ldind and stind.
 *   The result is a managed pointer (type &). The ldloca.s instruction provides an
 *   efficient encoding for use with the local variables 0-255.
 *
 *   (Local variables that are the subject of ldloca shall be aligned as described in
 *   the ldind instruction, since the address obtained by ldloca can be used as an
 *   argument to ldind.)
 *
 * Exceptions:
 *
 *   System.VerificationException is thrown if the localsinit bit for this method has
 *   not been set, and the assembly containing this method has not been granted
 *   System.Security.Permissions.SecurityPermission.SkipVerification (and the CIL does
 *   not perform automatic definite-assignment analysis)
 *
 * Correctness:
 *
 *   Correct CIL ensures that indx is a valid local index.
 *
 *   For the ldloca indx instruction, indx shall lie in the range 0-65534 inclusive
 *   (specifically, 65535 is not valid).
 *
 * Verifiability:
 *
 *   Verification (§III.1.8) tracks the type of the value loaded onto the stack as a
 *   managed pointer to the verification type (§I.8.7) of the local variable. For
 *   verifiable code, this instruction shall guarantee that it is not loading the
 *   address of an uninitialized value - whether that initialization is done explicitly
 *   by having set the localsinit bit for the method, or by previous instructions
 *   (where the CLI performs definite-assignment analysis)
 */
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
/// The evaluation stack tracks fewer types than a location can hold. A value already
/// at destination's width or layout passes through unchanged. This function narrows
/// an int32 back to its byte or char width, widens an int32 to a native int, and
/// casts between float widths. It also converts between an address and a native int.
/// Any other combination is a type mismatch that correct IL cannot produce, and the
/// function returns an error for it.
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

/*
 * III.3.63  stloc - pop value from stack to local variable
 *
 *   Format                  Assembly Format   Description
 *   FE 0E <unsigned int16>  stloc indx        Pop a value from stack into local
 *                                             variable indx.
 *   13 <unsigned int8>      stloc.s indx      Pop a value from stack into local
 *                                             variable indx, short form.
 *   0A                      stloc.0           Pop a value from stack into local
 *                                             variable 0.
 *   0B                      stloc.1           Pop a value from stack into local
 *                                             variable 1.
 *   0C                      stloc.2           Pop a value from stack into local
 *                                             variable 2.
 *   0D                      stloc.3           Pop a value from stack into local
 *                                             variable 3.
 *
 * Stack Transition:
 *
 *   ..., value -> ...
 *
 * Description:
 *
 *   The stloc indx instruction pops the top value off the evaluation stack and moves
 *   it into local variable number indx (see Partition I), where local variables are
 *   numbered 0 onwards. The type of value shall match the type of the local variable
 *   as specified in the current method's locals signature. The stloc.0, stloc.1,
 *   stloc.2, and stloc.3 instructions provide an efficient encoding for the first 4
 *   local variables; the stloc.s instruction provides an efficient encoding for local
 *   variables 4-255.
 *
 *   Storing into locals that hold a value smaller than 4 bytes long truncates the
 *   value as it moves from the stack to the local variable. Floating-point values are
 *   rounded from their native size (type F) to the size associated with the argument.
 *   (See §III.1.1.1, Numeric data types.)
 *
 * Exceptions:
 *
 *   None.
 *
 * Correctness:
 *
 *   Correct CIL requires that indx be a valid local index. For the stloc indx
 *   instruction, indx shall lie in the range 0-65534 inclusive (specifically, 65535 is
 *   not valid).
 *
 * Verifiability:
 *
 *   Verification also checks that the type of value is verifier-assignable-to the type
 *   of the local, as specified in the current method's locals signature.
 */
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

/*
 * III.3.47  localloc - allocate space in the local dynamic memory pool
 *
 *   Format   Assembly Format   Description
 *   FE 0F    localloc          Allocate space from the local memory pool.
 *
 * Stack Transition:
 *
 *   size -> address
 *
 * Description:
 *
 *   The localloc instruction allocates size (type native unsigned int or U4) bytes
 *   from the local dynamic memory pool and returns the address (an unmanaged pointer,
 *   type native int) of the first allocated byte. If the localsinit flag on the
 *   method is true, the block of memory returned is initialized to 0; otherwise, the
 *   initial value of that block of memory is unspecified. The area of memory is newly
 *   allocated. When the current method returns, the local memory pool is available
 *   for reuse.
 *
 *   address is aligned so that any built-in data type can be stored there using the
 *   stind instructions and loaded using the ldind instructions.
 *
 *   The localloc instruction cannot occur within an exception block: filter, catch,
 *   finally, or fault.
 *
 *   [Rationale: localloc is used to create local aggregates whose size shall be
 *   computed at runtime. It can be used for C's intrinsic alloca method. end
 *   rationale]
 *
 * Exceptions:
 *
 *   System.StackOverflowException is thrown if there is insufficient memory to
 *   service the request.
 *
 * Correctness:
 *
 *   Correct CIL requires that the evaluation stack be empty, apart from the size item
 *
 * Verifiability:
 *
 *   This instruction is never verifiable.
 */
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

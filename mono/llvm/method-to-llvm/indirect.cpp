#include "method-to-llvm.hpp"
#include "mono/metadata/class-internals.h"
#include "mono/metadata/metadata.h"
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Type.h>

namespace mono {

/// The address on top of the stack as an LLVM pointer, null-checked.
///
/// An indirect access reads its location off the stack, where it may be a managed
/// pointer or a bare native int; an object reference is not an address something may
/// dereference directly, so it is refused rather than reinterpreted.
llvm::Expected<llvm::Value *>
MethodLLVMEmitter::indirect_address (MonoIrBuilder &builder, StackValue address)
{
	StackType type = stack_type (address.type);

	if (type != ManagedPtr && type != NativeInt)
		return invalid_il (llvm::Twine ("an indirect access needs a pointer, not ")
		                   + describe (address.type, type));

	llvm::Value *pointer = address.value;

	/* A native int address is only a number until something dereferences it. */
	if (!pointer->getType ()->isPointerTy ())
		pointer = builder.CreateIntToPtr (pointer,
		                                  llvm::PointerType::get (context (), 0));

	emit_null_check (builder, pointer);
	return pointer;
}

/*
 * III.3.42  ldind.<type> - load value indirect onto the stack
 *
 *   Format   Assembly Format   Description
 *   46       ldind.i1          Indirect load value of type int8 as int32 on the stack.
 *   48       ldind.i2          Indirect load value of type int16 as int32 on the stack.
 *   4A       ldind.i4          Indirect load value of type int32 as int32 on the stack.
 *   4C       ldind.i8          Indirect load value of type int64 as int64 on the stack.
 *   47       ldind.u1          Indirect load value of type unsigned int8 as int32 on
 *                              the stack.
 *   49       ldind.u2          Indirect load value of type unsigned int16 as int32 on
 *                              the stack.
 *   4B       ldind.u4          Indirect load value of type unsigned int32 as int32 on
 *                              the stack.
 *   4E       ldind.r4          Indirect load value of type float32 as F on the stack.
 *   4C       ldind.u8          Indirect load value of type unsigned int64 as int64 on
 *                              the stack (alias for ldind.i8).
 *   4F       ldind.r8          Indirect load value of type float64 as F on the stack.
 *   4D       ldind.i           Indirect load value of type native int as native int on
 *                              the stack
 *   50       ldind.ref         Indirect load value of type object ref as O on the
 *                              stack.
 *
 * Stack Transition:
 *
 *   ..., addr -> ..., value
 *
 * Description:
 *
 *   The ldind.<type> instruction indirectly loads a value from address addr (an
 *   unmanaged pointer, native int, or managed pointer, &) onto the stack. The source
 *   value is indicated by the instruction suffix. The ldind.ref instruction is a
 *   shortcut for a ldobj instruction that specifies the type pointed at by addr, all
 *   of the other ldind instructions are shortcuts for a ldobj instruction that
 *   specifies the corresponding built-in value class.
 *
 *   If required, values are converted to the representation of the intermediate type
 *   (§I.8.7) of the <type> in the instruction when loaded onto the stack (§III.1.1.1).
 *
 *   [Note: that is integer values smaller than 4 bytes, a boolean, or a character
 *   converted to 4 bytes by sign or zero-extension as appropriate. Floating-point
 *   values are converted to F type. end note]
 *
 *   Correct CIL ensures that the ldind instructions are used in a manner consistent
 *   with the type of the pointer.
 *
 *   The address specified by addr shall be to a location with the natural alignment
 *   of <type> or a NullReferenceException might occur (but see the unaligned. prefix
 *   instruction). (Alignment is discussed in Partition I.) The results of all CIL
 *   instructions that return addresses (e.g., ldloca and ldarga) are safely aligned.
 *   For data types larger than 1 byte, the byte ordering is dependent on the target
 *   CPU. Code that depends on byte ordering might not run on all platforms.
 *
 *   The operation of the ldind instructions can be altered by an immediately preceding
 *   volatile. or unaligned. prefix instruction.
 *
 *   [Rationale: Signed and unsigned forms for the small integer types are needed so
 *   that the CLI can know whether to sign extend or zero extend. The ldind.u8 and
 *   ldind.u4 variants are provided for convenience; ldind.u8 is an alias for ldind.i8;
 *   ldind.u4 and ldind.i4 have different opcodes, but their effect is identical. end
 *   rationale]
 *
 * Exceptions:
 *
 *   System.NullReferenceException can be thrown if an invalid address is detected.
 *
 * Correctness:
 *
 *   Correct CIL only uses an ldind instruction in a manner consistent with the type of
 *   the pointer. For ldind.ref the type pointer at by addr cannot be a generic
 *   parameter. [Note: A ldobj instruction can be used with generic parameter types.
 *   end note]
 *
 * Verifiability:
 *
 *   For ldind.ref addr shall be a managed pointer, T&, T shall be a reference type,
 *   and verification tracks the type of the result value as the verification type of
 *   T. For the other instruction variants, addr shall be a managed pointer, T&, and T
 *   shall be assignable-to (§I.8.7.3) the <type> in the instruction. Verification
 *   tracks the type of the result value as the intermediate type of <type>.
 */
llvm::Error
MethodLLVMEmitter::emit_ldind (MonoIrBuilder &builder, MonoType *element)
{
	if (stack.empty ())
		return unbalanced_stack (1);

	llvm::Expected<llvm::Type *> type = convert_type (element);
	if (!type)
		return type.takeError ();

	llvm::Expected<llvm::Value *> address = indirect_address (builder, get_stack (0));
	if (!address)
		return address.takeError ();

	llvm::Value *value =
		builder.CreateAlignedLoad (*type, *address, type_alignment (element));

	pop_stack (1);
	push_stack (widen_to_stack (builder, value, element), stack_slot_type (element));
	return llvm::Error::success ();
}

/*
 * III.3.62  stind.<type> - store value indirect from stack
 *
 *   Format   Assembly Format   Description
 *   52       stind.i1          Store value of type int8 into memory at address
 *   53       stind.i2          Store value of type int16 into memory at address
 *   54       stind.i4          Store value of type int32 into memory at address
 *   55       stind.i8          Store value of type int64 into memory at address
 *   56       stind.r4          Store value of type float32 into memory at address
 *   57       stind.r8          Store value of type float64 into memory at address
 *   DF       stind.i           Store value of type native int into memory at address
 *   51       stind.ref         Store value of type object ref (type O) into memory at
 *                              address
 *
 * Stack Transition:
 *
 *   ..., addr, val -> ...
 *
 * Description:
 *
 *   The stind instruction stores value val at address addr (an unmanaged pointer, type
 *   native int, or managed pointer, type &). The address specified by addr shall be
 *   aligned to the natural size of val or a NullReferenceException can occur (but see
 *   the unaligned. prefix instruction). The results of all CIL instructions that
 *   return addresses (e.g., ldloca and ldarga) are safely aligned. For data types
 *   larger than 1 byte, the byte ordering is dependent on the target CPU. Code that
 *   depends on byte ordering might not run on all platforms.
 *
 *   Storing into locations smaller than 4 bytes truncates the value as it moves from
 *   the stack to memory. Floating-point values are rounded from their native size
 *   (type F) to the size associated with the instruction. (See §III.1.1.1, Numeric
 *   data types.)
 *
 *   The stind.ref instruction is a shortcut for a stobj instruction that specifies the
 *   type pointed at by addr, all of the other stind instructions are shortcuts for a
 *   stobj instruction that specifies the corresponding built-in value class.
 *
 *   Type-safe operation requires that the stind instruction be used in a manner
 *   consistent with the type of the pointer.
 *
 *   The operation of the stind instruction can be altered by an immediately preceding
 *   volatile. or unaligned. prefix instruction.
 *
 * Exceptions:
 *
 *   System.NullReferenceException is thrown if addr is not naturally aligned for the
 *   argument type implied by the instruction suffix.
 *
 * Correctness:
 *
 *   Correct CIL ensures that addr is a pointer to T and the type of val is
 *   verifier-assignable-to T. For stind.ref the type pointer at by addr cannot be a
 *   generic parameter. [Note: A stobj instruction can be used with generic parameter
 *   types. end note]
 *
 * Verifiability:
 *
 *   For verifiable code, addr shall be a managed pointer, T&, and the type of val
 *   shall be verifier-assignable-to T.
 */
llvm::Error
MethodLLVMEmitter::emit_stind (MonoIrBuilder &builder, MonoType *element)
{
	if (stack.size () < 2)
		return unbalanced_stack (2);

	llvm::Expected<llvm::Value *> value =
		coerce_to_location (builder, get_stack (0), element);
	if (!value)
		return value.takeError ();

	llvm::Expected<llvm::Value *> address = indirect_address (builder, get_stack (1));
	if (!address)
		return address.takeError ();

	pop_stack (2);

	/*
	 * A reference going into memory the GC may be tracking has to go through the
	 * collector, and the address here could point anywhere - the generic barrier is
	 * the one that tolerates that.
	 */
	if (mini_type_is_reference (element))
		builder.CreateCall (wbarrier_decl (), { *address, *value });
	else
		builder.CreateAlignedStore (*value, *address, type_alignment (element));

	return llvm::Error::success ();
}

} // namespace mono

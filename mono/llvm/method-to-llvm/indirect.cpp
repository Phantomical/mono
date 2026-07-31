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

/// The collector's hook for copying a value type that has reference fields inside it:
/// it moves the bytes and marks the cards in one step, given the class so it knows
/// where the references sit.
llvm::FunctionCallee
MethodLLVMEmitter::value_copy_decl ()
{
	llvm::LLVMContext &ctx = context ();
	llvm::Type *ptr = llvm::PointerType::get (ctx, 0);

	return module->getOrInsertFunction ("mono_gc_wbarrier_value_copy_internal",
	                                    llvm::Type::getVoidTy (ctx), ptr, ptr,
	                                    llvm::Type::getInt32Ty (ctx), ptr);
}

/*
 * III.4.13  ldobj - copy a value from an address to the stack
 *
 *   Format     Assembly Format   Description
 *   71 <T>     ldobj typeTok     Copy the value stored at address src to the stack.
 *
 * Stack Transition:
 *
 *   ..., src -> ..., val
 *
 * Description:
 *
 *   The ldobj instruction copies a value to the evaluation stack. typeTok is a
 *   metadata token (a typedef, typeref, or typespec). src is an unmanaged pointer
 *   (native int), or a managed pointer (&). If typeTok is not a generic parameter and
 *   either a reference type or a built-in value class, then the ldind instruction
 *   provides a shorthand for the ldobj instruction.
 *
 *   [Rationale: The ldobj instruction can be used to pass a value type as an argument.
 *   end rationale]
 *
 *   If required values are converted to the representation of the intermediate type
 *   (§I.8.7) of typeTok when loaded onto the stack (§III.1.1.1).
 *
 *   [Note: That is integer values of less than 4 bytes, a boolean or a character are
 *   converted to 4 bytes by sign or zero-extension as appropriate. Floating-point
 *   values are converted to F type. end note]
 *
 *   The operation of the ldobj instruction can be altered by an immediately preceding
 *   volatile. or unaligned. prefix instruction.
 *
 * Exceptions:
 *
 *   System.NullReferenceException can be thrown if an invalid address is detected.
 *
 *   System.TypeLoadException is thrown if typeTok cannot be found. This is typically
 *   detected when CIL is converted to native code rather than at runtime.
 *
 * Correctness:
 *
 *   typeTok shall be a valid typedef, typeref, or typespec metadata token.
 *
 *   [Note: Unlike the ldind instruction a ldobj instruction can be used with a generic
 *   parameter type. end note]
 *
 * Verifiability:
 *
 *   The tracked type of the source value on top of the stack shall be a managed
 *   pointer to some type srcType, and srcType shall be a assignable-to the type
 *   typeTok. Verification tracks the type of the result val as the intermediate type
 *   of typeTok.
 */
llvm::Error
MethodLLVMEmitter::emit_ldobj (MonoIrBuilder &builder, uint32_t token)
{
	llvm::Expected<MonoType *> type = element_type_from_token (token);
	if (!type)
		return type.takeError ();

	return emit_ldind (builder, *type);
}

/*
 * III.4.29  stobj - store a value at an address
 *
 *   Format     Assembly Format   Description
 *   81 <T>     stobj typeTok     Store a value of type typeTok at an address.
 *
 * Stack Transition:
 *
 *   ..., dest, src -> ...,
 *
 * Description:
 *
 *   The stobj instruction copies the value src to the address dest. If typeTok is not
 *   a generic parameter and either a reference type or a built-in value class, then
 *   the stind instruction provides a shorthand for the stobj instruction.
 *
 *   Storing values smaller than 4 bytes truncates the value as it moves from the
 *   stack to memory. Floating-point values are rounded from their native size (type
 *   F) to the size associated with typeTok. (See §III.1.1.1, Numeric data types.)
 *
 *   The operation of the stobj instruction can be altered by an immediately preceding
 *   volatile. or unaligned. prefix instruction.
 *
 * Exceptions:
 *
 *   System.NullReferenceException can be thrown if an invalid address is detected.
 *
 *   System.TypeLoadException is thrown if typeTok cannot be found. This is typically
 *   detected when CIL is converted to native code rather than at runtime.
 *
 * Correctness:
 *
 *   Correct CIL ensures that dest is a pointer to T and the type of src is
 *   verifier-assignable-to T. typeTok shall be a valid typedef, typeref, or typespec
 *   metadata token.
 *
 *   [Note: Unlike the stind instruction a stobj instruction can be used with a
 *   generic parameter type. end note]
 *
 * Verifiability:
 *
 *   Let the tracked type of the value on top of the stack be some type srcType. The
 *   value shall be initialized (when srcType is a reference type). The tracked type
 *   of the destination address dest on the preceding stack slot shall be a managed
 *   pointer (of type destType&) to some type destType. Finally, srcType shall be
 *   verifier-assignable-to typeTok.
 */
llvm::Error
MethodLLVMEmitter::emit_stobj (MonoIrBuilder &builder, uint32_t token)
{
	llvm::Expected<MonoType *> type = element_type_from_token (token);
	if (!type)
		return type.takeError ();

	MonoClass *klass = mono_class_from_mono_type_internal (*type);

	if (!m_class_is_valuetype (klass) || !m_class_has_references (klass))
		return emit_stind (builder, *type);

	/*
	 * A struct with references inside cannot just be stored: the collector has to
	 * mark the cards its reference fields land on. Its barrier copies from memory
	 * to memory, so the value takes a detour through a stack slot to have an
	 * address at all.
	 */
	if (stack.size () < 2)
		return unbalanced_stack (2);

	llvm::Expected<llvm::Value *> value =
		coerce_to_location (builder, get_stack (0), *type);
	if (!value)
		return value.takeError ();

	llvm::Expected<llvm::Value *> address = indirect_address (builder, get_stack (1));
	if (!address)
		return address.takeError ();

	MonoIrBuilder entry (entry_block, entry_block->begin ());
	llvm::AllocaInst *temp = entry.CreateAlloca ((*value)->getType ());

	builder.CreateAlignedStore (*value, temp, type_alignment (*type));
	builder.CreateCall (value_copy_decl (),
	                    { *address, temp, builder.getInt32 (1),
	                      class_symbol (klass, "mono_class_") });

	pop_stack (2);
	return llvm::Error::success ();
}

/*
 * III.4.4  cpobj - copy a value from one address to another
 *
 *   Format     Assembly Format   Description
 *   70 <T>     cpobj typeTok     Copy a value type from src to dest.
 *
 * Stack Transition:
 *
 *   ..., dest, src -> ...,
 *
 * Description:
 *
 *   The cpobj instruction copies the value at the address specified by src (an
 *   unmanaged pointer, native int, or a managed pointer, &) to the address specified
 *   by dest (also a pointer). typeTok can be a typedef, typeref, or typespec. The
 *   behavior is unspecified if the type of the location referenced by src is not
 *   assignable-to (§I.8.7.3) the type of the location referenced by dest.
 *
 *   If typeTok is a reference type, the cpobj instruction has the same effect as
 *   ldind.ref followed by stind.ref.
 *
 * Exceptions:
 *
 *   System.NullReferenceException can be thrown if an invalid address is detected.
 *
 *   System.TypeLoadException is thrown if typeTok cannot be found. This is typically
 *   detected when CIL is converted to native code rather than at runtime.
 *
 * Correctness:
 *
 *   typeTok shall be a valid typedef, typeref, or typespec metadata token.
 *
 * Verifiability:
 *
 *   The tracked types of the destination (dest) and source (src) values shall both be
 *   managed pointers (&) to values whose types we denote destType and srcType,
 *   respectively. Finally, srcType shall be assignable-to (§I.8.7.3) typeTok, and
 *   typeTok shall be assignable-to (§I.8.7.3) destType. In the case of an Enum, its
 *   type is that of the underlying, or base, type of the Enum.
 */
llvm::Error
MethodLLVMEmitter::emit_cpobj (MonoIrBuilder &builder, uint32_t token)
{
	llvm::Expected<MonoType *> type = element_type_from_token (token);
	if (!type)
		return type.takeError ();

	if (stack.size () < 2)
		return unbalanced_stack (2);

	llvm::Expected<llvm::Value *> src = indirect_address (builder, get_stack (0));
	if (!src)
		return src.takeError ();

	llvm::Expected<llvm::Value *> dest = indirect_address (builder, get_stack (1));
	if (!dest)
		return dest.takeError ();

	pop_stack (2);

	MonoClass *klass = mono_class_from_mono_type_internal (*type);

	if (mini_type_is_reference (*type)) {
		/* The reference itself moves, which is the ldind.ref/stind.ref pair. */
		llvm::Value *value = builder.CreateAlignedLoad (
			llvm::PointerType::get (context (), 0), *src,
			llvm::Align (TARGET_SIZEOF_VOID_P));

		builder.CreateCall (wbarrier_decl (), { *dest, value });
	} else if (m_class_has_references (klass)) {
		builder.CreateCall (value_copy_decl (),
		                    { *dest, *src, builder.getInt32 (1),
		                      class_symbol (klass, "mono_class_") });
	} else {
		guint32 align = 0;
		guint32 size = mono_class_value_size (klass, &align);

		builder.CreateMemCpy (*dest, llvm::Align (align), *src, llvm::Align (align),
		                      size);
	}

	return llvm::Error::success ();
}

/*
 * III.4.5  initobj - initialize the value at an address
 *
 *   Format        Assembly Format   Description
 *   FE 15 <T>     initobj typeTok   Initialize the value at address dest.
 *
 * Stack Transition:
 *
 *   ..., dest -> ...,
 *
 * Description:
 *
 *   The initobj instruction initializes an address with a default value. typeTok is a
 *   metadata token (a typedef, typeref, or typespec). dest is an unmanaged pointer
 *   (native int), or a managed pointer (&). If typeTok is a value type, the initobj
 *   instruction initializes each field of dest to null or a zero of the appropriate
 *   built-in type. If typeTok is a value type, then after this instruction is
 *   executed, the instance is ready for a constructor method to be called. If typeTok
 *   is a reference type, the initobj instruction has the same effect as ldnull
 *   followed by stind.ref.
 *
 *   Unlike newobj, the initobj instruction does not call any constructor method.
 *
 * Exceptions:
 *
 *   None.
 *
 * Correctness:
 *
 *   typeTok shall be a valid typedef, typeref, or typespec metadata token.
 *
 * Verifiability:
 *
 *   The type of the destination value on top of the stack shall be a managed pointer
 *   to some type destType, and typeTok shall be assignable-to destType. If typeTok is
 *   a non-reference type, the definition of subtyping implies that destType and
 *   typeTok shall be equal.
 */
llvm::Error
MethodLLVMEmitter::emit_initobj (MonoIrBuilder &builder, uint32_t token)
{
	llvm::Expected<MonoType *> type = element_type_from_token (token);
	if (!type)
		return type.takeError ();

	if (stack.empty ())
		return unbalanced_stack (1);

	llvm::Expected<llvm::Value *> dest = indirect_address (builder, get_stack (0));
	if (!dest)
		return dest.takeError ();

	pop_stack (1);

	/*
	 * Zeroing never creates a reference the collector could miss, so both shapes
	 * store plainly: null for a reference, cleared bytes for a value type.
	 */
	if (mini_type_is_reference (*type)) {
		llvm::PointerType *ptr = llvm::PointerType::get (context (), 0);

		builder.CreateAlignedStore (llvm::ConstantPointerNull::get (ptr), *dest,
		                            llvm::Align (TARGET_SIZEOF_VOID_P));
	} else {
		MonoClass *klass = mono_class_from_mono_type_internal (*type);
		guint32 align = 0;
		guint32 size = mono_class_value_size (klass, &align);

		builder.CreateMemSet (*dest, builder.getInt8 (0), size,
		                      llvm::Align (align));
	}

	return llvm::Error::success ();
}

} // namespace mono

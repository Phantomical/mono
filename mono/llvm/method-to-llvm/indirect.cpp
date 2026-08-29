#include "method-to-llvm.hpp"
#include "mono/metadata/class-internals.h"
#include "mono/metadata/metadata.h"
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Type.h>

namespace mono {

llvm::Expected<llvm::Value *>
MethodLLVMEmitter::indirect_address (MonoIrBuilder &builder, StackValue address)
{
	StackType type = stack_type (address.type);

	if (type != ManagedPtr && type != NativeInt)
		return invalid_il (llvm::Twine ("an indirect access needs a pointer, not ")
		                   + describe (address.type, type));

	llvm::Value *pointer = address.value;

	if (!pointer->getType ()->isPointerTy ())
		pointer = builder.CreateIntToPtr (pointer, llvm::PointerType::get (context (), 0));

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

	llvm::Expected<llvm::Value *> address = indirect_address (builder, get_stack (0));
	if (!address)
		return address.takeError ();

	pop_stack (1);
	return push_from_location (builder, *address, element);
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

	llvm::Expected<llvm::Value *> value = coerce_to_location (builder, get_stack (0), element);
	if (!value)
		return value.takeError ();

	llvm::Expected<llvm::Value *> address = indirect_address (builder, get_stack (1));
	if (!address)
		return address.takeError ();

	pop_stack (2);
	if (llvm::Error stored = emit_memory_store (builder, *value, *address, element))
		return stored;
	return llvm::Error::success ();
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

	return emit_stind (builder, *type);
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
		// Here the reference itself moves, so this loads it and stores it
		// through the write barrier.
		llvm::Value *value =
			builder.CreateAlignedLoad (llvm::PointerType::get (context (), 0), *src,
		                                   llvm::Align (TARGET_SIZEOF_VOID_P));

		emit_reference_store (builder, *dest, value,
		                      llvm::Align (TARGET_SIZEOF_VOID_P));
	} else if (m_class_has_references (klass)) {
		// The IL says nothing about the two addresses, so they may overlap.
		if (llvm::Error copied =
		            emit_value_copy (builder, *dest, *src, klass, /*may_overlap=*/true))
			return copied;
	} else {
		guint32 align = 0;
		guint32 size = mono_class_value_size (klass, &align);

		builder.CreateMemCpyInline (*dest, llvm::Align (align), *src,
		                            llvm::Align (align), builder.getInt64 (size));
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

	// Zeroing never creates a reference for the collector to miss.
	if (mini_type_is_reference (*type)) {
		llvm::PointerType *ptr = llvm::PointerType::get (context (), 0);

		builder.CreateAlignedStore (llvm::ConstantPointerNull::get (ptr), *dest,
		                            llvm::Align (TARGET_SIZEOF_VOID_P));
	} else {
		MonoClass *klass = mono_class_from_mono_type_internal (*type);
		guint32 align = 0;
		guint32 size = mono_class_value_size (klass, &align);

		builder.CreateMemSetInline (*dest, llvm::Align (align), builder.getInt8 (0),
		                           builder.getInt64 (size));
	}

	return llvm::Error::success ();
}

llvm::Expected<llvm::Value *>
MethodLLVMEmitter::block_size (MonoIrBuilder &builder, StackValue size)
{
	StackType type = stack_type (size.type);

	if (type != Int32 && type != NativeInt)
		return invalid_il (llvm::Twine ("a block size cannot be operand type ")
		                   + describe (size.type, type));

	llvm::Type *native = builder.getIntNTy (TARGET_SIZEOF_VOID_P * 8);
	llvm::Value *value = size.value;

	if (value->getType ()->isPointerTy ())
		value = builder.CreatePtrToInt (value, native);

	// The size is unsigned int32, so it zero-extends instead of sign-extends.
	return builder.CreateZExtOrTrunc (value, native);
}

/*
 * A block copy, inlined where that is the better lowering.
 *
 * The inline form keeps the copy inside this method, so a fault on a bad
 * pointer lands in managed code and the runtime raises the exception it owes
 * rather than dying inside libc. It is the right form only for a count the IL
 * gave as a constant. PreISelIntrinsicLowering expands a non-constant one into
 * a byte-at-a-time loop instead of letting the target lower it, because
 * getMemcpyLoopLoweringType is a byte on amd64. The plain form reaches
 * SelectionDAG, where `rep movsb` is what such a count becomes.
 *
 * Both ends keep the alignment the site promised, and neither form is volatile:
 * the fences a volatile block op needs are written around the copy.
 */
void
MethodLLVMEmitter::emit_inlined_copy (MonoIrBuilder &builder, llvm::Value *dest,
                                      llvm::Align dest_align, llvm::Value *src,
                                      llvm::Align src_align, llvm::Value *size)
{
	if (llvm::isa<llvm::ConstantInt> (size))
		builder.CreateMemCpyInline (dest, dest_align, src, src_align, size);
	else
		builder.CreateMemCpy (dest, dest_align, src, src_align, size);
}

/*
 * III.3.30  cpblk - copy data from memory to memory
 *
 *   Format     Instruction   Description
 *   FE 17      cpblk         Copy data from memory to memory.
 *
 * Stack Transition:
 *
 *   ..., destaddr, srcaddr, size -> ...
 *
 * Description:
 *
 *   The cpblk instruction copies size (of type unsigned int32) bytes from address
 *   srcaddr (of type native int, or &) to address destaddr (of type native int, or
 *   &). The behavior of cpblk is unspecified if the source and destination areas
 *   overlap.
 *
 *   cpblk assumes that both destaddr and srcaddr are aligned to the natural size of
 *   the machine (but see the unaligned. prefix instruction). The operation of the
 *   cpblk instruction can be altered by an immediately preceding volatile. or
 *   unaligned. prefix instruction.
 *
 *   [Rationale: cpblk is intended for copying structures (rather than arbitrary
 *   byte-runs). All such structures, allocated by the CLI, are naturally aligned for
 *   the current platform. Therefore, there is no need for the compiler that generates
 *   cpblk instructions to be aware of whether the code will eventually execute on a
 *   32-bit or 64-bit platform. end rationale]
 *
 * Exceptions:
 *
 *   System.NullReferenceException can be thrown if an invalid address is detected.
 *
 * Correctness:
 *
 *   CIL ensures the conditions specified above.
 *
 * Verifiability:
 *
 *   The cpblk instruction is never verifiable.
 */
llvm::Error
MethodLLVMEmitter::emit_cpblk (MonoIrBuilder &builder)
{
	if (stack.size () < 3)
		return unbalanced_stack (3);

	llvm::Expected<llvm::Value *> size = block_size (builder, get_stack (0));
	if (!size)
		return size.takeError ();

	llvm::Expected<llvm::Value *> src = indirect_address (builder, get_stack (1));
	if (!src)
		return src.takeError ();

	llvm::Expected<llvm::Value *> dest = indirect_address (builder, get_stack (2));
	if (!dest)
		return dest.takeError ();

	pop_stack (3);

	llvm::Align align = prefixes.unaligned != 0 ? llvm::Align (prefixes.unaligned)
	                                            : llvm::Align (TARGET_SIZEOF_VOID_P);

	// A block copy is a load and a store together. A volatile one fences both
	// sides, because ordering only one side leaves the other unordered.
	if (prefixes.volatile_)
		builder.CreateFence (llvm::AtomicOrdering::SequentiallyConsistent);
	emit_inlined_copy (builder, *dest, align, *src, align, *size);
	if (prefixes.volatile_)
		builder.CreateFence (llvm::AtomicOrdering::SequentiallyConsistent);

	return llvm::Error::success ();
}

/*
 * III.3.36  initblk - initialize a block of memory to a value
 *
 *   Format     Assembly Format   Description
 *   FE 18      initblk           Set all bytes in a block of memory to a given byte
 *                                value.
 *
 * Stack Transition:
 *
 *   ..., addr, value, size -> ...
 *
 * Description:
 *
 *   The initblk instruction sets size (of type unsigned int32) bytes starting at addr
 *   (of type native int, or &) to value (of type unsigned int8). initblk assumes that
 *   addr is aligned to the natural size of the machine (but see the unaligned. prefix
 *   instruction).
 *
 *   [Rationale: initblk is intended for initializing structures (rather than
 *   arbitrary byte-runs). All such structures, allocated by the CLI, are naturally
 *   aligned for the current platform. Therefore, there is no need for the compiler
 *   that generates initblk instructions to be aware of whether the code will
 *   eventually execute on a 32-bit or 64-bit platform. end rationale]
 *
 *   The operation of the initblk instructions can be altered by an immediately
 *   preceding volatile. or unaligned. prefix instruction.
 *
 * Exceptions:
 *
 *   System.NullReferenceException can be thrown if an invalid address is detected.
 *
 * Correctness:
 *
 *   Correct CIL code ensures the restrictions specified above.
 *
 * Verifiability:
 *
 *   The initblk instruction is never verifiable.
 */
llvm::Error
MethodLLVMEmitter::emit_initblk (MonoIrBuilder &builder)
{
	if (stack.size () < 3)
		return unbalanced_stack (3);

	llvm::Expected<llvm::Value *> size = block_size (builder, get_stack (0));
	if (!size)
		return size.takeError ();

	StackValue value = get_stack (1);
	StackType value_type = stack_type (value.type);

	if (value_type != Int32 && value_type != NativeInt)
		return invalid_il (llvm::Twine ("a fill byte cannot be operand type ")
		                   + describe (value.type, value_type));

	llvm::Expected<llvm::Value *> dest = indirect_address (builder, get_stack (2));
	if (!dest)
		return dest.takeError ();

	pop_stack (3);

	llvm::Align align = prefixes.unaligned != 0 ? llvm::Align (prefixes.unaligned)
	                                            : llvm::Align (TARGET_SIZEOF_VOID_P);
	llvm::Value *fill = value.value;

	if (fill->getType ()->isPointerTy ())
		fill = builder.CreatePtrToInt (fill, builder.getIntNTy (TARGET_SIZEOF_VOID_P * 8));
	fill = builder.CreateZExtOrTrunc (fill, builder.getInt8Ty ());

	// A volatile fill is only a store, so the fence goes before it.
	if (prefixes.volatile_)
		builder.CreateFence (llvm::AtomicOrdering::Release);
	// The same split as a block copy, for the same reason. amd64 lowers only a
	// constant-count fill itself, so a runtime count keeps the plain form and
	// the call to memset behind it.
	if (llvm::isa<llvm::ConstantInt> (*size))
		builder.CreateMemSetInline (*dest, align, fill, *size);
	else
		builder.CreateMemSet (*dest, fill, *size, align, prefixes.volatile_);

	return llvm::Error::success ();
}

} // namespace mono

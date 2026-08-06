#include "method-to-llvm.hpp"
#include "mono/metadata/class-internals.h"
#include "mono/metadata/metadata.h"
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Type.h>

namespace mono {

/*
 * III.3.38  ldarg.<length> - load argument onto the stack
 *
 *   Format                     Assembly Format   Description
 *   FE 09 <unsigned int16>     ldarg num         Load argument numbered num onto the
 *                                                stack.
 *   0E <unsigned int8>         ldarg.s num       Load argument numbered num onto the
 *                                                stack, short form.
 *   02                         ldarg.0           Load argument 0 onto the stack.
 *   03                         ldarg.1           Load argument 1 onto the stack.
 *   04                         ldarg.2           Load argument 2 onto the stack.
 *   05                         ldarg.3           Load argument 3 onto the stack.
 *
 * Stack Transition:
 *
 *   ... -> ..., value
 *
 * Description:
 *
 *   The ldarg num instruction pushes onto the evaluation stack, the num'th incoming
 *   argument, where arguments are numbered 0 onwards (see Partition I). The type of the
 *   value on the stack is tracked by verification as the intermediate type (§I.8.7) of
 *   the argument type, as specified by the current method's signature.
 *
 *   The ldarg.0, ldarg.1, ldarg.2, and ldarg.3 instructions are efficient encodings for
 *   loading any one of the first 4 arguments. The ldarg.s instruction is an efficient
 *   encoding for loading argument numbers 4-255.
 *
 *   For procedures that take a variable-length argument list, the ldarg instructions
 *   can be used only for the initial fixed arguments, not those in the variable part of
 *   the signature. (See the arglist instruction.)
 *
 *   If required, arguments are converted to the representation of their intermediate
 *   type (§I.8.7) when loaded onto the stack (§III.1.1.1).
 *
 * Exceptions:
 *
 *   None.
 *
 * Correctness:
 *
 *   Correct CIL requires that num is a valid argument index.
 *
 * Verifiability:
 *
 *   Verification (§III.1.8) tracks the type of the value loaded onto the stack as the
 *   intermediate type (§I.8.7) of the argument.
 */
llvm::Error
MethodLLVMEmitter::emit_ldarg (MonoIrBuilder &builder, uint32_t index)
{
	if (index >= args.size ())
		return invalid_argument (index);

	const Entry &argument = args[index];

	return push_from_location (builder, argument.alloca, argument.type, argument.native);
}

/*
 * III.3.39  ldarga.<length> - load an argument address
 *
 *   Format                     Assembly Format    Description
 *   FE 0A <unsigned int16>     ldarga argNum      Fetch the address of argument argNum.
 *   0F <unsigned int8>         ldarga.s argNum    Fetch the address of argument argNum,
 *                                                 short form.
 *
 * Stack Transition:
 *
 *   ..., -> ..., address of argument number argNum
 *
 * Description:
 *
 *   The ldarga instruction fetches the address (of type &, i.e., managed pointer) of
 *   the argNum'th argument, where arguments are numbered 0 onwards. The address will
 *   always be aligned to a natural boundary on the target machine (cf. cpblk and
 *   initblk). The short form (ldarga.s) should be used for argument numbers 0-255. The
 *   result is a managed pointer (type &).
 *
 *   For procedures that take a variable-length argument list, the ldarga instructions
 *   can be used only for the initial fixed arguments, not those in the variable part of
 *   the signature.
 *
 *   [Rationale: ldarga is used for byref parameter passing (see Partition I). In other
 *   cases, ldarg and starg should be used. end rationale]
 *
 * Exceptions:
 *
 *   None.
 *
 * Correctness:
 *
 *   Correct CIL ensures that argNum is a valid argument index.
 *
 * Verifiability:
 *
 *   Verification (§III.1.8) tracks the type of the value loaded onto the stack as a
 *   managed pointer to the verification type (§I.8.7) of the argument.
 */
llvm::Error
MethodLLVMEmitter::emit_ldarga (MonoIrBuilder &builder, uint32_t index)
{
	if (index >= args.size ())
		return invalid_argument (index);

	const Entry &argument = args[index];
	MonoClass *klass = mono_class_from_mono_type_internal (argument.type);

	/*
	 * Every argument already lives in an alloca that its own type's alignment was
	 * asked for, which is the natural boundary the spec wants here.
	 */
	push_stack (argument.alloca, m_class_get_this_arg (klass));
	return llvm::Error::success ();
}

/*
 * III.3.61  starg.<length> - store a value in an argument slot
 *
 *   Format                     Assembly Format   Description
 *   FE 0B <unsigned int16>     starg num         Store value to the argument numbered
 *                                                num.
 *   10 <unsigned int8>         starg.s num       Store value to the argument numbered
 *                                                num, short form.
 *
 * Stack Transition:
 *
 *   ..., value -> ...,
 *
 * Description:
 *
 *   The starg num instruction pops a value from the stack and places it in argument
 *   slot num (see Partition I). The type of the value shall match the type of the
 *   argument, as specified in the current method's signature. The starg.s instruction
 *   provides an efficient encoding for use with the first 256 arguments.
 *
 *   For procedures that take a variable argument list, the starg instructions can be
 *   used only for the initial fixed arguments, not those in the variable part of the
 *   signature.
 *
 *   Storing into arguments that hold a value smaller than 4 bytes whose intermediate
 *   type is int32 truncates the value as it moves from the stack to the argument.
 *   Floating-point values are rounded from their native size (type F) to the size
 *   associated with the argument. (See §III.1.1.1, Numeric data types.)
 *
 * Exceptions:
 *
 *   None.
 *
 * Correctness:
 *
 *   Correct CIL requires that num is a valid argument slot.
 */
llvm::Error
MethodLLVMEmitter::emit_starg (MonoIrBuilder &builder, uint32_t index)
{
	if (index >= args.size ())
		return invalid_argument (index);
	if (stack.empty ())
		return unbalanced_stack (1);

	const Entry &argument = args[index];
	llvm::Expected<llvm::Value *> value =
		coerce_to_location (builder, get_stack (0), argument.type, argument.native);
	if (!value)
		return value.takeError ();

	pop_stack (1);
	if (held_in_memory (argument.type))
		copy_vtype (builder, argument.alloca, *value, argument.type, argument.native);
	else
		builder.CreateAlignedStore (*value, argument.alloca,
		                            type_alignment (argument.type, argument.native));
	return llvm::Error::success ();
}

} // namespace mono

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
	llvm::Expected<llvm::Type *> type = convert_type (local.type);
	if (!type)
		return type.takeError ();

	llvm::Value *value =
		builder.CreateAlignedLoad (*type, local.alloca, type_alignment (local.type));
	MonoType *pushed = local.type;

	/*
	 * A location narrower than four bytes is tracked as int32 once it is on the
	 * stack, and it is the location's own signedness - not the value's - that decides
	 * how the bits it gains get filled.
	 */
	if (!local.type->byref) {
		switch (mini_get_underlying_type (local.type)->type) {
		case MONO_TYPE_I1:
		case MONO_TYPE_I2:
			value = builder.CreateSExt (value, builder.getInt32Ty ());
			pushed = mono_get_int32_type ();
			break;
		case MONO_TYPE_BOOLEAN:
		case MONO_TYPE_CHAR:
		case MONO_TYPE_U1:
		case MONO_TYPE_U2:
			value = builder.CreateZExt (value, builder.getInt32Ty ());
			pushed = mono_get_int32_type ();
			break;
		default:
			break;
		}
	}

	push_stack (value, pushed);
	return llvm::Error::success ();
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

	/*
	 * The alloca is the address, and it is already aligned the way the local's own
	 * type asked for. A class's this_arg is its byval_arg with byref set, which is
	 * exactly the & the spec says to push.
	 */
	push_stack (local.alloca, m_class_get_this_arg (klass));
	return llvm::Error::success ();
}

/// VALUE as something that can be stored into a location of type DESTINATION.
///
/// The evaluation stack tracks fewer types than a location can hold, so this is where
/// an int32 narrows back into the byte or the char it came out of, and where an
/// address and a native int swap representations. Anything wider than that is a
/// mismatch the locals signature says cannot happen.
llvm::Expected<llvm::Value *>
MethodLLVMEmitter::coerce_to_location (MonoIrBuilder &builder, StackValue value,
                                       MonoType *destination)
{
	llvm::Expected<llvm::Type *> type = convert_type (destination);
	if (!type)
		return type.takeError ();

	llvm::Type *from = value.value->getType ();

	if (from == *type)
		return value.value;
	if (from->isIntegerTy () && (*type)->isIntegerTy ()
	    && (*type)->getIntegerBitWidth () < from->getIntegerBitWidth ())
		return builder.CreateTrunc (value.value, *type);
	if (from->isPointerTy () && (*type)->isIntegerTy ())
		return builder.CreatePtrToInt (value.value, *type);
	if (from->isIntegerTy () && (*type)->isPointerTy ())
		return builder.CreateIntToPtr (value.value, *type);
	/* The stack tracks one float type, so an R4 location rounds and an R8 one widens. */
	if (from->isFloatingPointTy () && (*type)->isFloatingPointTy ())
		return builder.CreateFPCast (value.value, *type);

	char *source_name = mono_type_full_name (value.type);
	char *destination_name = mono_type_full_name (destination);
	llvm::Error error = invalid_il (llvm::Twine ("cannot store a value of type ")
	                                + source_name + " into a location of type "
	                                + destination_name);

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
	llvm::Expected<llvm::Value *> value = coerce_to_location (builder, get_stack (0),
	                                                          local.type);
	if (!value)
		return value.takeError ();

	pop_stack (1);
	builder.CreateAlignedStore (*value, local.alloca, type_alignment (local.type));
	return llvm::Error::success ();
}

} // namespace mono

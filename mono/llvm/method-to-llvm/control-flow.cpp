#include "method-to-llvm.hpp"
#include "mono/metadata/loader.h"
#include "mono/metadata/metadata.h"

namespace mono {

/*
 * III.3.57  ret - return from method
 *
 *   Format   Assembly Format   Description
 *   2A       ret               Return from method, possibly with a value.
 *
 * Stack Transition:
 *
 *   retVal on callee evaluation stack (not always present) ->
 *   ..., retVal on caller evaluation stack (not always present)
 *
 * Description:
 *
 *   Return from the current method. The return type, if any, of the current method
 *   determines the type of value to be fetched from the top of the stack and copied
 *   onto the stack of the method that called the current method. The evaluation stack
 *   for the current method shall be empty except for the value to be returned.
 *
 *   The ret instruction cannot be used to transfer control out of a try, filter,
 *   catch, or finally block. From within a try or catch, use the leave instruction
 *   with a destination of a ret instruction that is outside all enclosing exception
 *   blocks. Because the filter and finally blocks are logically part of exception
 *   handling, and not part of the method in which their code is embedded, correctly
 *   generated CIL does not perform a method return from within a filter or finally.
 *
 *   If the return value is a value type and the caller expects a value type, the
 *   value is copied. If the return value is an object reference, only the reference
 *   is copied.
 *
 * Exceptions:
 *
 *   None.
 *
 * Correctness:
 *
 *   Correct CIL obeys the control constraints described above.
 *
 * Verifiability:
 *
 *   Verification requires that the type of retVal is verifier-assignable-to the
 *   return type declared by the current method.
 */
llvm::Error
MethodLLVMEmitter::emit_ret (MonoIrBuilder &builder)
{
	MonoType *ret = mono_method_signature_internal (method)->ret;

	if (ret->type == MONO_TYPE_VOID && !ret->byref) {
		if (!stack.empty ())
			return unbalanced_stack (0);

		builder.CreateRetVoid ();
		return llvm::Error::success ();
	}

	if (stack.size () != 1)
		return unbalanced_stack (1);

	/* The return slot is a location like any other, so it narrows the same way. */
	llvm::Expected<llvm::Value *> value = coerce_to_location (builder, get_stack (0), ret);
	if (!value)
		return value.takeError ();

	pop_stack (1);
	builder.CreateRet (*value);
	return llvm::Error::success ();
}

} // namespace mono

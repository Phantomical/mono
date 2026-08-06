#include "method-to-llvm.hpp"

namespace mono {

/*
 * III.3.33  dup - duplicate the top value of the stack
 *
 *   Format   Assembly Format   Description
 *   25       dup               Duplicate the value on the top of the stack.
 *
 * Stack Transition:
 *
 *   ..., value -> ..., value, value
 *
 * Description:
 *
 *   The dup instruction duplicates the top element of the stack.
 *
 * Exceptions:
 *
 *   None.
 *
 * Correctness and Verifiability:
 *
 *   No additional requirements.
 */
llvm::Error
MethodLLVMEmitter::emit_dup (MonoIrBuilder &builder)
{
	if (stack.empty ())
		return unbalanced_stack (1);

	StackValue value = get_stack (0);

	/*
	 * The two copies have to be independent: whatever the program does with one
	 * of them - hand its address to a callee, run a constructor over it - the
	 * other still holds what was duplicated.
	 */
	if (held_in_memory (value.type)) {
		llvm::Expected<llvm::Value *> slot = vtype_slot (value.type, value.native);

		if (!slot)
			return slot.takeError ();

		copy_vtype (builder, *slot, value.value, value.type, value.native);
		push_stack (*slot, value.type, value.native);
		return llvm::Error::success ();
	}

	push_stack (value.value, value.type, value.native);
	return llvm::Error::success ();
}

/*
 * III.3.54  pop - remove the top element of the stack
 *
 *   Format   Assembly Format   Description
 *   26       pop               Pop value from the stack.
 *
 * Stack Transition:
 *
 *   ..., value -> ...
 *
 * Description:
 *
 *   The pop instruction removes the top element from the stack.
 *
 * Exceptions:
 *
 *   None.
 *
 * Verifiability:
 *
 *   No additional requirements.
 */
llvm::Error
MethodLLVMEmitter::emit_pop ()
{
	if (stack.empty ())
		return unbalanced_stack (1);

	pop_stack (1);
	return llvm::Error::success ();
}

} // namespace mono

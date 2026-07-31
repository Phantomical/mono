#include "method-to-llvm.hpp"
#include "mono/metadata/metadata.h"
#include <llvm/IR/InstrTypes.h>
#include <llvm/IR/Type.h>

namespace mono {

/*
 * III.3.21  ceq - compare equal
 *
 *   Format   Assembly Format   Description
 *   FE 01    ceq               Push 1 (of type int32) if value1 equals value2, else
 *                              push 0.
 *
 * Stack Transition:
 *
 *   ..., value1, value2 -> ..., result
 *
 * Description:
 *
 *   The ceq instruction compares value1 and value2. If value1 is equal to value2, then
 *   1 (of type int32) is pushed on the stack. Otherwise, 0 (of type int32) is pushed on
 *   the stack.
 *
 *   For floating-point numbers, ceq will return 0 if the numbers are unordered (either
 *   or both are NaN). The infinite values are equal to themselves.
 *
 *   The acceptable operand types are encapsulated in Table 4: Binary Comparison or
 *   Branch Operations.
 *
 * Exceptions:
 *
 *   None.
 *
 * Correctness:
 *
 *   Correct CIL provides two values on the stack whose types match those specified in
 *   Table 4: Binary Comparison or Branch Operations
 *
 * Verifiability:
 *
 *   There are no additional verification requirements.
 *
 *
 * III.3.22  cgt - compare greater than
 *
 *   Format   Assembly Format   Description
 *   FE 02    cgt               Push 1 (of type int32) if value1 > value2, else push 0.
 *
 * Stack Transition:
 *
 *   ..., value1, value2 -> ..., result
 *
 * Description:
 *
 *   The cgt instruction compares value1 and value2. If value1 is strictly greater than
 *   value2, then 1 (of type int32) is pushed on the stack. Otherwise, 0 (of type int32)
 *   is pushed on the stack.
 *
 *   For floating-point numbers, cgt returns 0 if the numbers are unordered (that is, if
 *   one or both of the arguments are NaN).
 *
 *   As with IEC 60559:1989, infinite values are ordered with respect to normal numbers
 *   (e.g., +infinity > 5.0 > -infinity).
 *
 *
 * III.3.23  cgt.un - compare greater than, unsigned or unordered
 *
 *   Format   Assembly Format   Description
 *   FE 03    cgt.un            Push 1 (of type int32) if value1 > value2, unsigned or
 *                              unordered, else push 0.
 *
 * Stack Transition:
 *
 *   ..., value1, value2 -> ..., result
 *
 * Description:
 *
 *   The cgt.un instruction compares value1 and value2. A value of 1 (of type int32) is
 *   pushed on the stack if
 *
 *     - for floating-point numbers, either value1 is strictly greater than value2, or
 *       value1 is not ordered with respect to value2.
 *
 *     - for integer values, value1 is strictly greater than value2 when considered as
 *       unsigned numbers.
 *
 *   Otherwise, 0 (of type int32) is pushed on the stack.
 *
 *
 * III.3.25  clt - compare less than
 *
 *   Format   Assembly Format   Description
 *   FE 04    clt               Push 1 (of type int32) if value1 < value2, else push 0.
 *
 * Stack Transition:
 *
 *   ..., value1, value2 -> ..., result
 *
 * Description:
 *
 *   The clt instruction compares value1 and value2. If value1 is strictly less than
 *   value2, then 1 (of type int32) is pushed on the stack. Otherwise, 0 (of type int32)
 *   is pushed on the stack.
 *
 *   For floating-point numbers, clt will return 0 if the numbers are unordered (that
 *   is, one or both of the arguments are NaN).
 *
 *
 * III.3.26  clt.un - compare less than, unsigned or unordered
 *
 *   Format   Assembly Format   Description
 *   FE 05    clt.un            Push 1 (of type int32) if value1 < value2, unsigned or
 *                              unordered, else push 0.
 *
 * Stack Transition:
 *
 *   ..., value1, value2 -> ..., result
 *
 * Description:
 *
 *   The clt.un instruction compares value1 and value2. A value of 1 (of type int32) is
 *   pushed on the stack if
 *
 *     - for floating-point numbers, either value1 is strictly less than value2, or
 *       value1 is not ordered with respect to value2.
 *
 *     - for integer values, value1 is strictly less than value2 when considered as
 *       unsigned numbers.
 *
 *   Otherwise, 0 (of type int32) is pushed on the stack.
 */
llvm::Error
MethodLLVMEmitter::emit_compare (MonoIrBuilder &builder, BinaryOp op)
{
	/*
	 * Table III.4's object-reference cell admits one comparison beyond equality, by
	 * footnote: "cgt.un is allowed and verifiable on ObjectRefs (O). This is
	 * commonly used when comparing an ObjectRef with null (there is no
	 * 'compare-not-equal' instruction, which would otherwise be a more obvious
	 * solution)." The footnote names only the compare, not bgt.un, so it is handled
	 * here rather than in the shared table.
	 */
	llvm::Value *result;

	if (op == BinaryOp::BgtUn && stack.size () >= 2
	    && stack_type (get_stack (1).type) == ObjectRef
	    && stack_type (get_stack (0).type) == ObjectRef) {
		result = builder.CreateICmp (llvm::CmpInst::ICMP_UGT, get_stack (1).value,
		                             get_stack (0).value);
		pop_stack (2);
	} else {
		llvm::Expected<llvm::Value *> condition = emit_comparison (builder, op);

		if (!condition)
			return condition.takeError ();
		result = *condition;
	}

	push_stack (builder.CreateZExt (result, builder.getInt32Ty ()), mono_get_int32_type ());
	return llvm::Error::success ();
}

} // namespace mono

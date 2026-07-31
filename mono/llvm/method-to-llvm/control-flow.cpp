#include "method-to-llvm.hpp"
#include "mono/metadata/loader.h"
#include "mono/metadata/metadata.h"
#include <llvm/IR/Constants.h>
#include <llvm/IR/Instructions.h>
#include <llvm/Support/ErrorHandling.h>

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

namespace {

/// How OP compares two integers.
llvm::CmpInst::Predicate
integer_predicate (BinaryOp op)
{
	switch (op) {
	case BinaryOp::Beq:
		return llvm::CmpInst::ICMP_EQ;
	case BinaryOp::Bge:
		return llvm::CmpInst::ICMP_SGE;
	case BinaryOp::Bgt:
		return llvm::CmpInst::ICMP_SGT;
	case BinaryOp::Ble:
		return llvm::CmpInst::ICMP_SLE;
	case BinaryOp::Blt:
		return llvm::CmpInst::ICMP_SLT;
	case BinaryOp::BneUn:
		return llvm::CmpInst::ICMP_NE;
	case BinaryOp::BgeUn:
		return llvm::CmpInst::ICMP_UGE;
	case BinaryOp::BgtUn:
		return llvm::CmpInst::ICMP_UGT;
	case BinaryOp::BleUn:
		return llvm::CmpInst::ICMP_ULE;
	case BinaryOp::BltUn:
		return llvm::CmpInst::ICMP_ULT;
	default:
		llvm::report_fatal_error ("integer_predicate: not a comparison");
	}
}

/// How OP compares two floats.
///
/// The .un forms are the unordered ones, which is what makes a NaN operand branch: the
/// CLI spells "unsigned or unordered" with one suffix, and on F it means the second.
llvm::CmpInst::Predicate
float_predicate (BinaryOp op)
{
	switch (op) {
	case BinaryOp::Beq:
		return llvm::CmpInst::FCMP_OEQ;
	case BinaryOp::Bge:
		return llvm::CmpInst::FCMP_OGE;
	case BinaryOp::Bgt:
		return llvm::CmpInst::FCMP_OGT;
	case BinaryOp::Ble:
		return llvm::CmpInst::FCMP_OLE;
	case BinaryOp::Blt:
		return llvm::CmpInst::FCMP_OLT;
	case BinaryOp::BneUn:
		return llvm::CmpInst::FCMP_UNE;
	case BinaryOp::BgeUn:
		return llvm::CmpInst::FCMP_UGE;
	case BinaryOp::BgtUn:
		return llvm::CmpInst::FCMP_UGT;
	case BinaryOp::BleUn:
		return llvm::CmpInst::FCMP_ULE;
	case BinaryOp::BltUn:
		return llvm::CmpInst::FCMP_ULT;
	default:
		llvm::report_fatal_error ("float_predicate: not a comparison");
	}
}

} // namespace

/// Pop the two operands Table III.4 allows for OP and compare them, leaving the i1.
///
/// The table's cell says what the two are compared as rather than what is left behind:
/// an int32 against a native int is settled at native int width, and two managed
/// pointers as addresses.
llvm::Expected<llvm::Value *>
MethodLLVMEmitter::emit_comparison (MonoIrBuilder &builder, BinaryOp op)
{
	llvm::Expected<BinaryOperands> operands = pop_binary_operands (op);
	if (!operands)
		return operands.takeError ();

	auto [value1, value2, common] = *operands;
	llvm::Expected<llvm::Type *> type = convert_type (common);
	if (!type)
		return type.takeError ();

	llvm::Value *lhs = coerce (builder, value1.value, *type);
	llvm::Value *rhs = coerce (builder, value2.value, *type);

	if ((*type)->isFloatingPointTy ())
		return builder.CreateFCmp (float_predicate (op), lhs, rhs);
	return builder.CreateICmp (integer_predicate (op), lhs, rhs);
}

/*
 * III.3.15  br.<length> - unconditional branch
 *
 *   Format        Assembly Format   Description
 *   38 <int32>    br target         Branch to target.
 *   2B <int8>     br.s target       Branch to target, short form.
 *
 * Stack Transition:
 *
 *   ..., -> ...,
 *
 * Description:
 *
 *   The br instruction unconditionally transfers control to target. target is
 *   represented as a signed offset (4 bytes for br, 1 byte for br.s) from the beginning
 *   of the instruction following the current instruction.
 *
 *   If the target instruction has one or more prefix codes, control can only be
 *   transferred to the first of these prefixes.
 *
 *   Control transfers into and out of try, catch, filter, and finally blocks cannot be
 *   performed by this instruction. (Such transfers are severely restricted and shall
 *   use the leave instruction instead; see Partition I for details).
 *
 * Exceptions:
 *
 *   None.
 *
 * Correctness:
 *
 *   Correct CIL shall observe all of the control transfer rules specified above.
 *
 * Verifiability:
 *
 *   Verifiable code requires the type-consistency of the stack, locals and arguments
 *   for every possible path to the destination instruction. See §III.1.8 for more
 *   details.
 */
llvm::Error
MethodLLVMEmitter::emit_br (MonoIrBuilder &builder, int32_t displacement)
{
	llvm::Expected<size_t> target = branch_target (displacement);
	if (!target)
		return target.takeError ();

	if (llvm::Error error = enter_block (*target, spill_stack (builder)))
		return error;

	builder.CreateBr (blocks[*target].block);
	return llvm::Error::success ();
}

/*
 * III.3.17  brfalse.<length> - branch on false, null, or zero
 *
 *   Format       Assembly Format   Description
 *   39 <int32>   brfalse target    Branch to target if value is zero (false).
 *   2C <int8>    brfalse.s target  Branch to target if value is zero (false), short
 *                                  form.
 *   39 <int32>   brnull target     Branch to target if value is null (alias for
 *                                  brfalse).
 *   2C <int8>    brnull.s target   Branch to target if value is null (alias for
 *                                  brfalse.s), short form.
 *   39 <int32>   brzero target     Branch to target if value is zero (alias for
 *                                  brfalse).
 *   2C <int8>    brzero.s target   Branch to target if value is zero (alias for
 *                                  brfalse.s), short form.
 *
 * Stack Transition:
 *
 *   ..., value -> ...
 *
 * Description:
 *
 *   The brfalse instruction transfers control to target if value (of type int32,
 *   int64, object reference, managed pointer, unmanaged pointer or native int) is zero
 *   (false). If value is non-zero (true), execution continues at the next instruction.
 *
 *   Target is represented as a signed offset (4 bytes for brfalse, 1 byte for
 *   brfalse.s) from the beginning of the instruction following the current instruction.
 *
 * Exceptions:
 *
 *   None.
 *
 *
 * III.3.18  brtrue.<length> - branch on non-false or non-null
 *
 *   Format       Assembly Format   Description
 *   3A <int32>   brtrue target     Branch to target if value is non-zero (true).
 *   2D <int8>    brtrue.s target   Branch to target if value is non-zero (true), short
 *                                  form.
 *   3A <int32>   brinst target     Branch to target if value is a non-null object
 *                                  reference (alias for brtrue).
 *   2D <int8>    brinst.s target   Branch to target if value is a non-null object
 *                                  reference, short form (alias for brtrue.s).
 *
 * Stack Transition:
 *
 *   ..., value -> ...
 *
 * Description:
 *
 *   The brtrue instruction transfers control to target if value (of type native int) is
 *   nonzero (true). If value is zero (false) execution continues at the next
 *   instruction.
 *
 *   If the value is an object reference (type O) then brinst (an alias for brtrue)
 *   transfers control if it represents an instance of an object (i.e., isn't the null
 *   object reference, see ldnull).
 *
 * Exceptions:
 *
 *   None.
 */
llvm::Error
MethodLLVMEmitter::emit_brcond (MonoIrBuilder &builder, int32_t displacement, bool branch_if_true)
{
	if (stack.empty ())
		return unbalanced_stack (1);

	StackValue value = get_stack (0);
	StackType type = stack_type (value.type);

	/* Everything the two sections list, which is everything but F. */
	if (type == Float || type == Invalid)
		return invalid_il (llvm::Twine (branch_if_true ? "brtrue" : "brfalse")
		                   + " is not defined for operand type "
		                   + describe (value.type, type));

	llvm::Expected<size_t> target = branch_target (displacement);
	if (!target)
		return target.takeError ();

	/* The tested value is gone by the time either edge is taken. */
	pop_stack (1);

	std::vector<Slot> slots = spill_stack (builder);

	if (llvm::Error error = enter_block (*target, slots))
		return error;
	if (llvm::Error error = enter_block (ip, slots))
		return error;

	llvm::Value *condition = branch_if_true ? builder.CreateIsNotNull (value.value)
	                                        : builder.CreateIsNull (value.value);

	builder.CreateCondBr (condition, blocks[*target].block, blocks[ip].block);
	return llvm::Error::success ();
}

/*
 * III.3.5 through III.3.14 - branch on comparison
 *
 *   Format       Assembly Format   Description
 *   3B <int32>   beq target        Branch to target if equal.
 *   2E <int8>    beq.s target      Branch to target if equal, short form.
 *   3C <int32>   bge target        Branch to target if greater than or equal to.
 *   2F <int8>    bge.s target      Branch to target if greater than or equal to, short
 *                                  form.
 *   41 <int32>   bge.un target     Branch to target if greater than or equal to
 *                                  (unsigned or unordered).
 *   34 <int8>    bge.un.s target   Branch to target if greater than or equal to
 *                                  (unsigned or unordered), short form.
 *   3D <int32>   bgt target        Branch to target if greater than.
 *   30 <int8>    bgt.s target      Branch to target if greater than, short form.
 *   42 <int32>   bgt.un target     Branch to target if greater than (unsigned or
 *                                  unordered).
 *   35 <int8>    bgt.un.s target   Branch to target if greater than (unsigned or
 *                                  unordered), short form.
 *   3E <int32>   ble target        Branch to target if less than or equal to.
 *   31 <int8>    ble.s target      Branch to target if less than or equal to, short
 *                                  form.
 *   43 <int32>   ble.un target     Branch to target if less than or equal to (unsigned
 *                                  or unordered).
 *   36 <int8>    ble.un.s target   Branch to target if less than or equal to (unsigned
 *                                  or unordered), short form.
 *   3F <int32>   blt target        Branch to target if less than.
 *   32 <int8>    blt.s target      Branch to target if less than, short form.
 *   44 <int32>   blt.un target     Branch to target if less than (unsigned or
 *                                  unordered).
 *   37 <int8>    blt.un.s target   Branch to target if less than (unsigned or
 *                                  unordered), short form.
 *   40 <int32>   bne.un target     Branch to target if unequal or unordered.
 *   33 <int8>    bne.un.s target   Branch to target if unequal or unordered, short
 *                                  form.
 *
 * Stack Transition:
 *
 *   ..., value1, value2 -> ...
 *
 * Description:
 *
 *   The beq instruction transfers control to target if value1 is equal to value2. The
 *   effect is identical to performing a ceq instruction followed by a brtrue target.
 *   target is represented as a signed offset (4 bytes for beq, 1 byte for beq.s) from
 *   the beginning of the instruction following the current instruction.
 *
 *   The acceptable operand types are encapsulated in Table 4: Binary Comparison or
 *   Branch Operations.
 *
 *   If the target instruction has one or more prefix codes, control can only be
 *   transferred to the first of these prefixes.
 *
 *   Control transfers into and out of try, catch, filter, and finally blocks cannot be
 *   performed by this instruction. (Such transfers are severely restricted and shall
 *   use the leave instruction instead; see Partition I for details).
 *
 * Exceptions:
 *
 *   None.
 *
 * Correctness:
 *
 *   Correct CIL shall observe all of the control transfer rules specified above and
 *   shall guarantee that the top two items on the stack correspond to the types shown
 *   in Table 4: Binary Comparison or Branch Operations.
 *
 * Verifiability:
 *
 *   Verifiable code requires the type-consistency of the stack, locals and arguments
 *   for every possible path to the destination instruction. See §III.1.8 for more
 *   details.
 */
llvm::Error
MethodLLVMEmitter::emit_branch_compare (MonoIrBuilder &builder, BinaryOp op, int32_t displacement)
{
	llvm::Expected<llvm::Value *> condition = emit_comparison (builder, op);
	if (!condition)
		return condition.takeError ();

	llvm::Expected<size_t> target = branch_target (displacement);
	if (!target)
		return target.takeError ();

	std::vector<Slot> slots = spill_stack (builder);

	if (llvm::Error error = enter_block (*target, slots))
		return error;
	if (llvm::Error error = enter_block (ip, slots))
		return error;

	builder.CreateCondBr (*condition, blocks[*target].block, blocks[ip].block);
	return llvm::Error::success ();
}

/*
 * III.3.66  switch - table switch based on value
 *
 *   Format                             Assembly Format        Description
 *   45 <unsigned int32> <int32>...     switch ( t1, t2 ... tN )  Jump to one of n
 *   <int32>                                                      values.
 *
 * Stack Transition:
 *
 *   ..., value -> ...,
 *
 * Description:
 *
 *   The switch instruction implements a jump table. The format of the instruction is an
 *   unsigned int32 representing the number of targets N, followed by N int32 values
 *   specifying jump targets: these targets are represented as offsets (positive or
 *   negative) from the beginning of the instruction following this switch instruction.
 *
 *   The switch instruction pops value off the stack and compares it, as an unsigned
 *   integer, to n. If value is less than n, execution is transferred to the value'th
 *   target, where targets are numbered from 0 (i.e., a value of 0 takes the first
 *   target, a value of 1 takes the second target, and so on). If value is not less than
 *   n, execution continues at the next instruction (fall through).
 *
 * Exceptions:
 *
 *   None.
 *
 * Correctness:
 *
 *   Correct CIL obeys the control transfer constraints listed above.
 */
llvm::Error
MethodLLVMEmitter::emit_switch (MonoIrBuilder &builder)
{
	llvm::Expected<uint32_t> count = read_u32 ();
	if (!count)
		return count.takeError ();

	std::vector<int32_t> displacements;

	for (uint32_t i = 0; i < *count; ++i) {
		llvm::Expected<uint32_t> displacement = read_u32 ();

		if (!displacement)
			return displacement.takeError ();

		displacements.push_back (static_cast<int32_t> (*displacement));
	}

	/* Every displacement is relative to here, which is why they are all read first. */
	if (stack.empty ())
		return unbalanced_stack (1);

	StackValue value = get_stack (0);
	StackType type = stack_type (value.type);

	if (type != Int32 && type != NativeInt)
		return invalid_il (llvm::Twine ("switch is not defined for operand type ")
		                   + describe (value.type, type));

	pop_stack (1);

	llvm::Value *index = value.value;

	if (index->getType ()->isPointerTy ())
		index = builder.CreatePtrToInt (index,
		                                builder.getIntNTy (TARGET_SIZEOF_VOID_P * 8));

	std::vector<Slot> slots = spill_stack (builder);

	if (llvm::Error error = enter_block (ip, slots))
		return error;

	/*
	 * Switching at the index's own width rather than truncating to int32 is what makes
	 * "compares it, as an unsigned integer, to n" come out right for a native int: a
	 * value that matches no case falls through, and no wide index can alias a case by
	 * losing its high half.
	 */
	llvm::SwitchInst *jump = builder.CreateSwitch (index, blocks[ip].block, *count);

	for (uint32_t i = 0; i < *count; ++i) {
		llvm::Expected<size_t> target = branch_target (displacements[i]);

		if (!target)
			return target.takeError ();
		if (llvm::Error error = enter_block (*target, slots))
			return error;

		jump->addCase (llvm::ConstantInt::get (
				       llvm::cast<llvm::IntegerType> (index->getType ()), i),
		               blocks[*target].block);
	}

	return llvm::Error::success ();
}

/*
 * III.3.16  break - breakpoint instruction
 *
 *   Format   Assembly Format   Description
 *   01       break             Inform a debugger that a breakpoint has been reached.
 *
 * Stack Transition:
 *
 *   ..., -> ...
 *
 * Description:
 *
 *   The break instruction is for debugging support. It signals the CLI to inform the
 *   debugger that a break point has been tripped. It has no other effect on the
 *   interpreter state.
 *
 *   The break instruction has the smallest possible instruction size so that code can
 *   be patched with a breakpoint with minimal disturbance to the surrounding code.
 *
 *   The break instruction might trap to a debugger, do nothing, or raise a security
 *   exception: the exact behavior is implementation-defined.
 *
 * Exceptions:
 *
 *   None.
 *
 * Verifiability:
 *
 *   The break instruction is always verifiable.
 */
llvm::Error
MethodLLVMEmitter::emit_break (MonoIrBuilder &builder)
{
	builder.CreateIntrinsic (llvm::Intrinsic::debugtrap, {}, {});
	return llvm::Error::success ();
}

} // namespace mono

#include "method-to-llvm.hpp"
#include "mono/metadata/class-internals.h"
#include "mono/metadata/metadata.h"
#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Instructions.h>
#include <llvm/Support/ErrorHandling.h>

#include <algorithm>

namespace mono {

namespace {

/// The runtime call that throws an object already on the stack, or throws the one a
/// catch handler is holding a second time without disturbing its trace.
llvm::FunctionCallee
throw_decl (llvm::Module *module, const char *name)
{
	llvm::LLVMContext &ctx = module->getContext ();
	llvm::FunctionCallee callee = module->getOrInsertFunction (
		name, llvm::Type::getVoidTy (ctx), llvm::PointerType::get (ctx, 0));

	if (auto *function = llvm::dyn_cast<llvm::Function> (callee.getCallee ())) {
		function->setDoesNotReturn ();
		function->addFnAttr (llvm::Attribute::Cold);
	}

	return callee;
}

} // namespace

/// The innermost clause whose try region covers AT, or -1 if nothing protects it.
///
/// Clauses arrive innermost-first for a nest, but overlapping regions from separate
/// nests do not order themselves, so the shortest try wins rather than the first.
int
MethodLLVMEmitter::innermost_try (size_t at) const
{
	int found = -1;

	for (uint32_t i = 0; i < num_clauses; ++i) {
		MonoExceptionClause *clause = &clauses[i];

		if (!MONO_OFFSET_IN_CLAUSE (clause, at))
			continue;
		if (found < 0 || clause->try_len < clauses[found].try_len)
			found = static_cast<int> (i);
	}

	return found;
}

/// The clause whose handler or filter covers AT, or -1 if AT is not in one.
int
MethodLLVMEmitter::innermost_handler (size_t at) const
{
	int found = -1;

	for (uint32_t i = 0; i < num_clauses; ++i) {
		MonoExceptionClause *clause = &clauses[i];

		if (!MONO_OFFSET_IN_HANDLER (clause, at) && !MONO_OFFSET_IN_FILTER (clause, at))
			continue;
		if (found < 0 || clause->handler_len < clauses[found].handler_len)
			found = static_cast<int> (i);
	}

	return found;
}

/// Where a throw inside CLAUSE's try region lands.
///
/// Mono's unwinder does the whole two-pass search itself out of MonoJitInfo and resumes
/// at the handler's native address, so nothing here dispatches: the pad exists so that
/// LLVM emits a call-site entry covering the try region, which is what the LSDA
/// transcoder turns into the native range the unwinder matches the faulting IP against.
/// Its resume is unreachable in practice.
///
/// A catch handler therefore has no edge into it from anywhere in this function yet, so
/// nothing stops LLVM deleting one. Keeping them alive needs whatever ends up recording
/// their native addresses to say so, which is the engine's half of this.
llvm::BasicBlock *
MethodLLVMEmitter::landing_pad (uint32_t clause)
{
	Clause &state = clause_state[clause];

	if (state.pad != nullptr)
		return state.pad;

	state.pad = llvm::BasicBlock::Create (context (),
	                                      llvm::Twine ("pad") + llvm::Twine (clause),
	                                      function);

	MonoIrBuilder pad (state.pad);
	llvm::Type *exception = llvm::PointerType::get (context (), 0);
	llvm::LandingPadInst *caught =
		pad.CreateLandingPad (llvm::StructType::get (exception, pad.getInt32Ty ()), 0);

	caught->setCleanup (true);
	pad.CreateResume (caught);

	return state.pad;
}

/// Emit a call that unwinds, as an invoke when a clause in this method protects the
/// instruction being translated.
///
/// This is the whole of "am I inside a try": a call inside a protected region has to be
/// an invoke or LLVM records the range as nounwind and the unwinder has nothing to
/// match. The invoke's normal edge is dead - none of these callees return.
void
MethodLLVMEmitter::emit_unwinding_call (MonoIrBuilder &builder, llvm::FunctionCallee callee,
                                        llvm::ArrayRef<llvm::Value *> args)
{
	int clause = innermost_try (offset);

	if (clause >= 0) {
		llvm::BasicBlock *unwound =
			llvm::BasicBlock::Create (context (), "unwound", function);

		builder.CreateInvoke (callee, unwound, landing_pad (clause), args);
		builder.SetInsertPoint (unwound);
	} else {
		builder.CreateCall (callee, args);
	}

	builder.CreateUnreachable ();
}

/// Emit a call that returns but may unwind, as an invoke when a clause protects it.
///
/// The same decision emit_unwinding_call makes, for the callees that come back: the
/// normal edge carries on with the translation instead of being dead.
llvm::Value *
MethodLLVMEmitter::emit_protected_call (MonoIrBuilder &builder, llvm::FunctionCallee callee,
                                        llvm::ArrayRef<llvm::Value *> args)
{
	int clause = innermost_try (offset);

	if (clause < 0)
		return builder.CreateCall (callee, args);

	llvm::BasicBlock *returned =
		llvm::BasicBlock::Create (context (), "returned", function);
	llvm::InvokeInst *call =
		builder.CreateInvoke (callee, returned, landing_pad (clause), args);

	builder.SetInsertPoint (returned);
	return call;
}

/*
 * III.4.31  throw - throw an exception
 *
 *   Format   Assembly Format   Description
 *   7A       throw             Throw an exception.
 *
 * Stack Transition:
 *
 *   ..., object -> ...,
 *
 * Description:
 *
 *   The throw instruction throws the exception object (type O) on the stack and empties
 *   the stack. For details of the exception mechanism, see Partition I.
 *
 * Exceptions:
 *
 *   System.NullReferenceException is thrown if obj is null.
 *
 * Correctness:
 *
 *   Correct CIL ensures that object is always either null or an object reference (i.e.,
 *   of type O).
 */
llvm::Error
MethodLLVMEmitter::emit_throw (MonoIrBuilder &builder)
{
	if (stack.empty ())
		return unbalanced_stack (1);

	StackValue value = get_stack (0);
	StackType type = stack_type (value.type);

	if (type != ObjectRef)
		return invalid_il (llvm::Twine ("throw is not defined for operand type ")
		                   + describe (value.type, type));

	/* The stack is emptied whatever happens next, so nothing survives to be spilled. */
	pop_stack (stack.size ());
	emit_unwinding_call (builder, throw_decl (module, "mono_llvm_throw_exception"),
	                     { value.value });
	return llvm::Error::success ();
}

/*
 * III.4.24  rethrow - rethrow the current exception
 *
 *   Format    Assembly Format   Description
 *   FE 1A     rethrow           Rethrow the current exception.
 *
 * Description:
 *
 *   The rethrow instruction is only permitted within the body of a catch handler. It
 *   throws the same exception that was caught by this handler, and the original stack
 *   trace is preserved.
 */
llvm::Error
MethodLLVMEmitter::emit_rethrow (MonoIrBuilder &builder)
{
	int clause = innermost_handler (offset);

	if (clause < 0 || clauses[clause].flags == MONO_EXCEPTION_CLAUSE_FINALLY
	    || clauses[clause].flags == MONO_EXCEPTION_CLAUSE_FAULT)
		return invalid_il ("rethrow is only valid inside a catch handler");

	/*
	 * The exception a catch handler was entered with is the one on the bottom of its
	 * stack, put there by the runtime rather than by any instruction here.
	 */
	if (stack.empty ())
		return unbalanced_stack (1);

	StackValue caught = stack.front ();

	pop_stack (stack.size ());
	emit_unwinding_call (builder, throw_decl (module, "mono_llvm_rethrow_exception"),
	                     { caught.value });
	return llvm::Error::success ();
}

/*
 * III.3.46  leave.<length> - exit a protected region of code
 *
 *   Format       Assembly Format   Description
 *   DD <int32>   leave target      Exit a protected region of code.
 *   DE <int8>    leave.s target    Exit a protected region of code, short form.
 *
 * Description:
 *
 *   The leave instruction unconditionally transfers control to target. target is
 *   represented as a signed offset (4 bytes for leave, 1 byte for leave.s) from the
 *   beginning of the instruction following the current instruction.
 *
 *   The leave instruction is similar to the br instruction, but the former can be used
 *   to exit a try, filter, or catch block whereas the ordinary branch instructions can
 *   only be used in such a block to transfer control within it. The leave instruction
 *   empties the evaluation stack and ensures that the appropriate surrounding finally
 *   blocks are executed.
 *
 *   It is not valid to use a leave instruction to exit a finally block. To ease code
 *   generation for exception handlers it is valid from within a catch block to use a
 *   leave instruction to transfer control to any instruction within the associated try
 *   block.
 */
llvm::Error
MethodLLVMEmitter::emit_leave (MonoIrBuilder &builder, int32_t displacement)
{
	llvm::Expected<size_t> target = branch_target (displacement);
	if (!target)
		return target.takeError ();

	/*
	 * Every finally whose try region we are inside but the target is not, innermost
	 * first. A fault is not in the chain: it runs only when something went wrong, and
	 * a leave is an ordinary exit.
	 */
	std::vector<uint32_t> chain;

	for (uint32_t i = 0; i < num_clauses; ++i) {
		MonoExceptionClause *clause = &clauses[i];

		if (clause->flags != MONO_EXCEPTION_CLAUSE_FINALLY)
			continue;
		if (!MONO_OFFSET_IN_CLAUSE (clause, offset)
		    || MONO_OFFSET_IN_CLAUSE (clause, *target))
			continue;

		chain.push_back (i);
	}

	std::sort (chain.begin (), chain.end (), [&] (uint32_t a, uint32_t b) {
		return clauses[a].try_len < clauses[b].try_len;
	});

	/* leave empties the stack, so nothing has to reach either the finally or target. */
	pop_stack (stack.size ());

	if (llvm::Error error = enter_block (*target, {}))
		return error;

	llvm::BasicBlock *next = blocks[*target].block;

	/*
	 * Walk the chain backwards, building each step's continuation before the step that
	 * jumps to it: the last finally carries on to the target, and every earlier one
	 * carries on to the next finally.
	 */
	for (size_t i = chain.size (); i-- > 0;) {
		Clause &state = clause_state[chain[i]];
		uint32_t id = next_continuation++;
		llvm::BasicBlock *enter = llvm::BasicBlock::Create (
			context (), llvm::Twine ("call_finally") + llvm::Twine (id), function);
		MonoIrBuilder step (enter);

		state.continuations.push_back ({ id, next });
		step.CreateStore (step.getInt32 (id), state.resume_at);
		step.CreateBr (blocks[clauses[chain[i]].handler_offset].block);

		next = enter;
	}

	builder.CreateBr (next);
	return llvm::Error::success ();
}

/*
 * III.3.35  endfinally - end the finally or fault clause of an exception block
 *
 *   Format   Assembly Format   Description
 *   DC       endfault          End fault clause of an exception block.
 *   DC       endfinally        End finally clause of an exception block.
 *
 * Stack Transition:
 *
 *   ... -> ...
 *
 * Description:
 *
 *   Return from the finally or fault clause of an exception block (see the Exception
 *   Handling subclause of Partition I for details).
 *
 *   Signals the end of the finally or fault clause so that stack unwinding can continue
 *   until the exception handler is invoked. The endfinally or endfault instruction
 *   transfers control back to the CLI exception mechanism. This then searches for the
 *   next finally clause in the chain, if the protected block was exited with a leave
 *   instruction. If the protected block was exited with an exception, the CLI will
 *   search for the next finally or fault, or enter the exception handler chosen during
 *   the first pass of exception handling.
 */
llvm::Error
MethodLLVMEmitter::emit_endfinally (MonoIrBuilder &builder)
{
	int clause = innermost_handler (offset);

	if (clause < 0 || (clauses[clause].flags != MONO_EXCEPTION_CLAUSE_FINALLY
	                   && clauses[clause].flags != MONO_EXCEPTION_CLAUSE_FAULT))
		return invalid_il ("endfinally is only valid inside a finally or fault handler");

	pop_stack (stack.size ());

	/*
	 * A fault has only ever been entered by unwinding, so there is nothing to switch
	 * on: carrying on means carrying on unwinding, which the runtime does for us.
	 */
	if (clauses[clause].flags == MONO_EXCEPTION_CLAUSE_FAULT) {
		builder.CreateUnreachable ();
		return llvm::Error::success ();
	}

	llvm::BasicBlock *unwinding =
		llvm::BasicBlock::Create (context (), "resume_unwind", function);

	MonoIrBuilder (unwinding).CreateUnreachable ();

	Clause &state = clause_state[clause];
	llvm::Value *which =
		builder.CreateLoad (builder.getInt32Ty (), state.resume_at, "resume_at");

	/* The cases are filled in once every leave that reaches this block has been seen. */
	state.resume = builder.CreateSwitch (which, unwinding);
	return llvm::Error::success ();
}

/*
 * III.3.34  endfilter - end filter clause of SEH exception handling
 *
 *   Format    Assembly Format   Description
 *   FE 11     endfilter         End an exception handling filter clause.
 *
 * Stack Transition:
 *
 *   ..., value -> ...
 *
 * Description:
 *
 *   Return from filter clause of an exception block. value (which shall be of type
 *   int32 and one of a specific set of values) is returned from the filter clause. It
 *   shall be one of exception_continue_search (value 0) or exception_execute_handler
 *   (value 1).
 */
llvm::Error
MethodLLVMEmitter::emit_endfilter (MonoIrBuilder &builder)
{
	int clause = innermost_handler (offset);

	if (clause < 0 || !MONO_OFFSET_IN_FILTER (&clauses[clause], offset))
		return invalid_il ("endfilter is only valid inside a filter clause");
	if (stack.empty ())
		return unbalanced_stack (1);

	StackValue value = get_stack (0);
	StackType type = stack_type (value.type);

	if (type != Int32)
		return invalid_il (llvm::Twine ("endfilter is not defined for operand type ")
		                   + describe (value.type, type));

	/*
	 * A filter runs during the search pass, before anything has been unwound, so it
	 * hands its answer back to the runtime by returning it rather than by branching
	 * anywhere in this method.
	 */
	pop_stack (stack.size ());
	builder.CreateRet (value.value);
	return llvm::Error::success ();
}

/// Fill in each finally's endfinally switch, now that every leave that runs it has been
/// translated and knows which id it used.
llvm::Error
MethodLLVMEmitter::resolve_finally_switches ()
{
	for (uint32_t i = 0; i < num_clauses; ++i) {
		Clause &state = clause_state[i];

		if (state.continuations.empty ())
			continue;
		if (state.resume == nullptr)
			return invalid_il (llvm::Twine ("a leave runs the finally at IL_")
			                   + llvm::Twine::utohexstr (clauses[i].handler_offset)
			                   + " but it has no endfinally");

		for (auto &[id, continuation] : state.continuations)
			state.resume->addCase (
				llvm::ConstantInt::get (
					llvm::Type::getInt32Ty (context ()), id),
				continuation);
	}

	return llvm::Error::success ();
}

} // namespace mono

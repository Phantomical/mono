#include "method-to-llvm.hpp"
#include "hidden-return.hpp"
#include "../mono_lsda_format.hpp"
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

/// The runtime call a finally or fault makes when it was entered by unwinding and
/// has run out: hand control back to the unwinder, which knows where it was.
llvm::FunctionCallee
resume_unwind_decl (llvm::Module *module)
{
	llvm::LLVMContext &ctx = module->getContext ();
	llvm::FunctionCallee callee = module->getOrInsertFunction (
		"mono_llvm_resume_unwind", llvm::Type::getVoidTy (ctx));

	if (auto *function = llvm::dyn_cast<llvm::Function> (callee.getCallee ()))
		function->setDoesNotReturn ();

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

	/*
	 * A filter body runs during the search pass, before anything has been
	 * unwound; an exception it fails to contain makes the filter answer no,
	 * it is never dispatched into this frame's clauses. So nothing in a
	 * filter function is protected and its calls stay plain calls.
	 */
	if (filter_mode)
		return -1;

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

/// Whether clause J's try region strictly encloses clause C's.
///
/// The test is on the try regions' own extents; handler placement says nothing
/// about nesting. Siblings - identical try regions - are excluded: they share one
/// pad and are routed as a group, not by nesting.
static bool
clause_encloses (const MonoExceptionClause *c, const MonoExceptionClause *j)
{
	bool siblings = c->try_offset == j->try_offset && c->try_len == j->try_len;

	return !siblings && c->try_offset >= j->try_offset
	       && (uint64_t) c->try_offset + c->try_len
	              <= (uint64_t) j->try_offset + j->try_len;
}

/// Every clause CLAUSE's pad can be asked to dispatch, in the order the runtime
/// tries them: the clause's own sibling group first - a catch shares its pad with
/// every catch over the identical try region, in declaration order, so the more
/// derived type is tried first; a finally or fault owns its pad alone - then each
/// enclosing clause outward. Ascending clause index is innermost first for the
/// enclosers: ECMA-335 puts a nested clause before the clauses that enclose it.
std::vector<uint32_t>
MethodLLVMEmitter::covering_chain (uint32_t clause) const
{
	std::vector<uint32_t> chain;
	MonoExceptionClause *self = &clauses[clause];

	bool self_catches = self->flags == MONO_EXCEPTION_CLAUSE_NONE
	                    || self->flags == MONO_EXCEPTION_CLAUSE_FILTER;

	for (uint32_t j = 0; j < num_clauses; ++j) {
		MonoExceptionClause *c = &clauses[j];

		/* Filtered catches are siblings of plain ones over the same try. */
		if (self_catches) {
			if (c->flags != MONO_EXCEPTION_CLAUSE_NONE
			    && c->flags != MONO_EXCEPTION_CLAUSE_FILTER)
				continue;
			if (c->try_offset != self->try_offset || c->try_len != self->try_len)
				continue;
		} else if (j != clause) {
			continue;
		}

		chain.push_back (j);
	}

	for (uint32_t j = 0; j < num_clauses; ++j)
		if (clause_encloses (self, &clauses[j]))
			chain.push_back (j);

	return chain;
}

/// The global that stands for clause CLAUSE in the exception tables.
///
/// A landing pad's catch operands travel into the object's type table, and that is
/// the one channel through which a clause's identity survives codegen: the table
/// entry points at this global, whose two words are the clause index and the
/// clause's kind. The runtime side reads them back when it builds the method's
/// MonoJitInfo.
llvm::Constant *
MethodLLVMEmitter::clause_marker (uint32_t clause)
{
	std::string name = "mono_eh_clause_" + std::to_string (clause);

	if (llvm::GlobalVariable *existing = module->getNamedGlobal (name))
		return existing;

	llvm::Type *i32 = llvm::Type::getInt32Ty (context ());
	llvm::StructType *pair = llvm::StructType::get (i32, i32);
	llvm::Constant *value = llvm::ConstantStruct::get (
		pair, { llvm::ConstantInt::get (i32, clause),
	                llvm::ConstantInt::get (i32, clauses[clause].flags) });

	return new llvm::GlobalVariable (*module, pair, /*isConstant=*/true,
	                                 llvm::GlobalValue::PrivateLinkage, value, name);
}

/// The global that marks CLAUSE's resume pad in the exception tables.
///
/// Same channel as clause_marker, different meaning: the kind word says the pad is
/// where control sits once CLAUSE's cleanup has run, so the runtime dispatches the
/// enclosing clauses through it from then on rather than through the try's own pad.
llvm::Constant *
MethodLLVMEmitter::resume_marker (uint32_t clause)
{
	std::string name = "mono_eh_resume_" + std::to_string (clause);

	if (llvm::GlobalVariable *existing = module->getNamedGlobal (name))
		return existing;

	llvm::Type *i32 = llvm::Type::getInt32Ty (context ());
	llvm::StructType *pair = llvm::StructType::get (i32, i32);
	llvm::Constant *value = llvm::ConstantStruct::get (
		pair, { llvm::ConstantInt::get (i32, clause),
	                llvm::ConstantInt::get (i32, MONO_LSDA_KIND_RESUME_PAD) });

	return new llvm::GlobalVariable (*module, pair, /*isConstant=*/true,
	                                 llvm::GlobalValue::PrivateLinkage, value, name);
}

/// A block that enters clause CLAUSE's handler the way the runtime expects it to be
/// entered: a finally is told it was entered by unwinding, a catch is handed EXC.
llvm::BasicBlock *
MethodLLVMEmitter::handler_entry (uint32_t clause, llvm::Value *exc)
{
	MonoExceptionClause *info = &clauses[clause];
	Block &handler = blocks[info->handler_offset];
	llvm::BasicBlock *enter = llvm::BasicBlock::Create (
		context (), llvm::Twine ("enter_clause") + llvm::Twine (clause), function);
	MonoIrBuilder prep (enter);

	if (info->flags == MONO_EXCEPTION_CLAUSE_FINALLY) {
		/* Arriving by unwinding is continuation 0: hand control back
		 * to the unwinder afterwards. */
		enter_finally (prep, clause, 0);
	} else if (info->flags != MONO_EXCEPTION_CLAUSE_FAULT
	           && !handler.entry.empty ()) {
		prep.CreateStore (exc, handler.entry[0].alloca);
	}

	prep.CreateBr (handler.block);
	return enter;
}

/// Where a throw inside CLAUSE's try region lands.
///
/// The pad is what every invoke in the region unwinds to. Mono's own unwinder does
/// the two-pass search out of MonoJitInfo, picks the clause, and resumes here - at
/// the innermost pad of the throw site - with the exception and the chosen clause's
/// index in the two registers a landing pad reads. The switch is what routes that
/// entry to the chosen clause, which need not be the innermost: a catch the
/// innermost clause does not satisfy hands dispatch to an encloser through this
/// same pad - until a cleanup has run, after which the enclosers are reached
/// through that cleanup's own resume pad (emit_resume_exit).
///
/// The catch operands name every clause the switch can route to, so the object's
/// exception table carries the whole chain for each call site - which is also what
/// keeps every handler reachable and alive through optimization.
///
/// A handler is entered holding what the runtime puts there: a catch gets the
/// exception, a finally or fault nothing. Those entry stacks are recorded up front
/// in emit (), so this only has to fill the slot in.
llvm::BasicBlock *
MethodLLVMEmitter::landing_pad (uint32_t clause)
{
	Clause &state = clause_state[clause];

	if (state.pad != nullptr)
		return state.pad;

	state.pad = llvm::BasicBlock::Create (context (),
	                                      llvm::Twine ("pad") + llvm::Twine (clause), function);

	MonoIrBuilder pad (state.pad);
	llvm::Type *exception = llvm::PointerType::get (context (), 0);
	std::vector<uint32_t> chain = covering_chain (clause);
	llvm::LandingPadInst *caught = pad.CreateLandingPad (
		llvm::StructType::get (exception, pad.getInt32Ty ()),
		static_cast<unsigned> (chain.size ()));

	for (uint32_t j : chain)
		caught->addClause (clause_marker (j));

	llvm::Value *exc = pad.CreateExtractValue (caught, 0);
	llvm::Value *selector = pad.CreateExtractValue (caught, 1);

	/*
	 * The runtime only ever enters with one of the chain's indices; anything else
	 * is a table we built wrongly, and there is nowhere sensible to go.
	 */
	llvm::BasicBlock *impossible =
		llvm::BasicBlock::Create (context (),
		                          llvm::Twine ("pad") + llvm::Twine (clause) + "_bad",
		                          function);
	MonoIrBuilder (impossible).CreateUnreachable ();

	llvm::SwitchInst *dispatch = pad.CreateSwitch (selector, impossible,
	                                               static_cast<unsigned> (chain.size ()));

	for (uint32_t j : chain)
		dispatch->addCase (pad.getInt32 (j), handler_entry (j, exc));

	return state.pad;
}

/// Hand control back to the unwinder at the end of a cleanup it entered.
///
/// With nothing enclosing the clause the frame is done and a plain call suffices.
/// An enclosing clause makes it an invoke landing on a pad of this cleanup's own:
/// the runtime dispatches the enclosers through that pad from then on, and the pad
/// being reachable only from here is what hands their handlers the values the
/// cleanup just wrote - through the try's own pad they would get the state the
/// throw site had, as though the cleanup had never run.
void
MethodLLVMEmitter::emit_resume_exit (MonoIrBuilder &builder, uint32_t clause)
{
	std::vector<uint32_t> enclosers;

	for (uint32_t j = 0; j < num_clauses; ++j)
		if (j != clause && clause_encloses (&clauses[clause], &clauses[j]))
			enclosers.push_back (j);

	if (enclosers.empty ()) {
		builder.CreateCall (resume_unwind_decl (module));
		builder.CreateUnreachable ();
		return;
	}

	llvm::BasicBlock *cont =
		llvm::BasicBlock::Create (context (), "resume_cont", function);
	llvm::BasicBlock *pad_block = llvm::BasicBlock::Create (
		context (), llvm::Twine ("resume_pad") + llvm::Twine (clause), function);

	builder.CreateInvoke (resume_unwind_decl (module), cont, pad_block);
	MonoIrBuilder (cont).CreateUnreachable ();

	MonoIrBuilder pad (pad_block);
	llvm::LandingPadInst *caught = pad.CreateLandingPad (
		llvm::StructType::get (llvm::PointerType::get (context (), 0),
	                               pad.getInt32Ty ()),
		1);

	caught->addClause (resume_marker (clause));

	llvm::Value *exc = pad.CreateExtractValue (caught, 0);
	llvm::Value *selector = pad.CreateExtractValue (caught, 1);

	llvm::BasicBlock *impossible = llvm::BasicBlock::Create (
		context (),
		llvm::Twine ("resume_pad") + llvm::Twine (clause) + "_bad", function);
	MonoIrBuilder (impossible).CreateUnreachable ();

	llvm::SwitchInst *dispatch = pad.CreateSwitch (
		selector, impossible, static_cast<unsigned> (enclosers.size ()));

	for (uint32_t j : enclosers)
		dispatch->addCase (pad.getInt32 (j), handler_entry (j, exc));
}

/// Record on the way in how CLAUSE's handler is being entered.
///
/// CONTINUATION is the id its endfinally switches on: 0 for an entry by unwinding,
/// which carries on by resuming that unwind. The abort guard is cleared here rather
/// than at the top of the body, because from the body's first instruction on the byte
/// belongs to the runtime - another thread writes it while this one is in there.
void
MethodLLVMEmitter::enter_finally (MonoIrBuilder &builder, uint32_t clause,
                                  uint32_t continuation)
{
	Clause &state = clause_state[clause];

	builder.CreateStore (builder.getInt32 (continuation), state.resume_at);
	builder.CreateStore (builder.getInt8 (0), state.abort_guard);
}

/// Mark where CLAUSE's handler body begins or ends.
///
/// The pair is what MonoFinallyRangePass reads back after codegen to work out which
/// PCs the body occupies - the question find_last_handler_block () asks of a frame it
/// is about to guard. A stackmap answers it because it is an instruction: it is moved,
/// cloned and merged along with the code around it, where a block loses its identity
/// to the first merge that touches it. The opening one also names the guard byte, so
/// that its frame home can be recovered once the frame has been laid out.
void
MethodLLVMEmitter::emit_finally_body_marker (MonoIrBuilder &builder, uint32_t clause,
                                             bool opening)
{
	/* A filter body is a function of its own and holds no clause's frame state. */
	if (clause_state[clause].abort_guard == nullptr)
		return;

	uint64_t id = (opening ? MONO_LLVM_FINALLY_STACKMAP_ID_BASE
	                       : MONO_LLVM_FINALLY_END_STACKMAP_ID_BASE)
	              | clause;
	std::vector<llvm::Value *> args = { builder.getInt64 (id), builder.getInt32 (0) };

	if (opening)
		args.push_back (clause_state[clause].abort_guard);

	builder.CreateIntrinsic (llvm::Intrinsic::experimental_stackmap, {}, args);
}

/// Deliver an abort that arrived while CLAUSE's handler was running, now that it has.
///
/// A thread aborted inside a finally has to finish it first, so the request does not
/// raise anything: it sets a byte in this frame (install_handler_block_guard) and
/// leaves the delivery to the handler's own exit. The icall hands the abort to its
/// wrapper's interruption checkpoint, which raises it here - past the body, but still
/// inside whatever protects the handler, so it reaches the catch it would have reached
/// had it been raised on time.
///
/// Only on the way out through a leave. WHICH is the continuation the endfinally is
/// about to take, and 0 says the handler was entered by unwinding: an exception is
/// already on its way out of the frame and the runtime delivers the abort behind it.
llvm::Error
MethodLLVMEmitter::emit_finally_abort_check (MonoIrBuilder &builder, uint32_t clause,
                                             llvm::Value *which)
{
	if (clause_state[clause].abort_guard == nullptr)
		return llvm::Error::success ();

	llvm::Expected<llvm::Function *> finish =
		icall_wrapper_decl (MONO_JIT_ICALL_ves_icall_thread_finish_async_abort);

	if (!finish)
		return finish.takeError ();

	llvm::BasicBlock *test =
		llvm::BasicBlock::Create (context (), "abort_guard", function);
	llvm::BasicBlock *deliver =
		llvm::BasicBlock::Create (context (), "deliver_abort", function);
	llvm::BasicBlock *leaving =
		llvm::BasicBlock::Create (context (), "finally_left", function);

	builder.CreateCondBr (builder.CreateIsNotNull (which), test, leaving);

	MonoIrBuilder guard (test);
	llvm::Value *flagged = guard.CreateLoad (guard.getInt8Ty (),
	                                         clause_state[clause].abort_guard,
	                                         /* isVolatile */ true, "abort_pending");

	guard.CreateCondBr (guard.CreateIsNotNull (flagged), deliver, leaving);

	builder.SetInsertPoint (deliver);
	emit_protected_call (builder, *finish, adapt_to_callee (builder, *finish, {}));
	builder.CreateBr (leaving);

	builder.SetInsertPoint (leaving);
	return llvm::Error::success ();
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
/// normal edge carries on with the translation instead of being dead. DESCRIBE says
/// the rest of what the site is, on the call instruction itself, which is not always
/// what comes back from here.
///
/// ARGS are the callee's declared arguments. A callee whose return travels through a
/// hidden pointer is handed a slot of this frame's to fill in, and what comes back is
/// what it left there.
llvm::Value *
MethodLLVMEmitter::emit_protected_call (MonoIrBuilder &builder, llvm::FunctionCallee callee,
                                        llvm::ArrayRef<llvm::Value *> args,
                                        llvm::function_ref<void (llvm::CallBase *)> describe,
                                        llvm::Type *hidden, unsigned at)
{
	if (auto *target = llvm::dyn_cast<llvm::Function> (callee.getCallee ())) {
		hidden = hidden_return_type (target);
		at = hidden_return_index (target->arg_size ());
	}

	llvm::SmallVector<llvm::Value *, 8> operands (args.begin (), args.end ());
	llvm::AllocaInst *slot = nullptr;

	if (hidden != nullptr) {
		slot = entry_alloca (hidden, "retslot");
		operands.insert (operands.begin () + at, slot);
	}

	int clause = innermost_try (offset);
	llvm::CallBase *call;

	if (clause < 0) {
		llvm::CallInst *plain = builder.CreateCall (callee, operands);

		/*
		 * A managed frame is observable - stack traces, StackFrame, the
		 * runtime's own stack walks - so a call in tail position still has to
		 * be a call. Left unmarked, tailcallelim marks it `tail`, which either
		 * rewrites self-recursion into a loop or lets codegen hand this frame
		 * to the callee. Either way the frame stops existing.
		 */
		plain->setTailCallKind (llvm::CallInst::TCK_NoTail);
		call = plain;
	} else {
		llvm::BasicBlock *returned =
			llvm::BasicBlock::Create (context (), "returned", function);

		call = builder.CreateInvoke (callee, returned, landing_pad (clause),
		                             operands);
		builder.SetInsertPoint (returned);
	}

	if (hidden != nullptr)
		call->addParamAttrs (at, llvm::AttrBuilder (
					        context (),
					        hidden_return_attributes (context (), hidden)));
	if (describe)
		describe (call);

	if (hidden == nullptr)
		return call;
	return builder.CreateAlignedLoad (hidden, slot, slot->getAlign ());
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
/*
 * III.F0.1F  mono_rethrow - throw an exception again without disturbing its trace
 *
 * Unlike CIL rethrow this names the exception rather than taking the one the
 * handler was entered with, so a wrapper can rethrow what it caught after doing
 * something else in between.
 */
llvm::Error
MethodLLVMEmitter::emit_mono_rethrow (MonoIrBuilder &builder)
{
	if (stack.empty ())
		return unbalanced_stack (1);

	StackValue value = get_stack (0);
	StackType type = stack_type (value.type);

	if (type != ObjectRef)
		return invalid_il (llvm::Twine ("mono_rethrow is not defined for operand type ")
		                   + describe (value.type, type));

	pop_stack (stack.size ());
	emit_unwinding_call (builder, throw_decl (module, "mono_llvm_rethrow_exception"),
	                     {value.value});
	return llvm::Error::success ();
}

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
	                     {value.value});
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
	 * The exception is not a stack operand - the handler was handed it on entry,
	 * and by here the body has usually stored it away or discarded it. The value
	 * remembered at the handler's entry is the one the runtime put there.
	 */
	llvm::Value *caught = clause_state[clause].caught;

	if (caught == nullptr)
		return invalid_il ("rethrow is only valid inside a catch handler");

	pop_stack (stack.size ());
	emit_unwinding_call (builder, throw_decl (module, "mono_llvm_rethrow_exception"), {caught});
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
	 * Leaving a catch handler asks the runtime whether an undeniable exception
	 * is pending and rethrows it: a thread abort raised for an appdomain unload
	 * survives ResetAbort, and rethrowing it at every catch exit is what stops
	 * a handler from swallowing it and keeping the thread in the dying domain.
	 * Not in runtime-invoke wrappers, whose native callers expect the wrapper
	 * to catch everything.
	 */
	bool leaving_catch = false;

	for (uint32_t i = 0; i < num_clauses; ++i) {
		MonoExceptionClause *clause = &clauses[i];

		if (clause->flags == MONO_EXCEPTION_CLAUSE_NONE
		    && MONO_OFFSET_IN_HANDLER (clause, offset)
		    && ip <= (size_t) clause->handler_offset + clause->handler_len) {
			leaving_catch = true;
			break;
		}
	}

	if (leaving_catch && method->wrapper_type != MONO_WRAPPER_RUNTIME_INVOKE) {
		/*
		 * Through the icall wrapper, not as a bare call: the runtime answers by
		 * walking the stack from the last LMF, and the wrapper's is what makes
		 * the walk start here rather than at whichever frame saved one last.
		 */
		llvm::Expected<llvm::Function *> undeniable = icall_wrapper_decl (
			MONO_JIT_ICALL_mono_thread_get_undeniable_exception);

		if (!undeniable)
			return undeniable.takeError ();

		llvm::Value *pending = emit_protected_call (
			builder, *undeniable, adapt_to_callee (builder, *undeniable, {}));

		llvm::BasicBlock *rethrow = llvm::BasicBlock::Create (
			context (), "rethrow_undeniable", function);
		llvm::BasicBlock *carry_on = llvm::BasicBlock::Create (
			context (), "no_undeniable", function);

		builder.CreateCondBr (builder.CreateIsNotNull (pending), rethrow, carry_on);

		MonoIrBuilder thrower (rethrow);
		emit_unwinding_call (thrower, throw_decl (module, "mono_llvm_throw_exception"),
		                     {pending});
		builder.SetInsertPoint (carry_on);
	}

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

	if (llvm::Error error = enter_block (builder, *target, {}))
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

		state.continuations.push_back ({id, next});
		enter_finally (step, chain[i], id);
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

	if (clause < 0
	    || (clauses[clause].flags != MONO_EXCEPTION_CLAUSE_FINALLY
	        && clauses[clause].flags != MONO_EXCEPTION_CLAUSE_FAULT))
		return invalid_il ("endfinally is only valid inside a finally or fault handler");

	pop_stack (stack.size ());

	/*
	 * A fault has only ever been entered by unwinding, so there is nothing to switch
	 * on: carrying on means handing control back to the unwinder, which saved where
	 * it was before entering the handler.
	 */
	if (clauses[clause].flags == MONO_EXCEPTION_CLAUSE_FAULT) {
		emit_resume_exit (builder, static_cast<uint32_t> (clause));
		return llvm::Error::success ();
	}

	llvm::BasicBlock *unwinding =
		llvm::BasicBlock::Create (context (), "resume_unwind", function);
	MonoIrBuilder resume (unwinding);

	emit_resume_exit (resume, static_cast<uint32_t> (clause));

	Clause &state = clause_state[clause];
	llvm::Value *which =
		builder.CreateLoad (builder.getInt32Ty (), state.resume_at, "resume_at");

	/*
	 * The body is over from here, so a thread stopped past this point is no longer
	 * in it - which is what lets the abort check below raise rather than defer.
	 */
	emit_finally_body_marker (builder, static_cast<uint32_t> (clause), /* opening */ false);

	if (llvm::Error error =
	            emit_finally_abort_check (builder, static_cast<uint32_t> (clause), which))
		return error;

	/* The cases are filled in once every leave that reaches this block has been seen. */
	state.resume.push_back (builder.CreateSwitch (which, unwinding));
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

/// Fill in each finally's endfinally switches, now that every leave that runs it has been
/// translated and knows which id it used.
///
/// A handler whose every path throws or loops has no endfinally at all, and so no switch
/// to fill in: the leave's continuation is simply never resumed. That is legal IL, and
/// what C# emits for `finally { throw ...; }`.
void
MethodLLVMEmitter::resolve_finally_switches ()
{
	for (uint32_t i = 0; i < num_clauses; ++i) {
		Clause &state = clause_state[i];

		for (llvm::SwitchInst *resume : state.resume)
			for (auto &[id, continuation] : state.continuations)
				resume->addCase (
					llvm::ConstantInt::get (llvm::Type::getInt32Ty (context ()), id),
					continuation);
	}
}

} // namespace mono

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

/// Declares one of mono's throw entry points by name: mono_llvm_throw_exception for
/// a new throw, or mono_llvm_rethrow_exception for a rethrow that keeps the original
/// trace.
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

/// Declares mono_llvm_resume_unwind, the call a finally or fault makes when it was
/// entered by unwinding and control must return to the unwinder.
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

/// The innermost clause whose try region covers at, or -1 if nothing protects it.
///
/// Clauses arrive innermost-first within one nest. Overlapping regions from separate
/// nests do not order themselves, so the shortest try region wins instead of the
/// first.
int
MethodLLVMEmitter::innermost_try (size_t at) const
{
	int found = -1;

	/*
	 * A filter body runs during the search pass, before anything has unwound.
	 * If it fails to contain the exception, it answers no and the runtime
	 * never dispatches into this frame's clauses. So nothing in a filter
	 * function is protected, and its calls stay plain calls.
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

/// The clause whose handler or filter covers at, or -1 if no clause does.
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

/// Whether clause j's try region strictly encloses clause c's.
///
/// The test looks only at the try regions' own extents. Handler placement says
/// nothing about nesting. Siblings, which share an identical try region, are
/// excluded: they share one pad and route as a group, not by nesting.
static bool
clause_encloses (const MonoExceptionClause *c, const MonoExceptionClause *j)
{
	bool siblings = c->try_offset == j->try_offset && c->try_len == j->try_len;

	return !siblings && c->try_offset >= j->try_offset
	       && (uint64_t) c->try_offset + c->try_len
	              <= (uint64_t) j->try_offset + j->try_len;
}

/// Every clause that the clause's pad can dispatch to, in the order the runtime
/// tries them.
///
/// The clause's own sibling group comes first. A catch shares its pad with every
/// catch over the identical try region, tried in declaration order so the more
/// derived type comes first. A finally or fault owns its pad alone.
///
/// Enclosing clauses come next, ascending clause index, which is innermost first:
/// ECMA-335 lists a nested clause before the clauses that enclose it.
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

/// The global that stands for the clause in the exception tables.
///
/// A landing pad's catch operands point into the object's type table, which is the
/// one channel through which a clause's identity survives codegen. The table entry
/// names this global, whose two words are the clause index and the clause's kind.
/// The runtime reads them back to build the method's MonoJitInfo.
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

/// The global that marks the clause's resume pad in the exception tables.
///
/// This uses the same channel as clause_marker, but with a different meaning. The
/// kind word marks the pad as where control sits once the clause's cleanup has
/// run. From then on, the runtime dispatches the enclosing clauses through this
/// pad instead of through the try's own pad.
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

/// A block that enters the clause's handler the way the runtime expects.
///
/// A finally is told it was entered by unwinding, and a catch is handed exc.
llvm::BasicBlock *
MethodLLVMEmitter::handler_entry (uint32_t clause, llvm::Value *exc)
{
	MonoExceptionClause *info = &clauses[clause];
	Block &handler = blocks[info->handler_offset];
	llvm::BasicBlock *enter =
		create_cold_block (llvm::Twine ("enter_clause") + llvm::Twine (clause));
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

/// Where a throw inside the clause's try region lands.
///
/// The pad is what every invoke in the region unwinds to. Mono's own unwinder does
/// the two-pass search out of MonoJitInfo, picks a clause, and resumes at the
/// innermost pad of the throw site. It hands the pad the exception and the chosen
/// clause's index, in the two registers a landing pad reads.
///
/// The switch routes that entry to the chosen clause, which need not be the
/// innermost clause. If the innermost clause's catch cannot take the exception,
/// dispatch passes to an encloser through this same pad. Once a cleanup has run,
/// the runtime instead reaches the enclosers through that cleanup's own resume pad
/// (emit_resume_exit).
///
/// The catch operands name every clause the switch can route to, so the object's
/// exception table carries the whole chain for each call site. That also keeps
/// every handler reachable and alive through optimization.
///
/// A handler is entered holding what the runtime puts there: a catch gets the
/// exception, and a finally or fault gets nothing. emit () records those entry
/// stacks up front, so this only fills the slot in.
llvm::BasicBlock *
MethodLLVMEmitter::landing_pad (uint32_t clause)
{
	Clause &state = clause_state[clause];

	if (state.pad != nullptr)
		return state.pad;

	state.pad = create_cold_block (llvm::Twine ("pad") + llvm::Twine (clause));

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
	 * The runtime only ever enters with one of the chain's indices. Anything else
	 * means we built the table wrongly, and there is nowhere sensible to go.
	 */
	llvm::BasicBlock *impossible =
		create_cold_block (llvm::Twine ("pad") + llvm::Twine (clause) + "_bad");
	MonoIrBuilder (impossible).CreateUnreachable ();

	llvm::SwitchInst *dispatch = pad.CreateSwitch (selector, impossible,
	                                               static_cast<unsigned> (chain.size ()));

	for (uint32_t j : chain)
		dispatch->addCase (pad.getInt32 (j), handler_entry (j, exc));

	return state.pad;
}

/// Hand control back to the unwinder at the end of a cleanup it entered.
///
/// If nothing encloses the clause, the frame is done and a plain call suffices. If a
/// clause encloses it, this becomes an invoke landing on a pad of this cleanup's
/// own. From then on, the runtime dispatches the enclosers through that pad.
///
/// That pad is reachable only from here, which is what hands the enclosing handlers
/// the values the cleanup wrote. Through the try's own pad, they instead get the
/// state the throw site had, as though the cleanup never ran.
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

	llvm::BasicBlock *cont = create_cold_block ("resume_cont");
	llvm::BasicBlock *pad_block =
		create_cold_block (llvm::Twine ("resume_pad") + llvm::Twine (clause));

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

	llvm::BasicBlock *impossible =
		create_cold_block (llvm::Twine ("resume_pad") + llvm::Twine (clause) + "_bad");
	MonoIrBuilder (impossible).CreateUnreachable ();

	llvm::SwitchInst *dispatch = pad.CreateSwitch (
		selector, impossible, static_cast<unsigned> (enclosers.size ()));

	for (uint32_t j : enclosers)
		dispatch->addCase (pad.getInt32 (j), handler_entry (j, exc));
}

/// Record how the clause's handler is entered, before jumping to it.
///
/// The id continuation is what its endfinally switches on. Zero means an entry by
/// unwinding, which carries on by resuming that unwind.
///
/// The abort guard is cleared here rather than at the top of the body. From the
/// body's first instruction on, the byte belongs to the runtime, and another thread
/// can write it while this one runs there.
void
MethodLLVMEmitter::enter_finally (MonoIrBuilder &builder, uint32_t clause,
                                  uint32_t continuation)
{
	Clause &state = clause_state[clause];

	builder.CreateStore (builder.getInt32 (continuation), state.resume_at);
	builder.CreateStore (builder.getInt8 (0), state.abort_guard);
}

/// Mark where the clause's handler body begins or ends.
///
/// MonoFinallyRangePass reads the pair back after codegen to work out which PCs the
/// body occupies. That is the question find_last_handler_block () asks of a frame it
/// is about to guard.
///
/// A stackmap answers it because it is an instruction, so it moves, clones and
/// merges along with the surrounding code. A basic block has no such guarantee: the
/// first merge that touches it erases its identity.
///
/// The opening marker also names the guard byte, so its frame home can be recovered
/// once the frame is laid out.
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

/// Deliver an abort that arrived while the clause's handler was running, now that
/// the handler has finished.
///
/// \param which  the continuation the endfinally is about to take. Zero means the
///               handler was entered by unwinding, and an exception is already
///               leaving the frame, so the runtime delivers the abort behind it.
///
/// A thread aborted inside a finally must finish the finally first, so the abort
/// request does not raise anything there. It sets a byte in this frame
/// (install_handler_block_guard) and leaves delivery to the handler's own exit. The
/// icall hands the abort to its wrapper's interruption checkpoint. That checkpoint
/// raises it here, past the body but still inside whatever protects the handler.
/// It reaches the same catch a timely abort reaches.
///
/// This only checks on the way out through a leave.
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
	llvm::BasicBlock *deliver = create_cold_block ("deliver_abort");
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

/// Emit a call that unwinds. It becomes an invoke when a clause in this method
/// protects the current instruction.
///
/// This is the whole test for "am I inside a try". A call inside a protected
/// region must be an invoke, or LLVM marks the range nounwind and the unwinder
/// has nothing to match. The invoke's normal edge is dead, because none of these
/// callees return.
void
MethodLLVMEmitter::emit_unwinding_call (MonoIrBuilder &builder, llvm::FunctionCallee callee,
                                        llvm::ArrayRef<llvm::Value *> args)
{
	int clause = innermost_try (offset);

	if (clause >= 0) {
		llvm::BasicBlock *unwound = create_cold_block ("unwound");

		builder.CreateInvoke (callee, unwound, landing_pad (clause), args);
		builder.SetInsertPoint (unwound);
	} else {
		builder.CreateCall (callee, args);
	}

	builder.CreateUnreachable ();
}

/// Emit a call that returns but can unwind. It becomes an invoke when a clause
/// protects it.
///
/// This makes the same decision as emit_unwinding_call, but for callees that come
/// back: the normal edge is live, and translation continues along it. The describe
/// callback adds the rest of what the call site needs, on the call instruction
/// itself. What it records is not always what this function returns.
///
/// The args are the callee's declared arguments. A callee whose return travels
/// through a hidden pointer is handed a slot of this frame to fill in. What
/// comes back is what it left there.
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
		 * A managed frame is observable: stack traces, StackFrame, and the
		 * runtime's own stack walks all read it. So a call in tail position
		 * still has to be a call. Left unmarked, TailCallElimPass marks it
		 * `tail`, which either rewrites self-recursion into a loop or lets
		 * codegen hand this frame to the callee. Either way, the frame
		 * stops existing.
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
	 * The exception is not a stack operand. The handler was handed it on entry,
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
	 * When a leave exits a catch handler, it asks the runtime whether an
	 * undeniable exception is pending, and rethrows it if so. A thread abort
	 * raised for an appdomain unload survives ResetAbort. A rethrow at every
	 * catch exit stops a handler from swallowing it and keeping the thread in
	 * the dying domain.
	 *
	 * This does not happen in runtime-invoke wrappers, whose native callers
	 * expect the wrapper to catch everything.
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
		 * This goes through the icall wrapper rather than a bare call. The
		 * runtime answers by walking the stack from the last LMF. The
		 * wrapper's own LMF is what starts that walk here, instead of at
		 * whichever frame saved one last.
		 */
		llvm::Expected<llvm::Function *> undeniable = icall_wrapper_decl (
			MONO_JIT_ICALL_mono_thread_get_undeniable_exception);

		if (!undeniable)
			return undeniable.takeError ();

		llvm::Value *pending = emit_protected_call (
			builder, *undeniable, adapt_to_callee (builder, *undeniable, {}));

		llvm::BasicBlock *rethrow = create_cold_block ("rethrow_undeniable");
		llvm::BasicBlock *carry_on = llvm::BasicBlock::Create (
			context (), "no_undeniable", function);

		builder.CreateCondBr (builder.CreateIsNotNull (pending), rethrow, carry_on);

		MonoIrBuilder thrower (rethrow);
		emit_unwinding_call (thrower, throw_decl (module, "mono_llvm_throw_exception"),
		                     {pending});
		builder.SetInsertPoint (carry_on);
	}

	/*
	 * The chain collects every finally whose try region we are inside but the
	 * target is not, innermost first. A fault does not belong in the chain: it
	 * runs only when something went wrong, and a leave is an ordinary exit.
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

	/* leave empties the stack, so nothing must reach either the finally or the target. */
	pop_stack (stack.size ());

	if (llvm::Error error = enter_block (builder, *target, {}))
		return error;

	llvm::BasicBlock *next = blocks[*target].block;

	/*
	 * Walk the chain backwards. Build each step's continuation before the step
	 * that jumps to it. The last finally carries on to the target, and every
	 * earlier one carries on to the next finally.
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
	 * A fault is only ever entered by unwinding, so there is nothing to switch on.
	 * Carrying on means handing control back to the unwinder, which saved where it
	 * was before entering the handler.
	 */
	if (clauses[clause].flags == MONO_EXCEPTION_CLAUSE_FAULT) {
		emit_resume_exit (builder, static_cast<uint32_t> (clause));
		return llvm::Error::success ();
	}

	llvm::BasicBlock *unwinding = create_cold_block ("resume_unwind");
	MonoIrBuilder resume (unwinding);

	emit_resume_exit (resume, static_cast<uint32_t> (clause));

	Clause &state = clause_state[clause];
	llvm::Value *which =
		builder.CreateLoad (builder.getInt32Ty (), state.resume_at, "resume_at");

	/*
	 * The body is over from here, so a thread stopped past this point is no longer
	 * in it. That is what lets the abort check below raise rather than defer.
	 */
	emit_finally_body_marker (builder, static_cast<uint32_t> (clause), /* opening */ false);

	if (llvm::Error error =
	            emit_finally_abort_check (builder, static_cast<uint32_t> (clause), which))
		return error;

	/*
	 * resolve_finally_switches fills in the cases, once every leave that reaches
	 * this block is translated.
	 */
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
	 * A filter runs during the search pass, before anything has unwound. It hands
	 * its answer back to the runtime by returning it, not by branching anywhere in
	 * this method.
	 */
	pop_stack (stack.size ());
	builder.CreateRet (value.value);
	return llvm::Error::success ();
}

/// Fill in each finally's endfinally switches, once translation has visited every
/// leave that runs it and each one has recorded the id it used.
///
/// A handler whose every path throws or loops has no endfinally at all, and so no
/// switch to fill in. The leave's continuation is never resumed. That is legal IL,
/// and it is what C# emits for `finally { throw ...; }`.
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

/**
 * \file
 * \brief Soft-debugger sequence points.
 *
 * A sequence point is where the soft debugger is allowed to stop, and the way
 * it stops is that the code asks, at every one of them, whether anybody wants
 * it to: one word says whether single stepping is on, another whether this body
 * has a breakpoint in it, and either one being set calls the matching sdb
 * trampoline. The trampoline builds a MonoContext out of the frame it was
 * called from, hands it to the debugger and restores it on the way back, so
 * everything the debugger does with the stopped thread it does from there.
 *
 * mini gets the same effect by patching the instruction stream, which needs an
 * encoding the code generator promises; nothing promises that here, so the
 * breakpoint arm is a store into a word the code loads instead
 * (MonoLLVMBreakpointSwitch).
 *
 * The construct is
 *
 *     %ss = load volatile ptr, @single_step_tramp
 *     %bp = load volatile ptr, @this_body's_switch
 *     br (%ss | %bp) != 0, label %trap, label %cont
 *   trap:
 *     nop                          ; marked, and what the runtime records
 *     call %ss_or_nop ()
 *     call %bp_or_nop ()
 *     br label %cont
 *
 * Both calls sit in the one block, ahead of which is the marked nop, because
 * what maps a trampoline's return address back to a sequence point is
 * mono_find_prev_seq_point_for_native_offset (): the nearest recorded offset at
 * or before it. Two calls in two blocks could be laid out either side of
 * another sequence point's marker and resolve to it instead. Calling through a
 * do-nothing function is what lets the arm that is not set stay in the block.
 */

#include "method-to-llvm.hpp"
#include "seq-point-marker.hpp"

#include "mini-runtime.h"

#include "mono/metadata/debug-internals.h"
#include "mono/metadata/domain-internals.h"
#include "mono/metadata/seq-points-data.h"
#include "mono/metadata/tabledefs.h"

#include <llvm/ADT/ArrayRef.h>
#include <llvm/ADT/DenseSet.h>
#include <llvm/ADT/STLExtras.h>
#include <llvm/ADT/SmallVector.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/InlineAsm.h>
#include <llvm/IR/Instructions.h>

#include <algorithm>

namespace mono {

/*
 * Called where neither the debugger nor the stepper wants this sequence point,
 * which is what keeps both arms of the construct inside one basic block.
 */
extern "C" void
mono_llvm_seq_point_nop (void)
{
}

/*
 * Read the symbol file's idea of where this method's statements begin.
 *
 * A sequence point is meant to name a source construct, and the symbol file is
 * what knows where those start; the evaluation stack being empty is only a
 * stand-in for that, and a coarse one - `return 0;` compiles to a store, a
 * branch and a load, with an empty stack in the middle of all three. Stopping
 * there reports the same source line more than once, which a step over then
 * has to be spent on.
 *
 * A method the symbol file mentions gets exactly the offsets it names, plus the
 * two an await turns into. One it does not mention gets none at all, so long as
 * its image carries symbols - that is what a compiler-generated accessor with
 * no source of its own looks like. Everything else falls back on the stack.
 */
void
MethodLLVMEmitter::collect_sym_seq_points ()
{
	if (!mini_get_debug_options ()->gen_sdb_seq_points)
		return;

	MonoDebugMethodInfo *minfo = mono_debug_lookup_method (method);

	if (minfo == nullptr) {
		sym_seq_points = method->wrapper_type == MONO_WRAPPER_NONE
			&& !method->dynamic
			&& mono_debug_image_has_debug_info (m_class_get_image (method->klass));
		return;
	}

	MonoSymSeqPoint *points = nullptr;
	int num_points = 0;

	mono_debug_get_seq_points (minfo, NULL, NULL, NULL, &points, &num_points);
	sym_seq_points = true;

	for (int i = 0; i < num_points; ++i)
		if ((size_t) points[i].il_offset < code_size)
			sym_seq_point_offsets.insert ((uint32_t) points[i].il_offset);
	g_free (points);

	/*
	 * The stepper matches a yield or resume offset against the sequence point it
	 * stopped at to recognise an await, so those have to be stops even though no
	 * statement starts there.
	 */
	if (MonoDebugMethodAsyncInfo *async =
	            mono_debug_lookup_method_async_debug_info (method)) {
		for (int i = 0; i < async->num_awaits; ++i) {
			sym_seq_point_offsets.insert (async->yield_offsets[i]);
			sym_seq_point_offsets.insert (async->resume_offsets[i]);
		}
		mono_debug_free_method_async_debug_info (async);
	}
}

/// Whether an ordinary sequence point belongs at OFFSET.
bool
MethodLLVMEmitter::wants_seq_point_at (size_t offset) const
{
	if (sym_seq_points)
		return sym_seq_point_offsets.contains ((uint32_t) offset);

	return stack.empty () || is_handler_start (offset);
}

/// Emit the check that lets the soft debugger stop at ENCODED_IL, which is an
/// IL offset in the encoding seq-point-marker.hpp describes, carrying FLAGS.
/// Hands back the marker whose address the runtime records for this stop, or
/// null when the method is being translated without sequence points.
llvm::Instruction *
MethodLLVMEmitter::emit_seq_point (MonoIrBuilder &builder, uint32_t encoded_il,
                                   uint8_t flags)
{
	/*
	 * A filter body is a helper the runtime calls over the parent frame during
	 * the search pass; a breakpoint in one would stop the thread in the middle
	 * of dispatching an exception, which is not a place the debugger's frame
	 * model knows about.
	 */
	if (!mini_get_debug_options ()->gen_sdb_seq_points || filter_mode)
		return nullptr;
	if (il_scope == nullptr)
		return nullptr;

	if (bp_switch == nullptr)
		bp_switch = (MonoLLVMBreakpointSwitch *) mono_domain_alloc0 (
			cfg->domain, sizeof (MonoLLVMBreakpointSwitch));

	/*
	 * The entry and exit markers name places the debugger can put a breakpoint
	 * on to ask for a METHOD_ENTRY or METHOD_EXIT event; they are not places
	 * execution can be said to be at. A stepper that stopped at one would
	 * report the frame twice - once at the marker and once at the first real
	 * sequence point, which is the same source construct - so they get the
	 * breakpoint arm only.
	 */
	bool marker = encoded_il >= SEQ_POINT_ENCODED_ENTRY;

	if (!marker)
		seq_point_offsets.push_back (encoded_il);

	llvm::Type *ptr = llvm::PointerType::get (context (), 0);
	llvm::Type *i64 = builder.getInt64Ty ();
	llvm::FunctionType *hook =
		llvm::FunctionType::get (llvm::Type::getVoidTy (context ()), false);

	llvm::Constant *ss_slot = address_symbol (
		"mono_sdb_single_step_tramp", mono_arch_get_single_step_tramp_addr ());
	llvm::Constant *bp_slot = address_symbol (
		identity_symbol ("mono_sdb_breakpoint_switch", bp_switch), bp_switch);
	llvm::Constant *nop = address_symbol (
		"mono_llvm_seq_point_nop", (void *) mono_llvm_seq_point_nop);

	/*
	 * Both words are written by other threads - the stepper's from the debugger
	 * thread, the switch from whichever thread set the breakpoint - so neither
	 * load may be hoisted out of a loop or folded with the one at the previous
	 * sequence point.
	 */
	llvm::Value *ss = marker
		? nullptr
		: builder.CreateLoad (ptr, ss_slot, true, "sp.ss");
	llvm::Value *bp = builder.CreateLoad (ptr, bp_slot, true, "sp.bp");
	llvm::Value *any = builder.CreatePtrToInt (bp, i64);

	if (ss != nullptr)
		any = builder.CreateOr (builder.CreatePtrToInt (ss, i64), any);

	llvm::Value *armed = builder.CreateICmpNE (
		any, llvm::ConstantInt::get (i64, 0), "sp.armed");

	llvm::BasicBlock *trap =
		llvm::BasicBlock::Create (context (), "sp.trap", function);
	llvm::BasicBlock *cont =
		llvm::BasicBlock::Create (context (), "sp.cont", function);

	builder.CreateCondBr (armed, trap, cont);
	builder.SetInsertPoint (trap);

	/*
	 * The nop's address is what the runtime records for this sequence point.
	 * It is ahead of both calls so that either trampoline's return address
	 * resolves back here through mono_find_prev_seq_point_for_native_offset (),
	 * and nothing else with an address of its own is between: an asm block
	 * cannot be reordered across a call, which is all the ordering this needs.
	 */
	uint32_t restore = (uint32_t) statement_offset;

	set_il_location (builder, seq_point_marker_line (encoded_il, flags));

	llvm::CallInst *recorded =
		builder.CreateCall (llvm::InlineAsm::get (hook, "nop", "", true));

	set_il_location (builder, restore);

	/*
	 * Both words are read again here rather than reusing what the test above
	 * loaded, so that the block depends on nothing its predecessor computed:
	 * CMD_THREAD_SET_IP moves a stopped thread to the address recorded for a
	 * sequence point, which is the nop, and everything after it has to hold up
	 * from there.
	 */
	llvm::Value *ss_here = marker
		? nullptr
		: builder.CreateLoad (ptr, ss_slot, true, "sp.ss.trap");
	llvm::Value *bp_here = builder.CreateLoad (ptr, bp_slot, true, "sp.bp.trap");

	llvm::CallInst *ss_call =
		ss_here == nullptr
			? nullptr
			: builder.CreateCall (hook, builder.CreateSelect (
				builder.CreateIsNotNull (ss_here), ss_here, nop));
	llvm::CallInst *bp_call = builder.CreateCall (
		hook, builder.CreateSelect (builder.CreateIsNotNull (bp_here), bp_here,
	                                    nop));

	/*
	 * A trampoline reads the frame it was called from and the debugger walks
	 * out of it, so neither call may become a jump.
	 */
	if (ss_call != nullptr)
		ss_call->setTailCallKind (llvm::CallInst::TCK_NoTail);
	bp_call->setTailCallKind (llvm::CallInst::TCK_NoTail);

	builder.CreateBr (cont);
	builder.SetInsertPoint (cont);
	return recorded;
}

/*
 * Whether this method's after-call sequence points take part in the nested-call
 * tagging below, which is a question the other engines answer like this:
 *
 *	if (!(method->flags & METHOD_IMPL_ATTRIBUTE_NATIVE))
 *
 * METHOD_IMPL_ATTRIBUTE_NATIVE is 0x0001 and belongs to iflags, so read off
 * flags it is bit 0 of the accessibility nibble - set for private, assembly and
 * famorassem, clear for everything else. Nothing about a method being native
 * comes into it.
 *
 * It has to be matched rather than repaired. A compiler-generated state machine
 * is private, so neither other engine tags anything in one, and a step over
 * walking out of an async MoveNext gets the stops it gets because of that.
 */
static bool
tags_nested_calls (MonoMethod *method)
{
	return !(method->flags & METHOD_IMPL_ATTRIBUTE_NATIVE);
}

/*
 * Emit the sequence point that belongs after the call just translated, if the
 * offset it lands on does not already get one.
 *
 * The pdb has no sequence point between the calls of `f (g (), h ())`, so
 * without these a step over out of `g ()` has nowhere in the caller to stop and
 * runs on past `h ()`. NONEMPTY_STACK on all of them is what makes a step over
 * pass over one, and NESTED_CALL on every call of an argument list but the
 * outermost is what exempts the ones it has to stop at. mono_de_ss_update ()
 * reads both.
 *
 * NESTS says whether this call takes part in that run - a newobj gets a point
 * of its own but does not open or extend one, matching mini.
 */
void
MethodLLVMEmitter::emit_after_call_seq_point (MonoIrBuilder &builder, bool nests)
{
	/*
	 * Nothing follows a call the body ended on, and an offset that is a stop in
	 * its own right gets its point from the ordinary rule instead.
	 */
	if (ip >= code_size || builder.GetInsertBlock ()->getTerminator () != nullptr)
		return;
	if (wants_seq_point_at (ip))
		return;

	/*
	 * A method the symbol file names no offsets for has no source of its own -
	 * a compiler-generated accessor, a state machine's constructor - and gets no
	 * stops at all. These would be the only ones it had, so a step into such a
	 * method would land on a call in code the user never wrote.
	 */
	if (sym_seq_points && sym_seq_point_offsets.empty ())
		return;

	if (nests && tags_nested_calls (method)) {
		if (call_seq_point_run)
			il_debug_set_instruction_location (
				il_scope, call_seq_point_marker,
				seq_point_marker_line (
					call_seq_point_offset,
					MONO_SEQ_POINT_FLAG_NONEMPTY_STACK
						| MONO_SEQ_POINT_FLAG_NESTED_CALL));
		else
			call_seq_point_run = true;
	}

	call_seq_point_offset = (uint32_t) ip;
	call_seq_point_marker = emit_seq_point (builder, call_seq_point_offset,
	                                        MONO_SEQ_POINT_FLAG_NONEMPTY_STACK);
}

/*
 * Work out which sequence points can execute next after each of this body's.
 *
 * This is what the debugger single-steps with: a step places a breakpoint at
 * every successor of the point the thread is stopped at, and if a body offers
 * none the stepper falls back to trapping every method entry in the process and
 * stops wherever that first lands. The graph is over the CIL rather than over
 * the code that came out, because an IL offset is how the debugger names a
 * place - and mono_seq_point_init_next () reads the entries back as indices
 * into the published table, which jinfo.cpp turns these offsets into.
 *
 * Edges are the ones the IL itself draws, plus the two an exception clause adds:
 * a `leave` reaches the finallys it unwinds through on its way to its target,
 * and their endfinallys reach that target back again.
 */
void
MethodLLVMEmitter::build_seq_point_graph ()
{
	if (seq_point_offsets.empty ())
		return;

	llvm::DenseSet<uint32_t> points (seq_point_offsets.begin (),
	                                 seq_point_offsets.end ());
	llvm::DenseMap<uint32_t, llvm::SmallVector<uint32_t, 2>> edges;

	auto add = [&] (size_t from, size_t to) {
		if (to >= code_size)
			return;

		llvm::SmallVectorImpl<uint32_t> &out = edges[(uint32_t) from];

		if (!llvm::is_contained (out, (uint32_t) to))
			out.push_back ((uint32_t) to);
	};

	/*
	 * Which target an endfinally carries on to depends on which leave entered
	 * the handler, so the leaves are what say where its handler's endfinallys
	 * can go.
	 */
	std::vector<std::vector<uint32_t>> leave_targets (num_clauses);
	std::vector<uint32_t> endfinallys;

	for (size_t at = 0; at < code_size;) {
		llvm::Expected<Flow> flow = decode_flow (at);

		if (!flow) {
			/* The body translated, so nothing here fails to decode. */
			llvm::consumeError (flow.takeError ());
			return;
		}

		for (size_t target : flow->targets)
			add (at, target);
		if (flow->falls_through ())
			add (at, flow->next);

		if ((flow->opcode == MONO_CEE_LEAVE || flow->opcode == MONO_CEE_LEAVE_S)
		    && !flow->targets.empty ()) {
			size_t target = flow->targets[0];

			for (uint32_t i = 0; i < num_clauses; ++i) {
				MonoExceptionClause *clause = &clauses[i];

				if (clause->flags != MONO_EXCEPTION_CLAUSE_FINALLY)
					continue;
				if (!MONO_OFFSET_IN_CLAUSE (clause, at)
				    || MONO_OFFSET_IN_CLAUSE (clause, target))
					continue;

				add (at, clause->handler_offset);
				leave_targets[i].push_back ((uint32_t) target);
			}
		} else if (flow->opcode == MONO_CEE_ENDFINALLY) {
			endfinallys.push_back ((uint32_t) at);
		}

		at = flow->next;
	}

	for (uint32_t at : endfinallys) {
		int clause = innermost_handler (at);

		if (clause < 0)
			continue;
		for (uint32_t target : leave_targets[clause])
			add (at, target);
	}

	auto successors = [&] (uint32_t at) -> llvm::ArrayRef<uint32_t> {
		auto found = edges.find (at);

		if (found == edges.end ())
			return {};
		return found->second;
	};

	for (uint32_t from : seq_point_offsets) {
		auto [entry, fresh] = seq_point_graph.try_emplace (from);

		if (!fresh)
			continue;

		llvm::DenseSet<uint32_t> seen;
		llvm::SmallVector<uint32_t, 8> work;
		auto push = [&] (uint32_t at) {
			if (seen.insert (at).second)
				work.push_back (at);
		};

		for (uint32_t next : successors (from))
			push (next);

		while (!work.empty ()) {
			uint32_t at = work.pop_back_val ();

			if (points.contains (at))
				entry->second.push_back (at);
			else
				for (uint32_t next : successors (at))
					push (next);
		}

		/* The order a worklist happens to visit in is nobody's business. */
		std::sort (entry->second.begin (), entry->second.end ());
	}
}

} // namespace mono

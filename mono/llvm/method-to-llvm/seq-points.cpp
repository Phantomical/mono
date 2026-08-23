/**
 * \file
 * \brief Soft-debugger sequence points.
 *
 * A sequence point is a place where the soft debugger can stop a thread. At
 * every sequence point the emitted code checks two words. One says whether
 * single stepping is on. The other says whether this body has a breakpoint.
 * If either word is set, the code calls the matching sdb trampoline. The
 * trampoline builds a MonoContext from the calling frame and hands it to the
 * debugger. It restores the context before it returns, so the debugger does
 * all its work with the stopped thread through that context.
 *
 * mini patches the instruction stream to get the same effect. That needs an
 * encoding the code generator promises. This backend makes no such promise.
 * Instead the breakpoint arm stores into a word (MonoLLVMBreakpointSwitch)
 * that the emitted code loads.
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
 * Both calls sit in the trap block, after the marked nop.
 * mono_find_prev_seq_point_for_native_offset () maps a trampoline's return
 * address back to a sequence point. It finds the nearest recorded offset at
 * or before that address. Two calls split across two blocks can land on
 * either side of another sequence point's marker and resolve to the wrong
 * one. Each call targets a do-nothing function when its own arm is not set,
 * so both calls can stay in the one block.
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

extern "C" void
mono_llvm_seq_point_nop (void)
{
}

/**
 * Read the symbol file's sequence-point offsets for this method.
 *
 * A sequence point marks where a source statement begins, and the symbol
 * file is what records that. An empty evaluation stack is only a rough
 * stand-in for that idea. `return 0;` compiles to a store, a branch, and a
 * load, with an empty stack after each one. If the code stops at all three,
 * it reports the same source line more than once. A step over must then
 * pass over the extra stops.
 *
 * If the symbol file names this method, it gets exactly the offsets the
 * symbol file lists, plus the two offsets an await expression adds. If the
 * symbol file does not name this method but its image carries debug
 * information, the method gets no offsets at all. That is what a
 * compiler-generated accessor with no source of its own looks like. A method
 * whose image carries no debug information falls back to the empty-stack
 * rule instead.
 */
void
MethodLLVMEmitter::collect_sym_seq_points ()
{
	if (!mini_get_debug_options ()->gen_sdb_seq_points)
		return;

	/*
	 * A dynamic method is built at run time, so no symbol file names it. It
	 * still gets a MonoDebugMethodInfo, and that record lists no offsets - which
	 * reads as "the symbol file says this body has no statements" and leaves the
	 * body with no sequence points at all. Its line table then keeps only the
	 * entry row, so every address in it resolves to IL offset 0. Take the
	 * empty-stack rule instead, which is what a body with no symbols wants.
	 */
	if (method->dynamic)
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
	 * The stepper matches a yield or resume offset against the sequence point
	 * where it stopped to recognize an await. These offsets must be stops
	 * even though no statement begins there.
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

bool
MethodLLVMEmitter::wants_seq_point_at (size_t offset) const
{
	if (sym_seq_points)
		return sym_seq_point_offsets.contains ((uint32_t) offset);

	return stack.empty () || is_handler_start (offset);
}

/// Emit the check that lets the soft debugger stop at encoded_il, an IL
/// offset in the encoding that seq-point-marker.hpp describes. The flags
/// parameter holds the MONO_SEQ_POINT_FLAG_* bits to record for this stop.
///
/// Returns the marker instruction whose address the runtime records for this
/// stop. Returns null when sequence points are off, in a filter body, or when
/// there is no debug scope to attach the stop to.
llvm::Instruction *
MethodLLVMEmitter::emit_seq_point (MonoIrBuilder &builder, uint32_t encoded_il,
                                   uint8_t flags)
{
	/*
	 * A filter body is a helper the runtime calls over the parent frame during
	 * the search pass. A breakpoint there stops the thread while it dispatches
	 * an exception, a place the debugger's frame model does not know about.
	 */
	if (!mini_get_debug_options ()->gen_sdb_seq_points || filter_mode)
		return nullptr;
	if (il_scope == nullptr)
		return nullptr;

	if (bp_switch == nullptr)
		bp_switch = (MonoLLVMBreakpointSwitch *) mono_domain_alloc0 (
			cfg->domain, sizeof (MonoLLVMBreakpointSwitch));

	/*
	 * The entry and exit markers name places where the debugger can set a
	 * breakpoint for a METHOD_ENTRY or METHOD_EXIT event. They are not places
	 * execution can be said to be at. A stepper stopped at one reports the
	 * frame twice. It stops once at the marker, and once at the first real
	 * sequence point, which names the same source construct. So a marker
	 * gets the breakpoint arm only, with no single-step check.
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
	 * Other threads write both words: the debugger thread writes the
	 * stepper's word, and whichever thread sets the breakpoint writes the
	 * switch. Each load stays volatile so the optimizer cannot hoist it out
	 * of a loop or fold it with the load at the previous sequence point.
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

	/*
	 * The trap block is cold, but it stays where it is emitted rather than
	 * going to the end with create_cold_block (). The runtime answers for an
	 * address with the sequence point recorded at or before it, so a marker
	 * that moves away from the code it belongs to answers for a statement
	 * somewhere else. The stepper then reports the wrong method.
	 */
	llvm::BasicBlock *trap =
		llvm::BasicBlock::Create (context (), "sp.trap", function);
	llvm::BasicBlock *cont =
		llvm::BasicBlock::Create (context (), "sp.cont", function);

	builder.CreateCondBr (armed, trap, cont);
	builder.SetInsertPoint (trap);

	/*
	 * An inline asm block with side effects cannot be reordered across a
	 * call, and that is all the ordering this needs.
	 */
	uint32_t restore = (uint32_t) statement_offset;

	set_il_location (builder, seq_point_marker_line (encoded_il, flags));

	llvm::CallInst *recorded =
		builder.CreateCall (llvm::InlineAsm::get (hook, "nop", "", true));

	set_il_location (builder, restore);

	/*
	 * Both words are read again here, not reused from the test above, so the
	 * trap block depends on nothing its predecessor computed.
	 * CMD_THREAD_SET_IP moves a stopped thread straight to the recorded
	 * address, the nop, and everything after it must still hold up when
	 * execution resumes there.
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
	 * A trampoline reads the frame it was called from, and the debugger walks
	 * out of it. Both calls carry TCK_NoTail, so neither can turn into a
	 * jump that skips that frame.
	 */
	if (ss_call != nullptr)
		ss_call->setTailCallKind (llvm::CallInst::TCK_NoTail);
	bp_call->setTailCallKind (llvm::CallInst::TCK_NoTail);

	builder.CreateBr (cont);
	builder.SetInsertPoint (cont);
	return recorded;
}

/**
 * Whether this method's after-call sequence points take part in the
 * nested-call tagging below. The interpreter answers the same question with:
 *
 *	if (!(method->flags & METHOD_IMPL_ATTRIBUTE_NATIVE))
 *
 * METHOD_IMPL_ATTRIBUTE_NATIVE is 0x0001 and belongs to iflags, but this test
 * reads it off flags instead. There it lands on bit 0 of the accessibility
 * nibble, set for private, assembly, and famorassem, and clear for everything
 * else. Whether a method is native plays no part in it.
 *
 * This backend matches that test. It does not correct it. A
 * compiler-generated state machine is private, so the interpreter tags
 * nothing inside one. A step over that exits an async MoveNext gets whatever
 * stops this test happens to produce.
 */
static bool
tags_nested_calls (MonoMethod *method)
{
	return !(method->flags & METHOD_IMPL_ATTRIBUTE_NATIVE);
}

/**
 * Emit the sequence point that belongs after the call just translated, if the
 * offset it lands on does not already get one.
 *
 * The pdb has no sequence point between the calls in `f (g (), h ())`.
 * Without this, a step over out of `g ()` finds nowhere to stop in the
 * caller and runs on past `h ()`. MONO_SEQ_POINT_FLAG_NONEMPTY_STACK on
 * every one of these points is what makes a step over pass over it.
 * MONO_SEQ_POINT_FLAG_NESTED_CALL on every call in an argument list but the
 * outermost is what exempts the ones a step over must still stop at.
 * mono_de_ss_update () reads both flags.
 *
 * nests says whether this call takes part in that run. A newobj gets a point
 * of its own but does not open or extend a run. The interpreter treats
 * newobj the same way.
 */
void
MethodLLVMEmitter::emit_after_call_seq_point (MonoIrBuilder &builder, bool nests)
{
	if (ip >= code_size || builder.GetInsertBlock ()->getTerminator () != nullptr)
		return;
	if (wants_seq_point_at (ip))
		return;

	/*
	 * If the symbol file names this method but lists no offsets, that is what
	 * a compiler-generated accessor or a state machine's constructor looks
	 * like. An after-call point is the only stop such a method can get. If
	 * one is added here, a step into the method lands on a call in code the
	 * user never wrote.
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

/**
 * Work out which sequence points can execute immediately after each sequence
 * point in this body.
 *
 * The debugger uses this graph to single-step. A step places a breakpoint at
 * every successor of the point where the thread is stopped. If a body offers
 * no successors, the stepper turns on global single stepping instead. Every
 * sequence point in the process then traps, and step filtering picks which
 * one to stop at.
 *
 * The graph is over the CIL rather than over the emitted code, because an IL
 * offset is how the debugger names a place. mono_seq_point_init_next () reads
 * the graph's entries back as indices into the published table. jinfo.cpp is
 * what builds that table from these IL offsets.
 *
 * Edges are the ones the IL itself draws, plus two more that an exception
 * clause adds. A `leave` reaches the finallys it unwinds through on its way
 * to its target, and their endfinallys reach that target again afterward.
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
	 * Which target an endfinally continues to depends on which leave entered
	 * its handler. The leaves are therefore what say where that handler's
	 * endfinallys can go.
	 */
	std::vector<std::vector<uint32_t>> leave_targets (num_clauses);
	std::vector<uint32_t> endfinallys;

	for (size_t at = 0; at < code_size;) {
		llvm::Expected<Flow> flow = decode_flow (at);

		if (!flow) {
			// The method already translated without error, so decode_flow ()
			// cannot fail here.
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

		// A worklist can visit successors in any order, so sort them for a
		// stable result.
		std::sort (entry->second.begin (), entry->second.end ());
	}
}

} // namespace mono

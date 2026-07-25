/**
 * \file
 * finally-range.cpp - MonoFinallyRangePass, the machine-level recovery of the PC
 * ranges each finally handler body occupies.
 */

/*
 * Same reason engine.cpp drops mono's PIC macro: libtool compiles this TU with
 * -DPIC, and LLVM uses `PIC` as an identifier (PassInstrumentationCallbacks), so
 * the macro would rewrite it and break a header.
 */
#ifdef PIC
#undef PIC
#endif

#include "finally-range.hpp"

#include "../engine.hpp"
#include "../mono_lsda_format.hpp"

#include <set>
#include <string>

#include <llvm/ADT/DenseMap.h>
#include <llvm/CodeGen/MachineBasicBlock.h>
#include <llvm/CodeGen/MachineFunction.h>
#include <llvm/CodeGen/MachineInstrBuilder.h>
#include <llvm/CodeGen/TargetInstrInfo.h>
#include <llvm/CodeGen/TargetSubtargetInfo.h>
#include <llvm/MC/MCContext.h>
#include <llvm/MC/MCSymbol.h>

using namespace llvm;

namespace mono {
namespace {

/*
 * True if MI is one of our finally markers, with the clause it names and whether
 * it opens or closes the body.
 */
bool
finally_marker (const MachineInstr &mi, int *clause, bool *is_start)
{
	if (mi.getOpcode () != TargetOpcode::STACKMAP)
		return false;

	uint64_t id = (uint64_t) mi.getOperand (0).getImm ();

	if ((id >> 32) == (MONO_LLVM_FINALLY_STACKMAP_ID_BASE >> 32))
		*is_start = true;
	else if ((id >> 32) == (MONO_LLVM_FINALLY_END_STACKMAP_ID_BASE >> 32))
		*is_start = false;
	else
		return false;

	*clause = (int) (uint32_t) (id & MONO_LLVM_FINALLY_STACKMAP_ID_MASK);
	return true;
}

MCSymbol *
plant_label (MachineBasicBlock &mbb, MachineBasicBlock::iterator at, MCContext &ctx,
             const TargetInstrInfo *tii, const char *name)
{
	MCSymbol *sym = ctx.createTempSymbol (name);

	BuildMI (mbb, at, DebugLoc (), tii->get (TargetOpcode::EH_LABEL)).addSym (sym);
	return sym;
}

void
close_range (MonoEHFinallyFunction &fn, MachineBasicBlock &mbb,
             MachineBasicBlock::iterator at, MCContext &ctx, const TargetInstrInfo *tii,
             MCSymbol *begin, int clause)
{
	MonoEHFinallyBody body;

	body.body_begin = begin;
	body.body_end = plant_label (mbb, at, ctx, tii, "mono_finally_end");
	body.clause_index = clause;
	fn.bodies.push_back (body);
}

/* Whether MBB, entered inside CLAUSE's body or not, leaves inside it. */
bool
transfer (MachineBasicBlock &mbb, int clause, bool in)
{
	bool state = in;

	for (MachineInstr &mi : mbb) {
		int found;
		bool is_start;

		if (!finally_marker (mi, &found, &is_start) || found != clause)
			continue;
		state = is_start;
	}

	return state;
}

/*
 * Whether each block starts inside CLAUSE's handler body. A marker is the whole
 * transfer function; the meet is agreement, since a block reached both from
 * inside the body and from outside it is not reliably either.
 *
 * BranchFolding can produce exactly that by merging a body's tail with an
 * identical tail elsewhere, so it is a real shape and not a broken invariant.
 * Such a block is treated as NOT body: the cost is an abort delivered a little
 * early inside a finally, where claiming it WOULD defer an abort for a frame
 * that is not in the finally at all - and nothing would ever rethrow it.
 */
void
solve (MachineFunction &mf, int clause, DenseMap<const MachineBasicBlock *, bool> &in_body)
{
	DenseMap<const MachineBasicBlock *, bool> known, out_body;
	bool changed = true;

	for (MachineBasicBlock &mbb : mf) {
		in_body[&mbb] = false;
		out_body[&mbb] = false;
		known[&mbb] = false;
	}
	known[&mf.front ()] = true;

	while (changed) {
		changed = false;

		for (MachineBasicBlock &mbb : mf) {
			bool in = false, have = false;

			if (&mbb == &mf.front ()) {
				have = true;
			} else {
				for (MachineBasicBlock *pred : mbb.predecessors ()) {
					if (!known[pred])
						continue;
					if (!have) {
						in = out_body[pred];
						have = true;
					} else if (in != out_body[pred]) {
						in = false;
					}
				}
			}

			bool out = transfer (mbb, clause, in);

			if (known[&mbb] != have || in_body[&mbb] != in || out_body[&mbb] != out) {
				known[&mbb] = have;
				in_body[&mbb] = in;
				out_body[&mbb] = out;
				changed = true;
			}
		}
	}
}

/*
 * Bracket each maximal run of CLAUSE's body instructions with a pair of labels
 * and record it. An EH_LABEL emits nothing but its symbol, so it can sit
 * anywhere in a block - which is what lets a range be recorded where the body
 * actually lies instead of having to move the code somewhere it can be named.
 */
void
record_ranges (MachineFunction &mf, int clause,
               const DenseMap<const MachineBasicBlock *, bool> &in_body,
               MonoEHFinallyFunction &fn)
{
	MCContext &ctx = mf.getContext ();
	const TargetInstrInfo *tii = mf.getSubtarget ().getInstrInfo ();

	for (MachineBasicBlock &mbb : mf) {
		bool state = in_body.lookup (&mbb);
		MCSymbol *begin = nullptr;

		if (state)
			begin = plant_label (mbb, mbb.begin (), ctx, tii, "mono_finally_begin");

		for (MachineBasicBlock::iterator it = mbb.begin (), end = mbb.end (); it != end; ++it) {
			int found;
			bool is_start;

			if (!finally_marker (*it, &found, &is_start) || found != clause)
				continue;

			if (is_start && !state) {
				begin = plant_label (mbb, it, ctx, tii, "mono_finally_begin");
				state = true;
			} else if (!is_start && state) {
				close_range (fn, mbb, it, ctx, tii, begin, clause);
				begin = nullptr;
				state = false;
			}
		}

		if (state)
			close_range (fn, mbb, mbb.end (), ctx, tii, begin, clause);
	}
}

} // namespace

bool
MonoFinallyRangePass::runOnMachineFunction (MachineFunction &mf)
{
	std::set<int> clauses;

	for (MachineBasicBlock &mbb : mf) {
		for (MachineInstr &mi : mbb) {
			int clause;
			bool is_start;
			if (finally_marker (mi, &clause, &is_start))
				clauses.insert (clause);
		}
	}

	/*
	 * No marker: the method has no finally, or every body was optimized away.
	 * Stay completely inert so a non-finally module's object is untouched.
	 */
	if (clauses.empty ())
		return false;

	MonoEHFinallyFunction fn;
	fn.function = mf.getName ().str ();

	/*
	 * One clause at a time, each blind to the others' markers. IL nests
	 * finallys - a finally body can hold a whole try/finally of its own - and
	 * then the inner body's PCs are inside the outer body too. Tracking one
	 * state for the whole function would have to choose between them; per
	 * clause they simply both cover, which is what the classic JIT's nested
	 * handler ranges do and what find_last_handler_block () expects.
	 */
	for (int clause : clauses) {
		DenseMap<const MachineBasicBlock *, bool> in_body;

		solve (mf, clause, in_body);
		record_ranges (mf, clause, in_body, fn);
	}

	sc_.finally_functions.push_back (std::move (fn));

	/* Only labels were added, and a label emits no code. */
	return true;
}

char MonoFinallyRangePass::ID = 0;

} // namespace mono

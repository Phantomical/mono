/**
 * \file
 * \brief Recovering, at the machine level, the PC ranges each finally handler
 * body occupies.
 */

// mono/utils/mono-tls.h defines PIC as an empty macro under -fPIC, and LLVM
// uses `PIC` as an identifier (PassInstrumentationCallbacks), so a stray
// expansion breaks the header.
#ifdef PIC
#undef PIC
#endif

#include "finally-range.hpp"

#include "../eh-side-channel.hpp"
#include "../mono_lsda_format.hpp"

#include <map>
#include <set>
#include <string>
#include <utility>

#include <llvm/ADT/DenseMap.h>
#include <llvm/CodeGen/MachineBasicBlock.h>
#include <llvm/CodeGen/MachineFunction.h>
#include <llvm/CodeGen/MachineInstrBuilder.h>
#include <llvm/CodeGen/StackMaps.h>
#include <llvm/CodeGen/TargetInstrInfo.h>
#include <llvm/CodeGen/TargetRegisterInfo.h>
#include <llvm/CodeGen/TargetSubtargetInfo.h>
#include <llvm/MC/MCContext.h>
#include <llvm/MC/MCSymbol.h>

using namespace llvm;

namespace mono {
namespace {

/// Whether mi is one of our finally markers, and if so, the clause it names
/// and whether it opens or closes the body.
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

/// Returns the opening marker's frame slot, as a DWARF register and a
/// displacement from it, or reg -1 if it named no slot.
std::pair<int, std::int64_t>
marker_slot (const MachineInstr &mi, const TargetRegisterInfo *tri)
{
	// id, shadow bytes, then the live values.
	if (mi.getNumOperands () < 5)
		return { -1, 0 };

	const MachineOperand &kind = mi.getOperand (2);
	const MachineOperand &base = mi.getOperand (3);
	const MachineOperand &offset = mi.getOperand (4);

	if (!kind.isImm () || kind.getImm () != StackMaps::DirectMemRefOp)
		return { -1, 0 };
	if (!base.isReg () || !offset.isImm ())
		return { -1, 0 };

	// By the time this pass runs, PEI has resolved the marker's frame index
	// into a register and offset. The register itself is the target's
	// choice: normally the frame pointer, or a base pointer when the frame
	// also has variable-sized objects. It is read here rather than assumed.
	return { tri->getDwarfRegNum (base.getReg (), false), offset.getImm () };
}

/// Plants a label at \p at that emits no code, so it can sit anywhere in a
/// block.
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
             MCSymbol *begin, int clause, std::pair<int, std::int64_t> slot)
{
	MonoEHFinallyBody body;

	body.body_begin = begin;
	body.body_end = plant_label (mbb, at, ctx, tii, "mono_finally_end");
	body.clause_index = clause;
	body.exvar_dwarf_reg = slot.first;
	body.exvar_offset = slot.second;
	fn.bodies.push_back (body);
}

/// Whether mbb, entered inside clause's body or not, leaves inside it.
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

/// Fills in_body with whether each block starts inside clause's handler body.
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
				/*
				 * The meet across predecessors is agreement: a block reached
				 * from both inside clause's body and from outside it is not
				 * reliably either.
				 *
				 * Predecessors can disagree here, which BranchFolding causes
				 * by merging a body's tail with an identical tail elsewhere -
				 * a real shape, not a broken invariant. A disputed block is
				 * treated as outside the body: worst case, an abort inside
				 * the finally arrives a little early. The alternative can
				 * defer an abort for a frame that was never in the finally at
				 * all, with nothing left to rethrow it.
				 */
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

/// Brackets each maximal run of clause's body instructions with a pair of
/// labels, and records it.
void
record_ranges (MachineFunction &mf, int clause,
               const DenseMap<const MachineBasicBlock *, bool> &in_body,
               MonoEHFinallyFunction &fn, std::pair<int, std::int64_t> slot)
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
				close_range (fn, mbb, it, ctx, tii, begin, clause, slot);
				begin = nullptr;
				state = false;
			}
		}

		/*
		 * A run reaching the end of a block ends at the first byte of the
		 * next one, so the block's branch is covered too. A thread can be
		 * stopped on a branch, and `while (!foo);` inside a finally spends
		 * most of its time on exactly that.
		 *
		 * Hence the label goes at the head of the next block, rather than
		 * after this block's branch. An instruction after the branch makes
		 * getFirstTerminator () walk off the end. The printer then reads the
		 * block as falling through, and leaves the successor's label
		 * unemitted. The last block in layout has no successor to lose a
		 * label, so its end label goes at the very end of the function.
		 */
		if (state) {
			MachineBasicBlock *next = mbb.getNextNode ();

			if (next)
				close_range (fn, *next, next->begin (), ctx, tii, begin,
				             clause, slot);
			else
				close_range (fn, mbb, mbb.end (), ctx, tii, begin, clause,
				             slot);
		}
	}
}

} // namespace

bool
MonoFinallyRangePass::runOnMachineFunction (MachineFunction &mf)
{
	const TargetRegisterInfo *tri = mf.getSubtarget ().getRegisterInfo ();
	std::set<int> clauses;
	std::map<int, std::pair<int, std::int64_t>> slots;

	for (MachineBasicBlock &mbb : mf) {
		for (MachineInstr &mi : mbb) {
			int clause;
			bool is_start;

			if (!finally_marker (mi, &clause, &is_start))
				continue;

			clauses.insert (clause);
			if (!is_start)
				continue;

			/*
			 * A clause's body can end up in the frame more than once - the
			 * optimizer duplicates it along its entry paths. A clone reuses
			 * the one slot.
			 *
			 * Two markers naming different slots mean the copies do not
			 * share one, which no guard entry can describe. So the clause
			 * goes unguarded rather than wrongly.
			 */
			std::pair<int, std::int64_t> slot = marker_slot (mi, tri);
			auto [known, fresh] = slots.emplace (clause, slot);

			if (!fresh && known->second != slot)
				known->second = { -1, 0 };
		}
	}

	// No marker: the method has no finally, or every body was optimized away.
	// Stay inert so a non-finally module's object is untouched.
	if (clauses.empty ())
		return false;

	MonoEHFinallyFunction fn;
	fn.function = mf.getName ().str ();

	/*
	 * One clause at a time, each blind to the others' markers. IL nests
	 * finallys: a finally body can hold a whole try/finally of its own. The
	 * inner body's PCs land inside the outer body's range too.
	 *
	 * One combined state cannot represent both. Each clause tracks its own,
	 * so the ranges overlap where bodies nest. That is what the
	 * classic JIT's nested handler ranges do, and what
	 * find_last_handler_block () expects.
	 */
	for (int clause : clauses) {
		DenseMap<const MachineBasicBlock *, bool> in_body;

		solve (mf, clause, in_body);
		record_ranges (mf, clause, in_body, fn, slots[clause]);
	}

	sc_->finally_functions.push_back (std::move (fn));

	// Only labels were added, and a label emits no code.
	return true;
}

char MonoFinallyRangePass::ID = 0;

} // namespace mono

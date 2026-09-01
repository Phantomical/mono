/**
 * \file
 * \brief The machine-level recovery of mono's EH clauses from the final
 * landing-pad set.
 */

/*
 * LLVM uses `PIC` as an identifier (PassInstrumentationCallbacks). Mono's
 * build defines it as a macro.
 */
#ifdef PIC
#undef PIC
#endif

#include "eh-gather.hpp"

#include "../eh-side-channel.hpp"
#include "../il-line-table.hpp"
#include "clause-marker.hpp"

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include <llvm/ADT/DenseMap.h>
#include <llvm/ADT/STLExtras.h>
#include <llvm/CodeGen/MachineBasicBlock.h>
#include <llvm/CodeGen/MachineFunction.h>
#include <llvm/CodeGen/MachineFunctionPass.h>
#include <llvm/CodeGen/MachineInstrBuilder.h>
#include <llvm/CodeGen/TargetInstrInfo.h>
#include <llvm/CodeGen/TargetOpcodes.h>
#include <llvm/CodeGen/TargetSubtargetInfo.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/MC/MCContext.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DebugInfoMetadata.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>
#include <llvm/MC/MCSymbol.h>
#include <llvm/Support/ErrorHandling.h>

using namespace llvm;

namespace mono {

namespace {

/// One IL clause's try region, as the method declared it.
struct ILClause {
	int index = -1;
	std::uint32_t try_offset = 0;
	std::uint32_t try_len = 0;
};

/// One instruction in layout order, and the try region its IL offset lies in.
struct Position {
	MachineBasicBlock *mbb;
	MachineBasicBlock::iterator at;
	int region;
};

/// One invoke's range, as positions in the layout, and the chain of clauses the
/// pad it unwinds to dispatches.
struct Invoke {
	unsigned begin;
	unsigned end;
	unsigned chain;
};

} // namespace

/// Reads the try regions the front end recorded as `!mono.clauses`
/// (emit_clause_geometry (), method-to-llvm/exceptions.cpp) back off the
/// function.
static std::vector<ILClause>
decode_clause_geometry (const Function &f)
{
	std::vector<ILClause> geometry;
	const MDNode *list = f.getMetadata ("mono.clauses");

	if (list == nullptr)
		return geometry;

	geometry.reserve (list->getNumOperands ());
	for (const MDOperand &operand : list->operands ()) {
		const auto *node = dyn_cast_or_null<MDNode> (operand.get ());

		/*
		 * The front end writes this list and this reads it back, in one
		 * compile. A shape it does not decode means our own emission or our
		 * own reader is wrong, not that the input did something we do not
		 * support.
		 */
		if (node == nullptr || node->getNumOperands () != 4)
			report_fatal_error ("mono: a !mono.clauses entry is not four words - our own emission or reader is wrong");

		std::uint64_t words[4];

		for (unsigned i = 0; i < 4; ++i) {
			const auto *word =
				mdconst::dyn_extract<ConstantInt> (node->getOperand (i));

			if (word == nullptr)
				report_fatal_error ("mono: a !mono.clauses word is not a constant - our own emission or reader is wrong");

			words[i] = word->getZExtValue ();
		}

		ILClause clause;

		clause.index = (int) words[0];
		clause.try_offset = (std::uint32_t) words[2];
		clause.try_len = (std::uint32_t) words[3];
		geometry.push_back (clause);
	}

	return geometry;
}

/**
 * The clause innermost_try () (method-to-llvm/exceptions.cpp) names for an IL
 * offset, or -1 where no try region covers it.
 *
 * This makes the same choice that function did, because that is what decided
 * which pad the method's protected calls unwind to. So it reads the regions in
 * clause-index order and takes a strictly smaller one, which leaves the first of
 * a set of siblings the answer.
 */
static int
innermost_try (const std::vector<ILClause> &geometry, int il)
{
	int found = -1;
	std::uint32_t narrowest = 0;

	if (il < 0)
		return -1;

	for (const ILClause &clause : geometry) {
		if ((std::uint32_t) il < clause.try_offset
		    || (std::uint32_t) il - clause.try_offset >= clause.try_len)
			continue;

		if (found < 0 || clause.try_len < narrowest) {
			found = clause.index;
			narrowest = clause.try_len;
		}
	}

	return found;
}

/// Plants a label at \p at that emits no code, so it can sit anywhere in a
/// block.
static MCSymbol *
plant_label (MachineBasicBlock &mbb, MachineBasicBlock::iterator at, MCContext &ctx,
             const TargetInstrInfo *tii)
{
	MCSymbol *sym = ctx.createTempSymbol ("mono_try");

	BuildMI (mbb, at, DebugLoc (), tii->get (TargetOpcode::EH_LABEL)).addSym (sym);
	return sym;
}

bool
MonoEHGatherPass::doInitialization (Module &m)
{
	ids_ = il_debug_subprogram_ids (m);
	return false;
}

bool
MonoEHGatherPass::runOnMachineFunction (MachineFunction &mf)
{
	const std::vector<LandingPadInfo> &pads = mf.getLandingPads ();

	if (pads.empty ()) {
		/*
		 * No landing pads survived to codegen. For an ordinary non-EH
		 * function there is nothing to do here. Staying inert keeps the
		 * emitted object byte-identical to a module the EH machinery never
		 * touched. (mono-has-eh-clauses is unset: method-to-llvm.cpp never
		 * marks a method whose IL declared no clauses.)
		 *
		 * For a method method-to-llvm.cpp did mark, zero landing pads
		 * means every protected call under its try region got optimized
		 * to a nounwind call before isel. Nothing is left that can unwind
		 * through it. That is a confirmed-safe case, not an uncertain
		 * one. This records a clean entry - no clauses, not declined -
		 * rather than leaving the function out of the side channel. The
		 * side-table writer (compiler.cpp) turns a present-but-empty
		 * entry into a genuinely empty, valid `.mono_lsda` record, which
		 * the reader (mono_lsda.cpp) tells apart from "absent because
		 * declined".
		 */
		if (mf.getFunction ().hasFnAttribute ("mono-has-eh-clauses")) {
			MonoEHFunctionClauses fn;
			fn.function = mf.getName ().str ();
			sc_->functions.push_back (std::move (fn));
		}
		return false;
	}

	const std::vector<const GlobalValue *> &type_infos = mf.getTypeInfos ();
	const std::vector<ILClause> geometry = decode_clause_geometry (mf.getFunction ());

	MonoEHFunctionClauses fn;
	fn.function = mf.getName ().str ();

	/*
	 * The function's instructions in layout order, each with the try region its
	 * IL offset falls in, and where each label sits among them.
	 *
	 * An instruction with no location keeps the offset of the one before it in
	 * the same block. The offset stops at the end of that block, because layout
	 * order is not control flow. The block laid out after a try region is as
	 * often a handler or a cold path as the rest of the region. A block that
	 * opens with no location therefore names no region, so a range never grows
	 * into it.
	 *
	 * self is a different question from anything positions answers: which
	 * method this whole machine function is itself the compiled body of,
	 * read off the function's own subprogram rather than off any one
	 * instruction. It never needs a fold's boundary respected the way a
	 * per-instruction answer would, because it is a property of the
	 * function, not of a position in it - see where it is used, below, for
	 * why an instruction's own position cannot answer this question at all.
	 */
	std::uint64_t self = ids_.lookup (mf.getFunction ().getSubprogram ());
	std::vector<Position> positions;
	DenseMap<const MCSymbol *, unsigned> at_label;

	for (MachineBasicBlock &mbb : mf) {
		int il = -1;

		for (MachineBasicBlock::iterator it = mbb.begin (), end = mbb.end ();
		     it != end; ++it) {
			if (it->getOpcode () == TargetOpcode::EH_LABEL)
				at_label[it->getOperand (0).getMCSymbol ()] =
					(unsigned) positions.size ();

			if (it->isMetaInstruction ())
				continue;

			/*
			 * A folded body's location carries the callee's IL offset,
			 * which says nothing about this method's try regions. The
			 * outermost location of the chain names the call site the
			 * body was folded at, which is the offset IlLineHandler
			 * records as well (compiler.cpp).
			 */
			if (const DILocation *loc = it->getDebugLoc ().get ()) {
				while (loc->getInlinedAt () != nullptr)
					loc = loc->getInlinedAt ();

				il = (int) loc->getLine () - (int) IL_OFFSET_LINE_BIAS;
			}

			positions.push_back ({ &mbb, it, innermost_try (geometry, il) });
		}
	}

	std::vector<Invoke> invokes;
	std::vector<std::vector<MonoEHClause>> chains;

	for (const LandingPadInfo &lp : pads) {
		const MCSymbol *handler = lp.LandingPadLabel;
		/*
		 * Every entry in mf.getLandingPads () comes from isel lowering a
		 * real `invoke`, and isel always assigns the pad's label at the
		 * same time. No legitimate input leaves it null here, after
		 * addMachinePasses () and before the AsmPrinter runs. A null
		 * label means LLVM's own invariant broke, or we read
		 * LandingPadInfo wrong. Either way, there is no handler to
		 * publish or decline against, so we abort.
		 */
		if (!handler)
			report_fatal_error ("mono: landing pad has no label - LLVM invariant broken");

		/*
		 * mono never marks a landing pad cleanup: landing_pad () and
		 * emit_resume_exit () (method-to-llvm/exceptions.cpp) build
		 * every pad's LandingPadInst and never call setCleanup ().
		 * A finally or fault clause publishes through the same
		 * smuggled type_info_N global a catch uses, not through
		 * LLVM's cleanup bit. So a pad this pass sees must never be
		 * cleanup-flagged. One that is means some code, ours or
		 * LLVM's, set the bit without our knowledge. Treating that
		 * as an ordinary decline hides a case we do not understand,
		 * so we abort instead.
		 */
		if (const BasicBlock *bb =
		        lp.LandingPadBlock ? lp.LandingPadBlock->getBasicBlock () : nullptr) {
			BasicBlock::const_iterator first = bb->getFirstNonPHIIt ();
			if (const auto *lpi = first == bb->end ()
			                          ? nullptr
			                          : dyn_cast<LandingPadInst> (&*first))
				if (lpi->isCleanup ())
					report_fatal_error ("mono: landing pad unexpectedly cleanup-flagged - mono never sets LLVMSetCleanup");
		}

		/*
		 * BeginLabels/EndLabels carry one (begin, end) pair per invoke
		 * that unwinds to this landing pad (SmallVector<MCSymbol*, 1>).
		 * The two vectors are paired by index. That is an LLVM
		 * invariant, not something an input program can violate, so a
		 * length mismatch means LandingPadInfo is broken, or we
		 * misread it.
		 */
		if (lp.BeginLabels.size () != lp.EndLabels.size ())
			report_fatal_error ("mono: landing pad Begin/EndLabels length mismatch - LLVM invariant broken");

		/*
		 * A landing pad with zero invoke ranges can legitimately happen.
		 * If every call this clause protected got optimized to a
		 * nounwind call, its invokes - and so its Begin/EndLabels -
		 * never existed. This clause contributes no protected range.
		 * That is not an error, so it is skipped rather than declining
		 * the whole method.
		 */
		if (lp.BeginLabels.empty ())
			continue;

		std::vector<MonoEHClause> chain;

		/*
		 * MachineFunction::addLandingPad () walks the landingpad's
		 * clauses in reverse when it builds TypeIds. Reversing here
		 * restores the order covering_chain ()
		 * (method-to-llvm/exceptions.cpp) emitted them in: innermost
		 * clause first, then outwards through the enclosers. That order
		 * is the nesting chain, and `.mono_lsda` carries it by position,
		 * so it has to survive this hop.
		 */
		for (auto it = lp.TypeIds.rbegin (); it != lp.TypeIds.rend (); ++it) {
			int type_id = *it;

			/*
			 * type_id < 0 is a filter, an exception-specification.
			 * It is a real, valid `catch (T) when (cond)` clause
			 * this backend does not support yet, out of scope by
			 * design. This is the one legitimate decline left in
			 * this pass. It is recorded in has_filter, separately
			 * from the report_fatal_error aborts above, and only
			 * this TypeId is skipped.
			 */
			if (type_id < 0) {
				fn.has_filter = true;
				fn.declined = true;
				continue;
			}
			/*
			 * type_id == 0 is LLVM's implicit cleanup marker, pushed onto
			 * TypeIds only alongside isCleanup () (see the report_fatal_error
			 * above). We already asserted this landing pad is not
			 * cleanup-flagged, so reaching a zero TypeId here means that
			 * assumption about LLVM's own bookkeeping was wrong.
			 */
			if (type_id == 0)
				report_fatal_error ("mono: TypeId 0 (cleanup marker) on a landing pad that isCleanup () said was not cleanup-flagged");

			MonoEHClause clause;

			clause.handler = handler;

			/*
			 * Which method clause_index indexes into, decoded off the
			 * clause's own marker rather than off anything nearby in the
			 * code: a fold can move a folded body's own code under a
			 * clause of the root's that was never that body's own
			 * (innermost_try ()'s widening above does exactly that, on
			 * purpose), so ambient code position answers a different
			 * question than clause ownership does. ids_ answers with the
			 * compile's own id for the compile's own clause, same as any
			 * other method this compile translated, so the comparison
			 * turns that case back into the 0 every clause carried before
			 * this existed.
			 */
			std::uint64_t marker_owner = 0;

			// TypeIds are 1-based indices into getTypeInfos ().
			if ((size_t) type_id <= type_infos.size ())
				clause.clause_resolved = decode_clause_marker (
					type_infos[type_id - 1], clause.clause_index,
					clause.kind, &marker_owner);
			/*
			 * type_info_N is a global we emit (clause_marker (),
			 * method-to-llvm/exceptions.cpp) with a fixed, known
			 * shape. Failing to read it back means our own emission
			 * or our own reader is wrong, not that the input did
			 * something we do not support.
			 */
			if (!clause.clause_resolved)
				report_fatal_error ("mono: type_info_N clause global did not decode - our own emission or reader is wrong");

			clause.owner = marker_owner == self ? 0 : marker_owner;

			chain.push_back (clause);
		}

		if (chain.empty ())
			continue;

		/*
		 * The begin and end labels come from the same addInvoke () call
		 * that grew Begin/EndLabels. A null one is the same class of
		 * broken invariant as the length mismatch above, not something
		 * unsupported input can produce.
		 */
		for (size_t i = 0; i < lp.BeginLabels.size (); ++i) {
			if (!lp.BeginLabels[i] || !lp.EndLabels[i])
				report_fatal_error ("mono: landing pad invoke range has a null label - LLVM invariant broken");

			auto begin = at_label.find (lp.BeginLabels[i]);
			auto end = at_label.find (lp.EndLabels[i]);

			/*
			 * A label the walk above never reached belongs to a block
			 * the printer will not print, so publishing it names an
			 * address that does not exist. An empty range holds no
			 * instruction either.
			 */
			if (begin == at_label.end () || end == at_label.end ()
			    || begin->second >= end->second)
				continue;

			invokes.push_back ({ begin->second, end->second,
			                     (unsigned) chains.size () });
		}

		chains.push_back (std::move (chain));
	}

	llvm::sort (invokes, [] (const Invoke &a, const Invoke &b) {
		return a.begin < b.begin;
	});

	MCContext &ctx = mf.getContext ();
	const TargetInstrInfo *tii = mf.getSubtarget ().getInstrInfo ();
	unsigned published = 0;
	bool planted = false;

	for (size_t k = 0; k < invokes.size (); ++k) {
		const Invoke &invoke = invokes[k];
		int region = positions[invoke.begin].region;
		unsigned begin = invoke.begin;
		unsigned end = invoke.end;

		/*
		 * The call sits in a try region, so the whole run of code around
		 * it that belongs to the same region is protected by the same
		 * clauses. Widening to that run is what covers a fault away from
		 * a call: a null check LLVM folded into a dereference, or a
		 * dereference inside a copy the backend expanded inline.
		 *
		 * A region of -1 means the IL offset in effect at the call is
		 * outside every try region, which contradicts the pad it unwinds
		 * to. The bare invoke range still describes the call, so that is
		 * what gets published and the widening is skipped.
		 */
		if (region >= 0) {
			while (begin > published && positions[begin - 1].region == region)
				--begin;
			while (end < positions.size () && positions[end].region == region)
				++end;
		}

		// Never past the next call's own range, so the ranges stay disjoint
		// and each one keeps the pad of the call it was grown from.
		if (k + 1 < invokes.size ())
			end = std::min (end, invokes[k + 1].begin);

		published = end;

		MCSymbol *try_begin = plant_label (*positions[begin].mbb,
		                                   positions[begin].at, ctx, tii);
		MCSymbol *try_end =
			end < positions.size ()
				? plant_label (*positions[end].mbb, positions[end].at, ctx, tii)
				: plant_label (mf.back (), mf.back ().end (), ctx, tii);

		planted = true;
		for (MonoEHClause clause : chains[invoke.chain]) {
			clause.try_begin = try_begin;
			clause.try_end = try_end;
			fn.clauses.push_back (clause);
		}
	}

	sc_->functions.push_back (std::move (fn));
	return planted;
}

char MonoEHGatherPass::ID = 0;

} // namespace mono

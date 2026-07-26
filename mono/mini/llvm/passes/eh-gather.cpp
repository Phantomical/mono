/**
 * \file
 * eh-gather.cpp - MonoEHGatherPass, the machine-level recovery of mono's EH
 * clauses from the final landing-pad set.
 */

/*
 * Same reason engine.cpp drops mono's PIC macro: libtool compiles this TU with
 * -DPIC, and LLVM uses `PIC` as an identifier (PassInstrumentationCallbacks), so
 * the macro would rewrite it and break a header.
 */
#ifdef PIC
#undef PIC
#endif

#include "eh-gather.hpp"

#include "../engine.hpp"

#include <algorithm>
#include <string>
#include <vector>

#include <llvm/CodeGen/MachineFunction.h>
#include <llvm/CodeGen/MachineFunctionPass.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/Instructions.h>
#include <llvm/MC/MCSymbol.h>
#include <llvm/Support/ErrorHandling.h>

using namespace llvm;

namespace mono {

bool
MonoEHGatherPass::runOnMachineFunction (MachineFunction &mf)
{
	const std::vector<LandingPadInfo> &pads = mf.getLandingPads ();

	if (pads.empty ()) {
		/*
		 * No landing pads survived to codegen. For an ordinary non-EH
		 * function this is simply nothing to do - stay inert so the emitted
		 * object is byte-identical to a module the EH machinery never
		 * touched (mono-has-eh-clauses is unset; translator.cpp never marks
		 * a method whose IL declared no clauses).
		 *
		 * For a method translator.cpp DID mark mono-has-eh-clauses, zero
		 * landing pads means every protected call under its try region got
		 * optimized to a nounwind call before isel - there is nothing left
		 * that can unwind through it. That is not uncertain, it is
		 * confirmed safe, so record a clean (not declined, no clauses)
		 * entry rather than leaving this function out of the side channel
		 * entirely: C3 turns a present-but-empty entry into a genuinely
		 * empty (but valid) `.mono_lsda` record, which C4 (translator.cpp)
		 * can tell apart from "absent because declined".
		 */
		if (mf.getFunction ().hasFnAttribute ("mono-has-eh-clauses")) {
			MonoEHFunctionClauses fn;
			fn.function = mf.getName ().str ();
			sc_->functions.push_back (std::move (fn));
		}
		return false;
	}

	const std::vector<const GlobalValue *> &type_infos = mf.getTypeInfos ();

	MonoEHFunctionClauses fn;
	fn.function = mf.getName ().str ();

	for (const LandingPadInfo &lp : pads) {
		const MCSymbol *handler = lp.LandingPadLabel;
		/*
		 * Every entry in mf.getLandingPads () was created by isel lowering a
		 * real `invoke`, which always assigns the pad's label at the same
		 * time; there is no legitimate input shape that leaves it null by the
		 * time this pass runs (after addMachinePasses (), before the
		 * AsmPrinter). If it ever is, either LLVM's own invariant broke or we
		 * are misreading LandingPadInfo - either way, keep going would publish
		 * (or wrongly decline) against a handler we cannot name.
		 */
		if (!handler)
			report_fatal_error ("mono: landing pad has no label - LLVM invariant broken");

		/*
		 * mono's own finally/fault emission (emit_handler_start,
		 * translator-call.cpp) never calls LLVMSetCleanup - a finally/fault
		 * clause is published through the same smuggled type_info_N clause a
		 * catch uses, not LLVM's native cleanup bit. So a landing pad this
		 * pass sees should never be cleanup-flagged; if one is, some code path
		 * (ours or LLVM's) set it without our knowledge, and treating that as
		 * an ordinary decline would hide a case we don't understand yet.
		 */
		if (const BasicBlock *bb =
		        lp.LandingPadBlock ? lp.LandingPadBlock->getBasicBlock () : nullptr) {
			if (const auto *lpi =
			        dyn_cast_or_null<LandingPadInst> (bb->getFirstNonPHI ()))
				if (lpi->isCleanup ())
					report_fatal_error ("mono: landing pad unexpectedly cleanup-flagged - mono never sets LLVMSetCleanup");
		}

		/*
		 * BeginLabels/EndLabels carry ONE (begin,end) pair PER INVOKE that
		 * unwinds to this landing pad (SmallVector<MCSymbol*,1>): mono's
		 * emit_call (translator-emit.cpp) issues one LLVMBuildInvoke2 per
		 * protected call in the try - including the implicit null/bounds/div
		 * checks that lower to throw-call invokes - all converging on the
		 * clause's single handler landing pad. So a try with N protected calls
		 * yields ONE landing pad with N invoke ranges. We MUST emit one clause
		 * per invoke range: keeping only the first would publish a
		 * [try_start,try_end) covering only the first call, so a throw from the
		 * 2nd+ call is not is_address_protected and the handler silently never
		 * runs. mono's model supports this directly - is_address_protected
		 * scans all clauses and takes the first PC match, so multiple ei with
		 * the same clause_index/handler over disjoint ranges is expected.
		 * .mono_lsda is therefore "one entry per invoke range"; C3/C4 honor it.
		 *
		 * The two vectors are paired by index - an LLVM invariant, not
		 * something an input program can violate - so a length mismatch means
		 * LandingPadInfo is broken or we are reading it wrong.
		 */
		if (lp.BeginLabels.size () != lp.EndLabels.size ())
			report_fatal_error ("mono: landing pad Begin/EndLabels length mismatch - LLVM invariant broken");
		size_t nranges = std::min (lp.BeginLabels.size (), lp.EndLabels.size ());
		/*
		 * A landing pad with zero invoke ranges CAN legitimately happen: if
		 * every call this specific clause protected got optimized to a
		 * nounwind call before isel, its invokes (and so its Begin/EndLabels)
		 * never existed. This clause contributes no protected range - not an
		 * error, just nothing to publish for it - so it is skipped rather than
		 * declining the whole method.
		 */
		if (nranges == 0)
			continue;

		for (size_t i = 0; i < nranges; ++i) {
			/*
			 * The invoke range and handler entry are the same MCSymbol*s the
			 * AsmPrinter emits into .text; C3 turns them into
			 * func_begin-relative offsets. Both are populated by the same
			 * addInvoke () call that grew Begin/EndLabels, so a null one here
			 * is the same class of broken invariant as the length mismatch
			 * above, not something unsupported input can produce.
			 */
			const MCSymbol *begin = lp.BeginLabels[i];
			const MCSymbol *end = lp.EndLabels[i];
			if (!begin || !end)
				report_fatal_error ("mono: landing pad invoke range has a null label - LLVM invariant broken");

			for (int type_id : lp.TypeIds) {
				/*
				 * type_id < 0 is a filter (exception-specification) - a real,
				 * valid `catch (T) when (cond)` clause we simply don't support
				 * yet (Option F1, out of scope by design). This is the one
				 * legitimate decline left in this pass: flag it explicitly so
				 * translator.cpp can report the specific reason instead of a
				 * generic parse failure, and skip just this TypeId.
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
				clause.try_begin = begin;
				clause.try_end = end;
				clause.handler = handler;

				/*
				 * mono's clause smuggling: TypeIds are 1-based indices into
				 * getTypeInfos(); the referenced type_info_N global's
				 * initializer carries the IL clause index AND the clause's
				 * flags (kind). Recover both in-process - no ttype-table deref,
				 * no relocation dependency.
				 *
				 * v2 form: a 2-word {i32 clause_index, i32 kind} struct
				 * (emit_handler_start, translator-call.cpp). A bare i32
				 * ConstantInt is the legacy 1-word form (clause_index only,
				 * kind stays NONE=0) and is still accepted. An all-zero struct
				 * lowers to ConstantAggregateZero, so the two words are read
				 * with Constant::getAggregateElement (not dyn_cast<ConstantStruct>,
				 * which a zero aggregate is not).
				 */
				if ((size_t) type_id <= type_infos.size ()) {
					const GlobalValue *gv = type_infos[type_id - 1];
					if (const auto *var = dyn_cast_or_null<GlobalVariable> (gv)) {
						if (var->hasInitializer ()) {
							const Constant *init = var->getInitializer ();
							if (const auto *ci = dyn_cast<ConstantInt> (init)) {
								clause.clause_index = (int) ci->getSExtValue ();
								clause.kind = 0;
								clause.clause_resolved = true;
							} else if (auto *st =
							               dyn_cast<StructType> (init->getType ())) {
								if (st->getNumElements () == 2) {
									const auto *ci0 = dyn_cast_or_null<ConstantInt> (
									        init->getAggregateElement ((unsigned) 0));
									const auto *ci1 = dyn_cast_or_null<ConstantInt> (
									        init->getAggregateElement ((unsigned) 1));
									if (ci0 && ci1) {
										clause.clause_index = (int) ci0->getSExtValue ();
										clause.kind = (int) ci1->getZExtValue ();
										clause.clause_resolved = true;
									}
								}
							}
						}
					}
				}
				/*
				 * type_info_N is a global WE emit (emit_handler_start,
				 * translator-call.cpp) with a fixed, known shape. Failing to
				 * read it back means our own emission or our own reader is
				 * wrong, not that the input did something we don't support.
				 */
				if (!clause.clause_resolved)
					report_fatal_error ("mono: type_info_N clause global did not decode - our own emission or reader is wrong");

				fn.clauses.push_back (clause);
			}
		}
	}

	sc_->functions.push_back (std::move (fn));
	return false;
}

char MonoEHGatherPass::ID = 0;

} // namespace mono

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

	MonoEHFunctionClauses fn;
	fn.function = mf.getName ().str ();

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
			if (const auto *lpi =
			        dyn_cast_or_null<LandingPadInst> (bb->getFirstNonPHI ()))
				if (lpi->isCleanup ())
					report_fatal_error ("mono: landing pad unexpectedly cleanup-flagged - mono never sets LLVMSetCleanup");
		}

		/*
		 * BeginLabels/EndLabels carry one (begin, end) pair per invoke
		 * that unwinds to this landing pad (SmallVector<MCSymbol*, 1>).
		 * Every protected call in the try becomes its own invoke - the
		 * emitted null, bounds and div-by-zero checks included -
		 * converging on the clause's one handler pad. Both come from
		 * emit_protected_call and emit_unwinding_call
		 * (method-to-llvm/exceptions.cpp). So a try with N protected
		 * calls yields one landing pad with N invoke ranges.
		 *
		 * We emit one clause per invoke range. Keeping only the first
		 * publishes a [try_start, try_end) that covers only the first
		 * call. A throw from the second call on is then not
		 * is_address_protected, so the handler silently never runs.
		 * mono's model supports this directly. is_address_protected
		 * scans all clauses and takes the first PC match, so several
		 * entries can share one clause_index and handler over
		 * disjoint ranges. `.mono_lsda` is therefore one entry per
		 * invoke range, and this pass and the side-table writer
		 * (compiler.cpp) both honor it.
		 *
		 * The two vectors are paired by index. That is an LLVM
		 * invariant, not something an input program can violate, so a
		 * length mismatch means LandingPadInfo is broken, or we
		 * misread it.
		 */
		if (lp.BeginLabels.size () != lp.EndLabels.size ())
			report_fatal_error ("mono: landing pad Begin/EndLabels length mismatch - LLVM invariant broken");
		size_t nranges = std::min (lp.BeginLabels.size (), lp.EndLabels.size ());
		/*
		 * A landing pad with zero invoke ranges can legitimately happen.
		 * If every call this clause protected got optimized to a
		 * nounwind call before isel, its invokes - and so its
		 * Begin/EndLabels - never existed. This clause contributes no
		 * protected range. That is not an error, so it is skipped
		 * rather than declining the whole method.
		 */
		if (nranges == 0)
			continue;

		for (size_t i = 0; i < nranges; ++i) {
			/*
			 * The invoke range and the handler entry are all
			 * MCSymbol*s the AsmPrinter emits into .text. The
			 * side-table writer (compiler.cpp) turns them into
			 * func_begin-relative offsets. The begin and end labels
			 * come from the same addInvoke () call that grew
			 * Begin/EndLabels. A null one here is the same class of
			 * broken invariant as the length mismatch above, not
			 * something unsupported input can produce.
			 */
			const MCSymbol *begin = lp.BeginLabels[i];
			const MCSymbol *end = lp.EndLabels[i];
			if (!begin || !end)
				report_fatal_error ("mono: landing pad invoke range has a null label - LLVM invariant broken");

			/*
			 * MachineFunction::addLandingPad () walks the
			 * landingpad's clauses in reverse when it builds
			 * TypeIds. Reversing here restores the order
			 * covering_chain () (method-to-llvm/exceptions.cpp)
			 * emitted them in: innermost clause first, then
			 * outwards through the enclosers. That order is the
			 * nesting chain, and `.mono_lsda` carries it by
			 * position, so it has to survive this hop.
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
				clause.try_begin = begin;
				clause.try_end = end;
				clause.handler = handler;

				/*
				 * mono's clause smuggling: TypeIds are 1-based indices
				 * into getTypeInfos (). The referenced type_info_N
				 * global's initializer carries the IL clause index and
				 * the clause's flags (kind). We recover both
				 * in-process, with no ttype-table deref and no
				 * relocation dependency.
				 *
				 * v2 form: a 2-word {i32 clause_index, i32 kind}
				 * struct (clause_marker (),
				 * method-to-llvm/exceptions.cpp). A bare i32
				 * ConstantInt is the legacy 1-word form (clause_index
				 * only, kind stays 0) and is still accepted. An
				 * all-zero struct lowers to ConstantAggregateZero, so
				 * the two words are read with
				 * Constant::getAggregateElement, not
				 * dyn_cast<ConstantStruct>, which a zero aggregate is
				 * not.
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
				 * type_info_N is a global we emit (clause_marker (),
				 * method-to-llvm/exceptions.cpp) with a fixed, known
				 * shape. Failing to read it back means our own emission
				 * or our own reader is wrong, not that the input did
				 * something we do not support.
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

/**
 * \file
 * \brief Lowers the GC write barrier to an inline conditional card mark.
 *
 * Boehm exposes no card table to inline against, so mini_emit_write_barrier ()
 * calls a wrapper that sets one bit in its dirty-page bitmap with an atomic OR.
 * The translator tags those calls `mono.wbarrier`; this pass replaces each one
 * with the equivalent inline:
 *
 *     fence release              ; pins the store this barrier guards
 *     cur = load monotonic *word ; coherent with concurrent RMWs on the same
 *                                 ; word (aliasing)
 *     if (!(cur & mask))
 *             atomicrmw or release *word, mask
 *
 * Skipping the atomic when the page is already dirty is the whole point - the
 * common case in steady state. The `fence` exists because Boehm suspends
 * mutators with a signal, not at a safepoint: a stop-the-world can land
 * between any two instructions here, including before the guarded store has
 * executed. A stale "already marked" decision drops the mark for good - the
 * bit only ever goes 0->1 while the world is stopped - so once that decision
 * is made against the wrong epoch, nothing corrects it. The RMW's `release` is
 * a real, cross-thread one (unlike the fence, which only needs to be
 * `SingleThread`-scoped), because it's the one operation the collector
 * actually observes.
 */

#ifndef MONO_MINI_LLVM_PASSES_WBARRIER_HPP
#define MONO_MINI_LLVM_PASSES_WBARRIER_HPP

/*
 * PassManager.h uses PIC as an identifier, and libtool passes -DPIC (as does
 * mono-tls.h when we are built as PIE), so the macro has to go before any LLVM
 * header - same dance as the other passes here.
 */
#ifdef PIC
#undef PIC
#endif

#include <cstdint>
#include <optional>

#include <llvm/IR/PassManager.h>

namespace llvm {
class Function;
class PassBuilder;
} // namespace llvm

namespace mono {

/*
 * Where the dirty-page bitmap lives and how an address indexes it, as reported
 * by mono_gc_get_card_bitmap ().
 */
struct CardBitmap {
	/* Base of the bitmap, as a constant address baked into the JITted code. */
	uint64_t table;
	/* Address to page index. */
	unsigned shift;
	/* Page index to bit index; the table aliases past this many pages. */
	uint64_t index_mask;
};

/*
 * The running collector's bitmap, or nullopt when it has none and barriers have
 * to stay as calls. Queried once per pipeline build rather than cached, since
 * the answer changes when incremental mode is switched on.
 */
std::optional<CardBitmap> card_bitmap ();

/*
 * Replaces every `mono.wbarrier`-tagged call in the function with the inline
 * card mark described above.
 */
class WriteBarrierLoweringPass : public llvm::PassInfoMixin<WriteBarrierLoweringPass> {
public:
	explicit WriteBarrierLoweringPass (CardBitmap bitmap) : bitmap_ (bitmap) {}

	llvm::PreservedAnalyses run (llvm::Function &f, llvm::FunctionAnalysisManager &fam);

private:
	CardBitmap bitmap_;
};

/*
 * Schedules the lowering inside PB's function simplification pipeline. A no-op
 * when the collector has no bitmap, which leaves the barrier calls alone and
 * the runtime behaving exactly as it did. Call before building a pipeline
 * from PB.
 */
void register_write_barrier_lowering (llvm::PassBuilder &pb);

} // namespace mono

#endif /* MONO_MINI_LLVM_PASSES_WBARRIER_HPP */

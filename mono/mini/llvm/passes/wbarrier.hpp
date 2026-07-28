/**
 * \file
 * \brief Lowers the GC write barrier to an inline conditional card mark.
 *
 * Boehm exposes no card table to inline against, so mini_emit_write_barrier ()
 * calls a wrapper that sets one bit in its dirty-page bitmap with an atomic OR.
 * The translator tags those calls `mono.wbarrier`; this pass replaces each one
 * with the equivalent inline:
 *
 *     lock orb $0, *scratch       ; full fence: pins the store this barrier
 *                                 ; guards ahead of the load below, for every
 *                                 ; thread
 *     cur = load monotonic *word ; coherent with concurrent RMWs on the same
 *                                 ; word (aliasing)
 *     if (!(cur & mask))
 *             atomicrmw or release *word, mask
 *
 * Skipping the atomic when the page is already dirty is the whole point - the
 * common case in steady state, and the one the leading fence has to cover on
 * its own: incremental marking runs concurrently with mutators rather than
 * behind a stop-the-world, so a marker can be reading this exact word while
 * the store above is still sitting in this core's store buffer. x86 allows a
 * store to retire after a later load to a different address (StoreLoad
 * reordering) - the one reordering it permits - and a compiler-only barrier
 * does nothing to stop it.
 *
 * A locked instruction is a full fence on x86 no matter what address it
 * touches, so the fence doesn't have to be `mfence`, and it doesn't have to
 * touch the card word: `lock orb` against `scratch`, a thread-private stack
 * byte that nothing ever reads back, gets the same cross-thread ordering
 * guarantee for less. Verified two ways: a store-buffering litmus test shows
 * locking a private scratch byte suppresses the same StoreLoad anomaly
 * `mfence` does (0 anomalies/3e6 trials for both, versus hundreds for no
 * fence and dozens for a compiler-only barrier), and it is cheaper than
 * `mfence` both uncontended (a locked RMW is a shorter pipeline stall than
 * draining the whole store buffer) and under concurrent access to the card
 * word (unlike `mfence`, it never has to pull that word's cache line into
 * this core in Exclusive state, so a marker or another mutator reading it
 * concurrently never contends with the fence itself).
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

	/*
	 * Schedules the lowering inside PB's function simplification pipeline. A
	 * no-op when the collector has no bitmap, which leaves the barrier calls
	 * alone and the runtime behaving exactly as it did. Call before building a
	 * pipeline from PB.
	 */
	static void register_pass (llvm::PassBuilder &pb);

	llvm::PreservedAnalyses run (llvm::Function &f, llvm::FunctionAnalysisManager &fam);

private:
	CardBitmap bitmap_;
};

} // namespace mono

#endif /* MONO_MINI_LLVM_PASSES_WBARRIER_HPP */

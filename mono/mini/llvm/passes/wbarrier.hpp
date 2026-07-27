/**
 * \file
 * \brief Lowers the GC write barrier to an inline conditional card mark.
 *
 * A collector that exposes no card table to inline against leaves
 * mini_emit_write_barrier () with only one option: call a wrapper, once per
 * reference store, ending in mono_gc_wbarrier_generic_nostore_internal (), which
 * sets one bit in boehm's dirty-page bitmap with an atomic OR. The translator
 * tags those calls `mono.wbarrier` (translator-call.cpp); this pass replaces
 * each one with the bit-set written out in line:
 *
 *     index = (address >> shift) & index_mask
 *     word  = &table[index / bits_per_word]
 *     bit   = 1 << (index % bits_per_word)
 *     if (!(*word & bit))
 *             *word |= bit
 *
 * The test is worth more than the removed call. It skips the atomic entirely for
 * a page that is already dirty, which in steady state is nearly every store - a
 * page holds a great many objects, and nothing clears the bit until the next
 * collection. What remains on the fast path is a shared load, so barriers
 * running on other cores keep the line in a shared state instead of trading
 * ownership of it on every reference store. The unconditional OR cannot: it is a
 * read-modify-write, and one word covers a whole run of pages, so it contends
 * far beyond the object that caused it.
 *
 * Skipping a mark on a stale read is safe because the bit only ever goes 0->1
 * while managed code runs. The collector clears the table in GC_read_dirty (),
 * reached from GC_initiate_gc () with the world already stopped. So of the two
 * ways the load can be out of date, only the harmless one is reachable: reading
 * 0 for a bit that is really set costs a redundant OR, while reading 1 for a bit
 * that is really clear would drop the mark - and that needs somebody clearing
 * concurrently, which nobody does.
 *
 * That is also why the load is `unordered` and needs no fence. It establishes
 * nothing; it is a hint whose only wrong answer is unreachable. Unordered rather
 * than monotonic because it is free to be - both are a plain load on every
 * target we emit for - and because it lets LICM hoist the load out of a loop and
 * lets GVN forward it across an earlier barrier's untaken arm. That second one
 * is the common case rather than a curiosity: a run of reference stores into one
 * object, which is what a constructor is, is a run of barriers on one word.
 *
 * The RMW is `release` so a thread that observes the mark also observes the
 * reference store that prompted it. The fast path skips that release, which is
 * fine for the same reason the rest of this is: the collector only ever reaches
 * these bits through a stop-the-world, and that orders every mutator store ahead
 * of it regardless.
 *
 * Every one of those arguments rests on the bits being cleared only while the
 * world is stopped. If that ever stops being true, this transform stops being
 * correct - the conditional test has to go back to being an unconditional mark.
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

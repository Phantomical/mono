/**
 * \file
 * \brief The declaration a reference store's write barrier is written as, and
 * the card path or collector helper it lowers to.
 */

#ifndef MONO_LLVM_PASSES_GC_BARRIER_HPP
#define MONO_LLVM_PASSES_GC_BARRIER_HPP

#include <llvm/ADT/StringRef.h>

#include <cstdint>

namespace llvm {
class Function;
class Module;
} // namespace llvm

namespace mono {

/*
 * The declaration takes the two operands of the store it belongs to:
 *
 *   void @mono.gc.wbarrier (ptr addr, ptr value)
 *
 * The store stands beside the call rather than inside it, so what the program
 * wrote to the field stays visible to the optimizer whatever the collector
 * needs for the card. addr is the location that store wrote, and value is the
 * reference it put there, which is what the nursery test reads.
 *
 * The call claims memory(argmem: read, inaccessiblemem: readwrite). Neither
 * collector reads the slot back, so the read of argument memory is an
 * over-claim, and it buys the edge that keeps the card mark behind the store. A
 * card marked in front of a collection that cleans it leaves the store behind
 * with no entry, and the next collection then drops the object that field
 * names. The card table and the concurrent-collection flag are the inaccessible
 * half, and `mono.alloc.object` claims that component as well, so an allocation
 * and a barrier do not move over each other.
 *
 * One collector runs for the life of the process, so its layout rides on the
 * declaration instead of on each site.
 */

/// The addresses and shifts a reference store needs to mark a card itself.
///
/// A null card_table is a collector that marks no cards, and the other fields
/// then say nothing. That is the one field to test before you read the rest.
struct GcBarrierLayout {
	void *card_table = nullptr;
	uintptr_t card_mask = 0;
	int card_bits = 0;
	void *nursery_start = nullptr;
	int nursery_bits = 0;

	/// True where the value alone decides the card, which is a major collector
	/// that collects nothing concurrently.
	bool value_decides = false;

	/// The flag a concurrent major collector raises while it marks, or null
	/// where the collector keeps none.
	void *concurrent_flag = nullptr;

	/// The width of that flag in bytes.
	unsigned concurrent_flag_size = 0;
};

/// Marks the card for a reference the caller has already stored.
constexpr llvm::StringRef gc_barrier_name = "mono.gc.wbarrier";

// The card path names these two addresses. The lowering makes a global for each
// one. The translator records the address under the same name, because a pass
// cannot reach the engine's list of externals. Both addresses are fixed for the
// process, so a record from any compile that writes a barrier resolves every
// site.
constexpr llvm::StringRef gc_card_table_symbol = "mono_gc_card_table";
constexpr llvm::StringRef gc_concurrent_flag_symbol =
	"mono_gc_concurrent_collection_flag";

/// What a collector with no card table marks through instead.
constexpr llvm::StringRef gc_barrier_helper_name =
	"mono_gc_wbarrier_generic_nostore_internal";

/// The declaration in \p m, made on first use and stamped with \p layout.
llvm::Function *gc_barrier_decl (llvm::Module &m, const GcBarrierLayout &layout);

/// Rewrites every barrier call into the card path the layout on its declaration
/// describes, or into the collector's own helper where that layout holds no card
/// table. Erases the declaration, and says whether it changed anything.
bool lower_gc_barriers (llvm::Module &m);

} // namespace mono

#endif

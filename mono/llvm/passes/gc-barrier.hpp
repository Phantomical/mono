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
class Value;
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

/*
 * The value-copy form is the copy and the cards together:
 *
 *   void @mono.gc.wbarrier.value.copy (ptr dest, ptr src, i32 count, i64 size,
 *                                      ptr klass)
 *
 * The translator writes every copy of a value type that holds references as this
 * one call. count and klass are what the icall behind it takes. size is the
 * bytes one element holds, which is what the fold needs and the icall works out
 * again from klass.
 *
 * The copy stays inside the call, so the optimizer reads neither end of it. What
 * the declaration buys is the claims: the icall on its own carries none, so it
 * captures both pointers and writes every location the module holds.
 *
 * `open_value_copies ()` (`passes/fold-barrier.cpp`) replaces the call with a
 * bare memcpy or memmove where dest is a stack slot. A card records an
 * old-to-young reference in the heap, and the collector scans a frame as a root
 * at every collection, so a copy that lands in one owes no card.
 *
 * The declaration writes through dest, so it claims argmem readwrite where the
 * single-reference form above claims argmem read.
 */

/// Copies a value type that holds references, and marks the cards it owes.
constexpr llvm::StringRef gc_value_copy_name = "mono.gc.wbarrier.value.copy";

/// The site attribute that says the destination and the source cannot overlap,
/// which is what lets the fold write a memcpy in place of a memmove.
constexpr llvm::StringRef gc_no_overlap_attr = "mono-gc-copy-no-overlap";

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

/// The icall that copies a value and marks the cards inside one call. Every
/// collector has one, so the value-copy form lowers to this whatever the layout
/// says.
constexpr llvm::StringRef gc_value_copy_helper_name =
	"mono_gc_wbarrier_value_copy_internal";

/// The declaration in \p m, made on first use and stamped with \p layout.
llvm::Function *gc_barrier_decl (llvm::Module &m, const GcBarrierLayout &layout);

/// The value-copy declaration in \p m, made on first use and stamped with
/// \p layout.
llvm::Function *gc_value_copy_decl (llvm::Module &m, const GcBarrierLayout &layout);

/// The layout \p decl was stamped with. \p decl must be one of the two
/// declarations above.
GcBarrierLayout gc_barrier_layout (const llvm::Function &decl);

/// Whether the IR settles \p pointer to a slot in the frame.
///
/// The collector reads a card to find a heap field that names a young object. It
/// scans a frame as a root at each collection instead, so a write into a frame
/// slot owes no card. `mono_gc_wbarrier_value_copy_internal ()`
/// (`mono/metadata/sgen-mono.c`) takes the same decision at run time, on
/// `ptr_on_stack (dest)`.
///
/// `getUnderlyingObject ()` stops at a phi and at a select, so a pointer that is
/// a frame slot on one arm and an object on the other reads as neither.
bool points_to_the_frame (const llvm::Value *pointer);

/// Rewrites every barrier call into the card path the layout on its declaration
/// describes, or into the collector's own helper where that layout holds no card
/// table. Erases the declaration, and says whether it changed anything.
bool lower_gc_barriers (llvm::Module &m);

/// Rewrites every value-copy call the fold left standing into the collector's
/// own copy-and-mark icall. Erases the declaration, and says whether it changed
/// anything.
bool lower_gc_value_copies (llvm::Module &m);

} // namespace mono

#endif

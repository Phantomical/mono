/**
 * \file
 * \brief Whether an allocation's pointer can be reached from outside the
 * function that makes it.
 */

#ifndef MONO_LLVM_ANALYSIS_ESCAPE_HPP
#define MONO_LLVM_ANALYSIS_ESCAPE_HPP

#include <llvm/ADT/STLFunctionalExtras.h>

namespace llvm {
class CallBase;
class Value;
} // namespace llvm

namespace mono {

/// The allocation \p pointer settles to, or null where it settles to something
/// else. It reads the four allocation builtins, the `.kept` forms included.
llvm::CallBase *allocation_behind (llvm::Value *pointer);

/**
 * Whether the object \p alloc answers with can be reached from outside the
 * function that makes it.
 *
 * The answer is about reach alone. An object nothing outside can reach is still
 * read inside, so a caller that wants to take the allocation away owes that
 * second question of its own.
 *
 * LLVM's own tracker reads a store of a pointer as a capture without looking at
 * where the store goes (`DetermineUseCaptureKind ()`,
 * `llvm/lib/Analysis/CaptureTracking.cpp`). Such a store captures the value
 * exactly as far as the destination object is captured, which is a relation
 * between two values and not something the per-store `!captures` metadata can
 * say. So this walk answers it here.
 *
 * \p keeps_it_inside is asked about the allocation behind such a store's
 * destination, and answering true makes that store not a way out. It has to
 * claim two things of that object, and the caller owes both:
 *
 *   - nothing outside the function reaches the object, and
 *   - nothing reads back what the object holds.
 *
 * The second is what makes the first enough. An object that stays inside can
 * still be read: a load of the field this store wrote gives out the pointer that
 * was stored, and the walk never sees that load, because it is a use of the
 * destination rather than of the pointer this walk follows.
 *
 * `EraseDeadAllocationsPass` vouches with the set of allocations it is erasing
 * this round, which claims both. An object it erases is unreachable and unread,
 * and the store itself goes with it.
 */
bool allocation_escapes (llvm::CallBase &alloc,
                         llvm::function_ref<bool (llvm::CallBase &)> keeps_it_inside);

} // namespace mono

#endif

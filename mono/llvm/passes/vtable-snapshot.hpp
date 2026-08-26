/**
 * \file
 * \brief The constant a class's vtable stands as while a compile optimizes, and
 * the sweep that takes it back off.
 */

#ifndef MONO_LLVM_PASSES_VTABLE_SNAPSHOT_HPP
#define MONO_LLVM_PASSES_VTABLE_SNAPSHOT_HPP

#include <llvm/ADT/SmallVector.h>
#include <llvm/ADT/StringRef.h>
#include <llvm/IR/PassManager.h>
#include <llvm/Support/Error.h>

#include <cstdint>

namespace llvm {
class Constant;
class GlobalVariable;
class Module;
class Type;
} // namespace llvm

namespace mono {

/*
 * A snapshot is a defined constant standing for one run-time object, laid out at
 * that object's own size and alignment. The size stops a load past the last
 * field from folding against whatever follows. Each offset in one is:
 *
 *   stated    fixed for the object's life. The initializer holds the value and
 *             a load there folds.
 *   named     a pointer to another object whose facts are wanted. The
 *             initializer holds that object's snapshot.
 *   withheld  everything else. Generated code reads such a field through a
 *             builtin, so what the initializer holds there is never read.
 *
 * vtable_snapshot_states () is the inventory and the strip refuses a plain load
 * that lands anywhere else. Without that refusal a withheld field folds to the
 * padding byte the initializer put there, which is a wrong answer and no crash.
 * gc_bits is what makes the refusal worth its cost: the collector writes it, and
 * it shares a storage unit with three more fields.
 */

/// Marks a global as a class's vtable for as long as it carries an initializer.
///
/// The strip reads this, and so does the check behind it. A global without it is
/// an ordinary external symbol and is left alone.
constexpr llvm::StringRef vtable_snapshot_metadata = "mono.vtable.snapshot";

/// Marks \p snapshot as a vtable a compile can read, and gives it the linkage
/// and alignment one carries.
void mark_vtable_snapshot (llvm::GlobalVariable &snapshot);

/// The values a snapshot states about a class's vtable, one for each stated
/// field.
///
/// Both pointers are symbols rather than addresses, so a comparison against the
/// same symbol folds. A type test reads the class word, and `typeof` names the
/// `System.Type` object under the symbol `type` carries here.
struct VTableConstants {
	llvm::Constant *klass = nullptr;
	llvm::Constant *type = nullptr;
	uint8_t rank = 0;

	/// One entry for each dispatch slot, in vtable order. A slot the compile
	/// can name holds the callee's own declaration, which makes a folded
	/// dispatch a direct call an inliner can act on. Any other holds the
	/// address the runtime's own slot holds.
	///
	/// Every slot the class has, or none at all: a load past the last entry
	/// folds against whatever the constant ends with.
	llvm::SmallVector<llvm::Constant *, 16> slots;
};

/// The initializer a snapshot carries, laid out for as many slots as \p held
/// gives.
llvm::Constant *vtable_snapshot_init (llvm::Module &m, const VTableConstants &held);

/// The type a snapshot of a vtable with \p slots dispatch slots is laid out as.
///
/// Packed, with each stated field at the offset `MonoVTable` puts it, and the
/// bytes between them padding. So an initializer built against this type reads
/// back through the offsets generated code already uses. Fails the process where
/// the result is not the size the runtime gives a vtable of that many slots.
llvm::Type *vtable_snapshot_type (llvm::Module &m, uint32_t slots);

/// Whether a snapshot of a vtable with \p slots slots states the field at
/// \p offset. Generated code can read a stated field as a plain load.
bool vtable_snapshot_states (uint64_t offset, uint32_t slots);

/// Turns every vtable snapshot the module still defines back into the external
/// symbol the link resolves to the real `MonoVTable`.
///
/// The global keeps its name and its identity, so a use the optimization left
/// standing needs no rewriting. Run it in front of `LowerVTableFuncPass`, which
/// reads an IMT slot at a negative offset from the base — memory the snapshot
/// does not describe.
///
/// Fails the process on a plain load that lands on a withheld field.
class StripVTableSnapshotPass : public llvm::PassInfoMixin<StripVTableSnapshotPass> {
public:
	llvm::PreservedAnalyses run (llvm::Module &m, llvm::ModuleAnalysisManager &mam);
};

/// Returns an error naming a vtable snapshot the module still defines.
llvm::Error vtable_snapshots_stripped (const llvm::Module &m);

} // namespace mono

#endif

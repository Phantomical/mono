#include "vtable-snapshot.hpp"

#include "mono/metadata/abi-details.h"
#include "mono/metadata/class-internals.h"

#include <llvm/ADT/APInt.h>
#include <llvm/ADT/SmallVector.h>
#include <llvm/ADT/Twine.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DataLayout.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Metadata.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Operator.h>
#include <llvm/Support/Alignment.h>
#include <llvm/Support/ErrorHandling.h>

using namespace llvm;

namespace mono {
namespace {

/// One field a snapshot states, at the offset `MonoVTable` puts it.
struct StatedField {
	uint64_t at;
	uint64_t width;
};

/*
 * The inventory. mono_class_create_runtime_vtable () writes each of these
 * between object.c:2183 and :2314 and nothing writes them again. initialized is
 * the one that moves after that, and it moves once: it rises when the type
 * initializer returns and never falls.
 *
 * type and the dispatch slots are left out, and both are fixed for the vtable's
 * life too. Neither is a value a compile can write down. The System.Type object
 * moves, and a slot holds an address the runtime chooses, so each takes a form
 * of its own.
 */
const StatedField stated[] = {
	{ MONO_STRUCT_OFFSET (MonoVTable, klass), sizeof (MonoClass *) },
	{ MONO_STRUCT_OFFSET (MonoVTable, gc_descr), sizeof (MonoGCDescriptor) },
	{ MONO_STRUCT_OFFSET (MonoVTable, domain), sizeof (MonoDomain *) },
	{ MONO_STRUCT_OFFSET (MonoVTable, interface_bitmap), sizeof (guint8 *) },
	{ MONO_STRUCT_OFFSET (MonoVTable, max_interface_id), sizeof (guint32) },
	{ MONO_STRUCT_OFFSET (MonoVTable, rank), sizeof (guint8) },
	{ MONO_STRUCT_OFFSET (MonoVTable, initialized), sizeof (guint8) },
};

/// The bytes a vtable with \p slots dispatch slots occupies.
///
/// A class with static fields gets one word more, which holds the block they
/// live in (`object.c:2224`). Generated code reads that block through the
/// class's own `mono_statics_` symbol, so the snapshot stops before it.
uint64_t
vtable_bytes (uint32_t slots)
{
	return MONO_SIZEOF_VTABLE + uint64_t (slots) * sizeof (gpointer);
}

/// The snapshots \p m defines.
SmallVector<GlobalVariable *, 8>
snapshots_in (Module &m)
{
	SmallVector<GlobalVariable *, 8> found;

	for (GlobalVariable &g : m.globals ())
		if (g.hasMetadata (vtable_snapshot_metadata))
			found.push_back (&g);

	return found;
}

/// Fails the process where a plain load reads \p snapshot anywhere the inventory
/// does not cover.
///
/// The walk goes over the snapshot's own users rather than over every
/// instruction, which is what keeps it cheap enough to run unconditionally. On
/// an opaque pointer a getelementptr is the only address arithmetic, so
/// following those reaches every load that can name this global.
void
refuse_withheld_reads (const GlobalVariable &snapshot, uint32_t slots)
{
	const DataLayout &dl = snapshot.getParent ()->getDataLayout ();
	SmallVector<const User *, 8> work;

	work.append (snapshot.user_begin (), snapshot.user_end ());

	while (!work.empty ()) {
		const User *user = work.pop_back_val ();

		if (isa<GEPOperator>(user)) {
			work.append (user->user_begin (), user->user_end ());
			continue;
		}

		const auto *load = dyn_cast<LoadInst> (user);

		if (load == nullptr)
			continue;

		APInt offset (64, 0);
		const Value *base = load->getPointerOperand ()
		                            ->stripAndAccumulateConstantOffsets (
						    dl, offset, /*AllowNonInbounds=*/true);

		// An offset the walk cannot read is refused with the rest. A load
		// this cannot place is one this cannot show lands on a stated field.
		if (base == &snapshot
		    && vtable_snapshot_states (offset.getZExtValue (), slots))
			continue;

		report_fatal_error (Twine ("a load reads ") + snapshot.getName ()
		                    + " at an offset the snapshot does not state");
	}
}

/// The slots \p snapshot was built with, read back off its own type.
uint32_t
slots_of (const GlobalVariable &snapshot)
{
	const DataLayout &dl = snapshot.getParent ()->getDataLayout ();
	uint64_t bytes = dl.getTypeAllocSize (snapshot.getValueType ());

	if (bytes < MONO_SIZEOF_VTABLE)
		return 0;

	return uint32_t ((bytes - MONO_SIZEOF_VTABLE) / sizeof (gpointer));
}

} // namespace

void
mark_vtable_snapshot (GlobalVariable &snapshot)
{
	LLVMContext &c = snapshot.getContext ();

	snapshot.setMetadata (vtable_snapshot_metadata, MDNode::get (c, {}));
	snapshot.setAlignment (Align (alignof (MonoVTable)));

	// The link resolves the name to the real vtable once the strip has run, so
	// the linkage stays what the translator gave it. `constant` is what makes a
	// load fold. hasDefinitiveInitializer () wants an initializer the module
	// owns and a linkage no other module can interpose, and external is one.
	snapshot.setConstant (true);
	snapshot.setDSOLocal (true);
}

Type *
vtable_snapshot_type (Module &m, uint32_t slots)
{
	LLVMContext &c = m.getContext ();
	Type *byte = Type::getInt8Ty (c);
	SmallVector<Type *, 16> members;
	uint64_t at = 0;

	auto pad_to = [&] (uint64_t offset) {
		if (offset > at)
			members.push_back (ArrayType::get (byte, offset - at));
		at = offset;
	};

	for (const StatedField &field : stated) {
		pad_to (field.at);
		members.push_back (IntegerType::get (c, field.width * 8));
		at += field.width;
	}

	pad_to (vtable_bytes (slots));

	// Packed, so each field sits where the offset above put it rather than
	// where LLVM's own alignment rules would.
	Type *built = StructType::get (c, members, /*isPacked=*/true);

	if (m.getDataLayout ().getTypeAllocSize (built) != vtable_bytes (slots))
		report_fatal_error ("the vtable snapshot layout is not MonoVTable's");

	return built;
}

bool
vtable_snapshot_states (uint64_t offset, uint32_t slots)
{
	for (const StatedField &field : stated)
		if (offset >= field.at && offset < field.at + field.width)
			return true;

	(void) slots;
	return false;
}

PreservedAnalyses
StripVTableSnapshotPass::run (Module &m, ModuleAnalysisManager &)
{
	SmallVector<GlobalVariable *, 8> found = snapshots_in (m);

	if (found.empty ())
		return PreservedAnalyses::all ();

	for (GlobalVariable *snapshot : found) {
		refuse_withheld_reads (*snapshot, slots_of (*snapshot));

		// The initializer is what made this a definition. Dropping it leaves
		// the declaration the translator wrote before the snapshot, under the
		// same name and with the same uses. The value type stays the struct:
		// a declaration emits no bytes whatever its type, and every reader
		// here builds its own byte offsets.
		snapshot->setInitializer (nullptr);
		snapshot->setConstant (false);
		snapshot->setLinkage (GlobalValue::ExternalLinkage);
		snapshot->eraseMetadata (
			m.getContext ().getMDKindID (vtable_snapshot_metadata));
	}

	return PreservedAnalyses::none ();
}

Error
vtable_snapshots_stripped (const Module &m)
{
	for (const GlobalVariable &g : m.globals ()) {
		if (!g.hasMetadata (vtable_snapshot_metadata))
			continue;

		return createStringError (
			inconvertibleErrorCode (),
			"%s stood as a vtable a compile could read and was never "
			"taken back to its symbol",
			g.getName ().str ().c_str ());
	}

	return Error::success ();
}

} // namespace mono

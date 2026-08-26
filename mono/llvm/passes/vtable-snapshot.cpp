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
///
/// A pointer field keeps a pointer type, because the value it states is a symbol
/// and a reader loads it as a pointer. Folding one through an integer of the
/// same width goes through a reinterpretation that answers nothing useful.
struct StatedField {
	uint64_t at;
	uint64_t width;
	bool pointer;
};

/// Which field each entry of the inventory below is, for the value builder to
/// name rather than count to.
enum StatedIndex { stated_klass, stated_type, stated_rank, stated_count };

/*
 * The inventory. mono_class_create_runtime_vtable () writes each of these once,
 * while it builds the vtable, and nothing writes them again.
 *
 * klass and rank come off the class alone (object.c:2183-2184), so a compile
 * states them with no vtable to read. type is the System.Type object at
 * object.c:2355, which mono_type_get_object_checked () allocates pinned
 * (reflection.c:536) because the runtime stores it in vtables and in compiled
 * code. So its address is one a compile can write down. A type built through
 * Reflection.Emit is the exception: its object is the builder's own and is not
 * pinned, and vtable_symbol () gives such a class no snapshot at all.
 *
 * A withheld field is one an emitter must not read as a plain load, because
 * what an initializer holds there is padding. The strip refuses one that
 * survives to it.
 */
const StatedField stated[stated_count] = {
	{ MONO_STRUCT_OFFSET (MonoVTable, klass), sizeof (MonoClass *), true },
	{ MONO_STRUCT_OFFSET (MonoVTable, type), sizeof (gpointer), true },
	{ MONO_STRUCT_OFFSET (MonoVTable, rank), sizeof (guint8), false },
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

/// Walks a snapshot's members in layout order. \p field takes each stated
/// field's index and \p gap the width of each run between them.
///
/// The type and the initializer are both built from this walk, which is what
/// keeps a field and its value in step.
template <typename Field, typename Gap>
void
walk_members (uint32_t slots, Field field, Gap gap)
{
	uint64_t at = 0;
	auto pad_to = [&] (uint64_t offset) {
		if (offset > at)
			gap (offset - at);
		at = offset;
	};

	for (unsigned i = 0; i < stated_count; ++i) {
		pad_to (stated[i].at);
		field (StatedIndex (i));
		at += stated[i].width;
	}

	pad_to (vtable_bytes (slots));
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

	/*
	 * `available_externally` says the body is here to be read and belongs to
	 * somebody else, which is what a snapshot is. Three things follow, and each
	 * is one the plainer linkages get wrong. hasDefinitiveInitializer () holds,
	 * so a load folds. Two modules can each carry the constant, which the
	 * inliner's link needs - two definitions of one name do not merge. And a
	 * definition left standing is dropped rather than emitted. A strip that
	 * missed one then costs the fold, instead of publishing bytes the runtime
	 * never wrote.
	 *
	 * Not dso_local. That says the symbol lands in this object, so codegen
	 * reaches it with a PC-relative fixup rather than asking the linker. JITLink
	 * then resolves the real vtable's name against the object itself.
	 */
	snapshot.setConstant (true);
	snapshot.setLinkage (GlobalValue::AvailableExternallyLinkage);
}

Type *
vtable_snapshot_type (Module &m, uint32_t slots)
{
	LLVMContext &c = m.getContext ();
	Type *byte = Type::getInt8Ty (c);
	SmallVector<Type *, 16> members;

	walk_members (
		slots,
		[&] (StatedIndex i) {
			members.push_back (stated[i].pointer
			                           ? static_cast<Type *> (PointerType::get (c, 0))
			                           : IntegerType::get (c, stated[i].width * 8));
		},
		[&] (uint64_t width) {
			members.push_back (ArrayType::get (byte, width));
		});

	// Packed, so each field sits where the offset above put it rather than
	// where LLVM's own alignment rules would.
	Type *built = StructType::get (c, members, /*isPacked=*/true);

	if (m.getDataLayout ().getTypeAllocSize (built) != vtable_bytes (slots))
		report_fatal_error ("the vtable snapshot layout is not MonoVTable's");

	return built;
}

Constant *
vtable_snapshot_init (Module &m, const VTableFacts &facts)
{
	auto *laid_out = cast<StructType> (vtable_snapshot_type (m, 0));
	LLVMContext &c = m.getContext ();
	SmallVector<Constant *, 8> members;

	walk_members (
		0,
		[&] (StatedIndex i) {
			switch (i) {
			case stated_klass:
				members.push_back (facts.klass);
				return;
			case stated_type:
				members.push_back (facts.type);
				return;
			case stated_rank:
				members.push_back (ConstantInt::get (
					IntegerType::get (c, stated[i].width * 8), facts.rank));
				return;
			case stated_count:
				break;
			}

			llvm_unreachable ("a stated field with no value");
		},
		// A withheld offset holds no fact, and zero is what reads most
		// clearly in a dump.
		[&] (uint64_t width) {
			members.push_back (Constant::getNullValue (
				ArrayType::get (Type::getInt8Ty (c), width)));
		});

	return ConstantStruct::get (laid_out, members);
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
		snapshot->setDSOLocal (false);
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

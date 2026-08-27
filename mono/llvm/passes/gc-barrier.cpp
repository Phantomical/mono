/**
 * \file
 * \brief Writing a write barrier back as the card path, or as the collector's
 * own helper where it keeps no card table.
 */

#include "gc-barrier.hpp"

#include "builtins.hpp"

#include "mono/metadata/abi-details.h"

#include <llvm/ADT/StringRef.h>
#include <llvm/IR/Attributes.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/InstrTypes.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/ErrorHandling.h>
#include <llvm/Support/ModRef.h>

#include <string>

using namespace llvm;

namespace mono {
namespace {

// The layout, one attribute for each field GcBarrierLayout holds. A number is
// written in decimal, and an address is the number its bytes make. A collector
// that marks no cards stamps none of them.
constexpr StringRef card_table_attr = "mono-gc-card-table";
constexpr StringRef card_mask_attr = "mono-gc-card-mask";
constexpr StringRef card_bits_attr = "mono-gc-card-bits";
constexpr StringRef nursery_attr = "mono-gc-nursery";
constexpr StringRef nursery_bits_attr = "mono-gc-nursery-bits";
constexpr StringRef value_decides_attr = "mono-gc-value-decides";
constexpr StringRef concurrent_flag_attr = "mono-gc-concurrent-flag";
constexpr StringRef concurrent_flag_size_attr = "mono-gc-concurrent-flag-size";

/// What the barrier does to memory: it reads what its arguments name, and it
/// reads and writes the collector's own tables, which the module reaches no
/// other way.
///
/// The write is what keeps the call. With the read alone, DCE erases every
/// barrier before a lowering sees one, and a compile is left with the store and
/// no card.
MemoryEffects
barrier_effects ()
{
	return MemoryEffects::argMemOnly (ModRefInfo::Ref)
	       | MemoryEffects::inaccessibleMemOnly (ModRefInfo::ModRef);
}

void
set_number (Function *decl, StringRef name, uint64_t value)
{
	decl->addFnAttr (name, std::to_string (value));
}

uint64_t
number (const Function &decl, StringRef name)
{
	uint64_t value = 0;

	if (decl.hasFnAttribute (name))
		decl.getFnAttribute (name).getValueAsString ().getAsInteger (10, value);

	return value;
}

GcBarrierLayout
layout_of (const Function &decl)
{
	GcBarrierLayout layout;

	layout.card_table = reinterpret_cast<void *> (number (decl, card_table_attr));

	if (layout.card_table == nullptr)
		return layout;

	layout.card_mask = uintptr_t (number (decl, card_mask_attr));
	layout.card_bits = int (number (decl, card_bits_attr));
	layout.nursery_start = reinterpret_cast<void *> (number (decl, nursery_attr));
	layout.nursery_bits = int (number (decl, nursery_bits_attr));
	layout.value_decides = decl.hasFnAttribute (value_decides_attr);
	layout.concurrent_flag =
		reinterpret_cast<void *> (number (decl, concurrent_flag_attr));
	layout.concurrent_flag_size = unsigned (number (decl, concurrent_flag_size_attr));
	return layout;
}

/// The global the card path reads an address through, made on first use.
Constant *
gc_symbol (Module &m, StringRef name)
{
	return m.getOrInsertGlobal (name, Type::getInt8Ty (m.getContext ()));
}

/// Rewrites one site into the test and the card store it stands for.
void
lower_card (CallInst *site, const GcBarrierLayout &gc)
{
	Function *f = site->getFunction ();
	Module &m = *f->getParent ();
	LLVMContext &c = f->getContext ();
	Value *address = site->getArgOperand (0);
	Value *value = site->getArgOperand (1);
	IntegerType *word = Type::getIntNTy (c, TARGET_SIZEOF_VOID_P * 8);
	BasicBlock *head = site->getParent ();
	BasicBlock *done = head->splitBasicBlock (std::next (site->getIterator ()), "wb_done");
	BasicBlock *card = BasicBlock::Create (c, "wb_mark", f);
	IRBuilder<> b (c);

	b.SetCurrentDebugLocation (site->getDebugLoc ());
	site->eraseFromParent ();

	// The split left a branch to done behind, and the tests below end the
	// block instead.
	head->getTerminator ()->eraseFromParent ();
	b.SetInsertPoint (head);

	Constant *nursery = ConstantInt::get (
		word, reinterpret_cast<uintptr_t> (gc.nursery_start) >> gc.nursery_bits);
	Value *target = b.CreatePtrToInt (address, word);
	Value *target_is_old = b.CreateICmpNE (
		b.CreateLShr (target, gc.nursery_bits), nursery, "wb_target_is_old");

	/*
	 * The collector reads the card under an old destination that names a young
	 * object. While a concurrent collection runs, it reads the card under every
	 * old destination:
	 *
	 *     mark = target_is_old && (value_is_young || concurrent_collection)
	 *
	 * The collector's own wrapper reads the destination back for the value
	 * test, because its caller made the store. We test the value that was
	 * stored instead. A thread that puts a different reference there marks the
	 * card with its own barrier, so the card is marked either way.
	 */
	auto value_is_young = [&] {
		Value *stored = b.CreateLShr (b.CreatePtrToInt (value, word), gc.nursery_bits);

		return b.CreateICmpEQ (stored, nursery, "wb_value_is_young");
	};

	if (gc.value_decides) {
		// A collector that collects nothing concurrently drops the last term.
		// The two tests that are left are shifts and compares on values already
		// in registers, so one branch carries them both.
		b.CreateCondBr (b.CreateAnd (target_is_old, value_is_young ()), card, done);
	} else if (gc.concurrent_flag != nullptr) {
		Type *flag_type = b.getIntNTy (gc.concurrent_flag_size * 8);
		BasicBlock *old_target = BasicBlock::Create (c, "wb_target_old", f);

		b.CreateCondBr (target_is_old, old_target, done);
		b.SetInsertPoint (old_target);

		/*
		 * The load is volatile, so no pass takes it out of the loop the store
		 * sits in, or moves it in front of that store. The collector sets the
		 * flag with the world stopped, so a load in front of the store can read
		 * false while the collection starts. The store then lands with no card
		 * in an object the marker already read.
		 */
		Value *concurrent = b.CreateICmpNE (
			b.CreateAlignedLoad (flag_type, gc_symbol (m, gc_concurrent_flag_symbol),
		                             Align (gc.concurrent_flag_size), true,
		                             "wb_concurrent"),
			ConstantInt::get (flag_type, 0));

		b.CreateCondBr (b.CreateOr (value_is_young (), concurrent), card, done);
	} else {
		// A collector that keeps no flag can run a concurrent collection at any
		// moment, so every old destination gets a card.
		b.CreateCondBr (target_is_old, card, done);
	}

	b.SetInsertPoint (card);

	Value *index = b.CreateLShr (target, gc.card_bits);

	// A zero mask is a table that covers the address space, so the index needs
	// none.
	if (gc.card_mask != 0)
		index = b.CreateAnd (index, ConstantInt::get (word, gc.card_mask));

	b.CreateAlignedStore (
		b.getInt8 (1),
		b.CreateGEP (b.getInt8Ty (), gc_symbol (m, gc_card_table_symbol), index),
		Align (1));
	b.CreateBr (done);
}

/// Rewrites one site into the call the collector marks through.
void
lower_helper (CallInst *site)
{
	Module &m = *site->getModule ();
	LLVMContext &c = m.getContext ();
	FunctionCallee helper = m.getOrInsertFunction (
		gc_barrier_helper_name, Type::getVoidTy (c), PointerType::get (c, 0));

	// The helper marks the same tables the card path writes and raises nothing,
	// so it keeps what the site claimed.
	if (auto *decl = dyn_cast<Function> (helper.getCallee ())) {
		decl->setDoesNotThrow ();
		decl->setMemoryEffects (barrier_effects ());
	}

	IRBuilder<> b (site);

	b.SetCurrentDebugLocation (site->getDebugLoc ());
	b.CreateCall (helper, { site->getArgOperand (0) });
	site->eraseFromParent ();
}

} // namespace

Function *
gc_barrier_decl (Module &m, const GcBarrierLayout &layout)
{
	LLVMContext &c = m.getContext ();
	Type *ptr = PointerType::get (c, 0);
	Function *decl =
		builtin_decl (m, gc_barrier_name,
	                      FunctionType::get (Type::getVoidTy (c), { ptr, ptr }, false));

	decl->setDoesNotThrow ();
	decl->setMemoryEffects (barrier_effects ());
	decl->addFnAttr (Attribute::WillReturn);
	decl->addFnAttr (Attribute::NoCallback);

	/*
	 * A barrier marks a table from the address it is given and keeps neither
	 * pointer. `lower_card ()` writes the card byte the destination indexes, and
	 * `lower_helper ()` calls the collector's own
	 * `mono_gc_wbarrier_generic_nostore_internal ()`, which is
	 * `sgen_card_table_mark_address ()` under SGen and `GC_dirty ()` under Boehm.
	 * A remembered set that instead recorded the address would capture it, and
	 * `sgen-cardtable.c` holds the one assignment to
	 * `remset.wbarrier_generic_nostore`.
	 *
	 * Capture is tracked apart from the memory effects above, so a barrier with
	 * no such attribute hands the object its destination points into to an
	 * opaque call. Every call below that then may-writes the object's fields.
	 * A field a caller stored before a loop is then re-read inside it, and the
	 * class an allocation stated does not reach the dispatch that wants it.
	 */
	decl->addParamAttr (0, Attribute::getWithCaptureInfo (c, CaptureInfo::none ()));
	decl->addParamAttr (1, Attribute::getWithCaptureInfo (c, CaptureInfo::none ()));

	if (layout.card_table == nullptr)
		return decl;

	set_number (decl, card_table_attr, reinterpret_cast<uintptr_t> (layout.card_table));
	set_number (decl, card_mask_attr, layout.card_mask);
	set_number (decl, card_bits_attr, uint64_t (layout.card_bits));
	set_number (decl, nursery_attr, reinterpret_cast<uintptr_t> (layout.nursery_start));
	set_number (decl, nursery_bits_attr, uint64_t (layout.nursery_bits));

	if (layout.value_decides)
		decl->addFnAttr (value_decides_attr);

	if (layout.concurrent_flag != nullptr) {
		set_number (decl, concurrent_flag_attr,
		            reinterpret_cast<uintptr_t> (layout.concurrent_flag));
		set_number (decl, concurrent_flag_size_attr, layout.concurrent_flag_size);
	}

	return decl;
}

bool
lower_gc_barriers (Module &m)
{
	Function *decl = m.getFunction (gc_barrier_name);

	if (decl == nullptr)
		return false;

	GcBarrierLayout layout = layout_of (*decl);

	for (CallBase *site : builtin_sites (m, gc_barrier_name)) {
		auto *call = dyn_cast<CallInst> (site);

		// The declaration is nounwind, so no site is on an unwind edge and
		// there is no handler to keep.
		if (call == nullptr)
			report_fatal_error ("a write barrier site is an invoke");

		if (layout.card_table != nullptr)
			lower_card (call, layout);
		else
			lower_helper (call);
	}

	return erase_builtin (m, gc_barrier_name);
}

} // namespace mono

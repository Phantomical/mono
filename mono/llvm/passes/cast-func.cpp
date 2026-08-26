/**
 * \file
 * \brief Writing a type test back as the probe the runtime reads.
 */

#include "cast-func.hpp"

#include "method-symbols.hpp"

#include "mono/metadata/abi-details.h"
#include "mono/metadata/class-init.h"
#include "mono/metadata/class-inlines.h"
#include "mono/metadata/class-internals.h"
#include "mono/metadata/metadata.h"
#include "mono/metadata/object-internals.h"

#include <llvm/ADT/SmallVector.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/InstrTypes.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/ErrorHandling.h>

using namespace llvm;

namespace mono {
namespace {

/**
 * Whether `mono_class_has_parent ()` is the whole answer for a cast to klass.
 *
 * `mono_class_is_assignable_from_general ()` (`mono/metadata/class.c`) ends at
 * that call, and each branch above it either agrees or governs a shape refused
 * here. An interface reads the interface bitmap. An array and a delegate have
 * variance, and a pointer, a nullable and a generic argument each have a rule
 * of their own.
 *
 * A marshal-by-ref target never reaches that function at all. The runtime
 * answers it with `mono_object_handle_isinst_mbyref ()` instead, so this
 * refuses it too.
 *
 * A context-dependent class does not reach here. The test compares against the
 * class and against its depth as constants, and the operand carries neither
 * where an rgctx fetch answered for the class.
 */
bool
subtype_test_applies (MonoClass *klass)
{
	MonoType *self = m_class_get_byval_arg (klass);

	if (mono_class_is_interface (klass) || m_class_get_marshalbyref (klass)
	    || m_class_get_rank (klass) != 0 || m_class_is_valuetype (klass)
	    || mono_class_is_nullable (klass) || m_class_is_delegate (klass)
	    || m_class_get_class_kind (klass) == MONO_CLASS_POINTER
	    || self->type == MONO_TYPE_VAR || self->type == MONO_TYPE_MVAR)
		return false;

	// The depth reached below indexes the object's supertypes, and the
	// runtime writes this one once and never changes it.
	mono_class_setup_supertypes (klass);

	return m_class_get_idepth (klass) > 0;
}

/**
 * Whether the interface bitmap is the whole answer for a cast to klass.
 *
 * `mono_object_handle_isinst_mbyref_raw ()` (`mono/metadata/object.c`) reads
 * the same bitmap off the same vtable first, and the two branches below that
 * read only add a yes: an array special interface, and a variant generic
 * interface. A transparent proxy takes the bitmap first as well, and
 * `mono_upgrade_remote_class ()` is what puts the bit there.
 *
 * A context-dependent class does not reach here, for the reason the subtype
 * test gives: the interface id is a constant the operand does not carry.
 */
bool
interface_test_applies (MonoClass *klass)
{
#ifdef COMPRESSED_INTERFACE_BITMAP
	// A compressed bitmap holds runs of empty bytes rather than the bytes
	// themselves, so the constant index emit_interface_test () computes
	// reaches the wrong byte. mono_class_interface_match () walks it instead.
	return false;
#else
	// The id indexes the bitmap, and the runtime assigns one on demand.
	// Zero is never assigned, so an id of zero says the class has none yet.
	mono_class_setup_interface_id (klass);

	return m_class_get_interface_id (klass) != 0;
#endif
}

/// The class the test names, or null where an rgctx fetch answered for it.
MonoClass *
tested_class (const CallBase *site)
{
	auto *global = dyn_cast<GlobalValue> (site->getArgOperand (1));

	return global != nullptr ? marked_class (*global) : nullptr;
}

Value *
load_vtable (IRBuilder<> &b, Value *object, const Twine &name = "")
{
	return b.CreateAlignedLoad (PointerType::get (b.getContext (), 0), object,
	                            Align (TARGET_SIZEOF_VOID_P), name);
}

/// Emits the inline half of a cast: branches to yes when the object's class has
/// klass among its supertypes, and to otherwise when this cannot tell.
///
/// Leaves yes and otherwise unterminated. The caller fills in both.
void
emit_subtype_test (IRBuilder<> &b, Function *f, MonoClass *klass, Value *obj, Value *target,
                   BasicBlock *yes, BasicBlock *otherwise)
{
	LLVMContext &c = b.getContext ();
	Type *ptr = PointerType::get (c, 0);
	uint16_t depth = m_class_get_idepth (klass);

	Value *vtable = load_vtable (b, obj);
	Value *its_class = b.CreateAlignedLoad (
		ptr,
		b.CreateGEP (b.getInt8Ty (), vtable,
	                     b.getInt32 (MONO_STRUCT_OFFSET (MonoVTable, klass))),
		Align (TARGET_SIZEOF_VOID_P), "obj_class");

	// The supertypes array holds one entry for each level down to the class
	// itself, so a class shallower than klass cannot hold it and indexing at
	// klass's depth would read past the end.
	Value *its_depth = b.CreateAlignedLoad (
		b.getInt16Ty (),
		b.CreateGEP (b.getInt8Ty (), its_class,
	                     b.getInt32 (MONO_STRUCT_OFFSET (MonoClass, idepth))),
		Align (2), "obj_idepth");

	BasicBlock *deep_enough = BasicBlock::Create (c, "cast_deep_enough", f);

	b.CreateCondBr (b.CreateICmpUGE (its_depth, b.getInt16 (depth)), deep_enough, otherwise);
	b.SetInsertPoint (deep_enough);

	Value *supertypes = b.CreateAlignedLoad (
		ptr,
		b.CreateGEP (b.getInt8Ty (), its_class,
	                     b.getInt32 (MONO_STRUCT_OFFSET (MonoClass, supertypes))),
		Align (TARGET_SIZEOF_VOID_P), "supertypes");
	Value *at_depth =
		b.CreateAlignedLoad (ptr, b.CreateGEP (ptr, supertypes, b.getInt32 (depth - 1)),
	                             Align (TARGET_SIZEOF_VOID_P), "supertype");

	b.CreateCondBr (b.CreateICmpEQ (at_depth, target), yes, otherwise);
}

/// Emits the inline half of a cast to an interface: branches to yes when the
/// object's vtable has the interface among the ones it implements, and to
/// otherwise when this cannot tell.
///
/// Leaves yes and otherwise unterminated. The caller fills in both.
///
/// This is `MONO_VTABLE_IMPLEMENTS_INTERFACE ()` (`class-internals.h`) as IR.
/// The vtable carries the bitmap and the bound both, so the object's vtable is
/// the only load in front of the test, and the target needs no operand.
void
emit_interface_test (IRBuilder<> &b, Function *f, MonoClass *klass, Value *obj,
                     BasicBlock *yes, BasicBlock *otherwise)
{
	LLVMContext &c = b.getContext ();
	Type *ptr = PointerType::get (c, 0);
	uint32_t iid = m_class_get_interface_id (klass);

	Value *vtable = load_vtable (b, obj, "obj_vtable");

	// The bitmap holds one bit for each id up to the bound, so a bound below
	// the target's id means the byte the test wants is past the end.
	Value *bound = b.CreateAlignedLoad (
		b.getInt32Ty (),
		b.CreateGEP (b.getInt8Ty (), vtable,
	                     b.getInt32 (MONO_STRUCT_OFFSET (MonoVTable, max_interface_id))),
		Align (4), "max_interface_id");

	BasicBlock *in_range = BasicBlock::Create (c, "cast_iface_in_range", f);

	b.CreateCondBr (b.CreateICmpUGE (bound, b.getInt32 (iid)), in_range, otherwise);
	b.SetInsertPoint (in_range);

	Value *bitmap = b.CreateAlignedLoad (
		ptr,
		b.CreateGEP (b.getInt8Ty (), vtable,
	                     b.getInt32 (MONO_STRUCT_OFFSET (MonoVTable, interface_bitmap))),
		Align (TARGET_SIZEOF_VOID_P), "interface_bitmap");
	Value *byte = b.CreateAlignedLoad (
		b.getInt8Ty (), b.CreateGEP (b.getInt8Ty (), bitmap, b.getInt32 (iid >> 3)),
		Align (1), "interface_byte");
	Value *bit = b.CreateAnd (byte, b.getInt8 (1 << (iid & 7)));

	b.CreateCondBr (b.CreateIsNotNull (bit), yes, otherwise);
}

/// \p args, reshaped to what \p callee declares.
///
/// The wrapper takes the class and the cache word as integers, so a pointer
/// reaching an integer parameter is converted rather than passed.
SmallVector<Value *, 3>
adapt_to_callee (IRBuilder<> &b, Function *callee, ArrayRef<Value *> args)
{
	SmallVector<Value *, 3> adapted (args.begin (), args.end ());
	FunctionType *type = callee->getFunctionType ();

	for (unsigned i = 0; i < adapted.size () && i < type->getNumParams (); ++i) {
		Type *want = type->getParamType (i);
		Value *have = adapted[i];

		if (have->getType () == want)
			continue;
		if (want->isPointerTy ())
			adapted[i] = b.CreateIntToPtr (have, want);
		else if (want->isIntegerTy ())
			adapted[i] = b.CreatePtrToInt (have, want);
	}

	return adapted;
}

/**
 * Rewrites one site into the code it stands for.
 *
 * The site is the block's terminator where a clause protects it. The wrapper
 * then inherits that unwind edge, so the block the wrapper ends up in takes the
 * site's place among the pad's predecessors.
 */
void
lower (CallBase *site, bool throw_on_fail)
{
	Function *f = site->getFunction ();
	LLVMContext &c = site->getContext ();
	BasicBlock *head = site->getParent ();
	Type *ptr = PointerType::get (c, 0);
	Type *word = Type::getIntNTy (c, TARGET_SIZEOF_VOID_P * 8);
	Constant *null = ConstantPointerNull::get (cast<PointerType> (ptr));

	Value *obj = site->getArgOperand (0);
	Value *target = site->getArgOperand (1);
	Value *cache = site->getArgOperand (2);
	auto *icall = cast<Function> (site->getArgOperand (3)->stripPointerCasts ());

	BasicBlock *tail;
	BasicBlock *pad = nullptr;

	if (auto *invoke = dyn_cast<InvokeInst> (site)) {
		tail = invoke->getNormalDest ();
		pad = invoke->getUnwindDest ();
	} else {
		// The rest of the block becomes the block the answer flows into, and
		// head keeps a branch to it until the null check replaces that branch.
		tail = head->splitBasicBlock (site->getIterator (), "cast_tail");
	}

	BasicBlock *probe = BasicBlock::Create (c, "cast_probe", f);
	BasicBlock *hit = BasicBlock::Create (c, "cast_hit", f);
	BasicBlock *miss = BasicBlock::Create (c, "cast_miss", f);
	BasicBlock *done = BasicBlock::Create (c, "cast_done", f);

	MonoClass *klass = tested_class (site);
	bool to_interface = klass != nullptr && mono_class_is_interface (klass);
	BasicBlock *told_yes = nullptr;
	BasicBlock *first = probe;

	IRBuilder<> b (c);

	b.SetCurrentDebugLocation (site->getDebugLoc ());

	/*
	 * In front of the cache, the test the runtime itself ends at. For a target
	 * class that is not an interface and not marshal-by-ref,
	 * mono_class_is_assignable_from_general () answers
	 * `mono_class_has_parent (object's class, target)`. For an interface,
	 * mono_object_handle_isinst_mbyref_raw () answers the bitmap on the
	 * object's vtable. Either one is a bounds check and one comparison, and
	 * either answers for every class rather than for the last one, so neither
	 * needs a slot and neither misses.
	 *
	 * Both are one-sided. Where one says yes the runtime says yes, and where
	 * one says no the answer can still be yes through a path they do not
	 * model, so a no falls through to the cache and then to the wrapper.
	 */
	if (klass != nullptr
	    && (to_interface ? interface_test_applies (klass) : subtype_test_applies (klass))) {
		told_yes = BasicBlock::Create (c, "cast_inline_yes", f);
		first = BasicBlock::Create (c, to_interface ? "cast_interface" : "cast_subtype", f);

		b.SetInsertPoint (first);

		if (to_interface)
			emit_interface_test (b, f, klass, obj, told_yes, probe);
		else
			emit_subtype_test (b, f, klass, obj, target, told_yes, probe);

		b.SetInsertPoint (told_yes);
		b.CreateBr (done);
	}

	b.SetInsertPoint (probe);

	Value *cached = b.CreateAlignedLoad (ptr, cache, Align (TARGET_SIZEOF_VOID_P),
	                                     "cached_vtable");
	Value *vtable = load_vtable (b, obj, "obj_vtable");

	// The word holds the vtable that last answered here, with bit 0 set when that
	// answer was no. Only isinst caches a no. castclass throws instead, so its
	// word is the pointer on its own.
	Value *cached_word = b.CreatePtrToInt (cached, word);
	Value *cached_vtable =
		throw_on_fail ? cached_word
		              : b.CreateAnd (cached_word, ConstantInt::get (word, ~(uint64_t) 1));

	b.CreateCondBr (b.CreateICmpEQ (cached_vtable, b.CreatePtrToInt (vtable, word)), hit,
	                miss);

	b.SetInsertPoint (hit);

	Value *answer = obj;

	if (!throw_on_fail) {
		Value *answered_no = b.CreateTrunc (cached_word, b.getInt1Ty (), "answered_no");

		answer = b.CreateSelect (answered_no, null, obj);
	}

	b.CreateBr (done);
	b.SetInsertPoint (miss);

	// Castclass reports a failed cast as a pending InvalidCastException. Only
	// the wrapper's check after the call turns that into a throw.
	SmallVector<Value *, 3> args = adapt_to_callee (b, icall, { obj, target, cache });
	CallBase *slow;

	if (pad != nullptr) {
		slow = b.CreateInvoke (icall, done, pad, args);
	} else {
		CallInst *plain = b.CreateCall (icall, args);

		// A managed frame is observable, so a call in tail position stays a
		// call. emit_protected_call () marks the sites it writes the same way.
		plain->setTailCallKind (CallInst::TCK_NoTail);
		slow = plain;
		b.CreateBr (done);
	}

	b.SetInsertPoint (done);

	PHINode *result = b.CreatePHI (ptr, 4, "cast_result");

	// Before the incoming values name it, so that replacing the site's uses
	// does not reach into the phi's own operand for the wrapper's answer.
	site->replaceAllUsesWith (result);

	// Both forms answer null for a null reference, and neither one reads the
	// vtable for it. The tests above do, so this one comes first.
	result->addIncoming (null, head);
	result->addIncoming (answer, hit);
	result->addIncoming (slow, miss);

	if (told_yes != nullptr)
		result->addIncoming (obj, told_yes);

	b.CreateBr (tail);

	// The phis of the two blocks the site reached name it as their predecessor.
	// Renaming keeps the values they take, which is what the new edges carry as
	// well, and leaves nothing naming head once the site goes.
	tail->replacePhiUsesWith (head, done);

	if (pad != nullptr)
		pad->replacePhiUsesWith (head, miss);

	site->eraseFromParent ();

	// head is left without a terminator either way: an invoke was the
	// terminator, and a call left the branch the split wrote.
	if (Instruction *stale = head->getTerminator ())
		stale->eraseFromParent ();

	b.SetInsertPoint (head);
	b.CreateCondBr (b.CreateIsNull (obj), done, first);
}

/// Lowers every call to the declaration \p name holds in \p m, and erases it.
bool
lower_all (Module &m, StringRef name, bool throw_on_fail)
{
	Function *decl = m.getFunction (name);

	if (decl == nullptr)
		return false;

	SmallVector<CallBase *, 8> sites;

	for (User *user : decl->users ())
		if (auto *site = dyn_cast<CallBase> (user))
			sites.push_back (site);

	for (CallBase *site : sites)
		lower (site, throw_on_fail);

	// Anything left is a use this lowering does not understand.
	if (!decl->use_empty ())
		report_fatal_error (Twine ("unlowered use of ") + name);
	decl->eraseFromParent ();

	return true;
}

} // namespace

Function *
cast_func_decl (Module &m, bool throw_on_fail)
{
	StringRef name = throw_on_fail ? cast_castclass_name : cast_isinst_name;

	if (Function *existing = m.getFunction (name))
		return existing;

	LLVMContext &c = m.getContext ();
	Type *ptr = PointerType::get (c, 0);

	return Function::Create (FunctionType::get (ptr, { ptr, ptr, ptr, ptr }, false),
	                         GlobalValue::ExternalLinkage, name, m);
}

PreservedAnalyses
LowerCastFuncPass::run (Module &m, ModuleAnalysisManager &)
{
	bool changed = lower_all (m, cast_isinst_name, false);

	changed |= lower_all (m, cast_castclass_name, true);

	return changed ? PreservedAnalyses::none () : PreservedAnalyses::all ();
}

} // namespace mono

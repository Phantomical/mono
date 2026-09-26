/**
 * \file
 * \brief Writing a type test back as the probe the runtime reads.
 */

#include "cast-func.hpp"

#include "builtins.hpp"
#include "method-symbols.hpp"

#include "mono/llvm/internal-loads.hpp"
#include "mono/llvm/managed-pointer.hpp"

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

/// The supertype-chain depth to test a cast to klass at. A value-type klass
/// may use it here: a value type can never be a transparent proxy, so the
/// failure arm below skips the proxy check for one.
uint16_t
subtype_test_depth (MonoClass *klass)
{
	return mono_class_get_supertype_test_depth (klass, /* allow_valuetype */ TRUE);
}

namespace {

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

/**
 * Whether a failed interface bitmap test is conclusive for klass.
 *
 * Variant and array-special interfaces can match through covariance even when
 * their exact interface id is absent from the bitmap.
 */
bool
interface_test_is_conclusive (MonoClass *klass)
{
	return !mono_class_has_variant_generic_params (klass) && !m_class_is_array_special_interface (klass);
}

/// The class the test names, or null where an rgctx fetch answered for it.
MonoClass *
tested_class (const CallBase *site)
{
	auto *global = dyn_cast<GlobalValue> (site->getArgOperand (1));

	return global != nullptr ? get_class (*global) : nullptr;
}

/// The leaf alone, and not the invariance `mark_object_vtable_read ()` adds.
/// The remote arm below calls `mono_object_isinst_remote ()`, which is what
/// reaches `mono_upgrade_remote_class ()`, and that builds a fresh vtable and
/// points the proxy at it. This is the one read that can see the word change.
Value *
load_vtable (IRBuilder<> &b, Value *object, const Twine &name = "")
{
	return mark_internal_load (
		b.CreateAlignedLoad (PointerType::get (b.getContext (), 0), object,
	                             Align (TARGET_SIZEOF_VOID_P), name),
		object_header_tbaa_leaf, InternalLife::varies);
}

/// Emits the inline half of a cast: branches to yes when the object's class has
/// the class tested at depth among its supertypes, and to otherwise when it cannot.
/// The caller supplies the depth, as an i16, because the class may come from an
/// rgctx fetch - and so, at a site sharing a generic body, may the depth itself.
///
/// Leaves yes and otherwise unterminated. The caller fills in both.
void
emit_subtype_test (IRBuilder<> &b, Function *f, Value *depth, Value *obj, Value *target,
                   BasicBlock *yes, BasicBlock *otherwise)
{
	LLVMContext &c = b.getContext ();
	Type *ptr = PointerType::get (c, 0);

	Value *vtable = load_vtable (b, obj);
	Value *its_class = mark_internal_load (
		b.CreateAlignedLoad (ptr,
	                             b.CreateGEP (b.getInt8Ty (), vtable,
	                                          b.getInt32 (MONO_STRUCT_OFFSET (MonoVTable, klass))),
	                             Align (TARGET_SIZEOF_VOID_P), "obj_class"),
		vtable_tbaa_leaf, InternalLife::fixed);

	// The supertypes array holds one entry for each level down to the class
	// itself, so a class shallower than klass cannot hold it and indexing at
	// klass's depth would read past the end. A class `mono_class_setup_supertypes
	// ()` has not reached reads a depth of zero and fails the same test.
	Value *its_depth = mark_internal_load (
		b.CreateAlignedLoad (b.getInt16Ty (),
	                             b.CreateGEP (b.getInt8Ty (), its_class,
	                                          b.getInt32 (MONO_STRUCT_OFFSET (MonoClass, idepth))),
	                             Align (2), "obj_idepth"),
		class_tbaa_leaf, InternalLife::varies);

	BasicBlock *deep_enough = BasicBlock::Create (c, "cast_deep_enough", f);

	b.CreateCondBr (b.CreateICmpUGE (its_depth, depth), deep_enough, otherwise);
	b.SetInsertPoint (deep_enough);

	Value *supertypes = mark_internal_load (
		b.CreateAlignedLoad (ptr,
	                             b.CreateGEP (b.getInt8Ty (), its_class,
	                                          b.getInt32 (MONO_STRUCT_OFFSET (MonoClass, supertypes))),
	                             Align (TARGET_SIZEOF_VOID_P), "supertypes"),
		class_tbaa_leaf, InternalLife::varies);
	Value *index =
		b.CreateZExt (b.CreateSub (depth, b.getInt16 (1)), b.getInt32Ty (), "supertype_index");
	Value *at_depth = mark_internal_load (
		b.CreateAlignedLoad (ptr, b.CreateGEP (ptr, supertypes, index),
	                             Align (TARGET_SIZEOF_VOID_P), "supertype"),
		class_tbaa_leaf, InternalLife::fixed);

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
	Value *bound = mark_internal_load (
		b.CreateAlignedLoad (
			b.getInt32Ty (),
			b.CreateGEP (b.getInt8Ty (), vtable,
	                             b.getInt32 (MONO_STRUCT_OFFSET (MonoVTable, max_interface_id))),
			Align (4), "max_interface_id"),
		vtable_tbaa_leaf, InternalLife::fixed);

	BasicBlock *in_range = BasicBlock::Create (c, "cast_iface_in_range", f);

	b.CreateCondBr (b.CreateICmpUGE (bound, b.getInt32 (iid)), in_range, otherwise);
	b.SetInsertPoint (in_range);

	Value *bitmap = mark_internal_load (
		b.CreateAlignedLoad (
			ptr,
			b.CreateGEP (b.getInt8Ty (), vtable,
	                             b.getInt32 (MONO_STRUCT_OFFSET (MonoVTable, interface_bitmap))),
			Align (TARGET_SIZEOF_VOID_P), "interface_bitmap"),
		vtable_tbaa_leaf, InternalLife::fixed);
	Value *byte = mark_internal_load (
		b.CreateAlignedLoad (b.getInt8Ty (),
	                             b.CreateGEP (b.getInt8Ty (), bitmap, b.getInt32 (iid >> 3)),
	                             Align (1), "interface_byte"),
		vtable_tbaa_leaf, InternalLife::fixed);
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
		if (want->isPointerTy () && have->getType ()->isPointerTy ())
			adapted[i] = in_address_space (b, have, want->getPointerAddressSpace ());
		else if (want->isPointerTy ())
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
	Constant *null = ConstantPointerNull::get (cast<PointerType> (site->getType ()));

	Value *obj = site->getArgOperand (0);
	Value *target = site->getArgOperand (1);
	Value *cache = site->getArgOperand (2);
	auto *icall = cast<Function> (site->getArgOperand (3)->stripPointerCasts ());
	auto *remote_icall = cast<Function> (site->getArgOperand (4)->stripPointerCasts ());
	Value *proxy_class = site->getArgOperand (5);
	uint16_t subtype_depth =
		(uint16_t) cast<ConstantInt> (site->getArgOperand (6))->getZExtValue ();
	Value *rgctx_depth = site->getArgOperand (7);

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

	BasicBlock *done = BasicBlock::Create (c, "cast_done", f);

	MonoClass *klass = tested_class (site);
	bool to_interface = klass != nullptr && mono_class_is_interface (klass);
	bool via_static_subtype_chain = subtype_depth != 0;

	// rgctx_depth is the front end's ConstantInt 0 wherever a shared body cannot
	// resolve the target through a bare type parameter, so a real Value here is
	// what tells the two rgctx-resolved shapes apart: a declared class (already
	// covered by subtype_depth above, since its depth does not depend on the
	// binding) from a bare one, whose depth does.
	bool via_dynamic_subtype_chain = !via_static_subtype_chain && !isa<ConstantInt> (rgctx_depth);
	bool via_subtype_chain = via_static_subtype_chain || via_dynamic_subtype_chain;
	bool via_interface_bitmap = klass != nullptr && to_interface && interface_test_applies (klass)
	                             && interface_test_is_conclusive (klass);
	bool via_conclusive_test = via_subtype_chain || via_interface_bitmap;

	// An rgctx-only class takes the general proxy-checked path.
	bool to_valuetype = via_subtype_chain && klass != nullptr && m_class_is_valuetype (klass);

	BasicBlock *told_yes = nullptr;
	BasicBlock *first;
	BasicBlock *probe = nullptr, *hit = nullptr, *miss = nullptr;
	BasicBlock *remote = nullptr;
	Value *answer = nullptr;
	CallBase *slow = nullptr;
	SmallVector<BasicBlock *, 2> new_pad_preds;

	IRBuilder<> b (c);

	b.SetCurrentDebugLocation (site->getDebugLoc ());

	// Shared by the general path below and by the dynamic subtype chain's own
	// fallback, reached when depth turns out to be zero at run time.
	auto emit_probe = [&] () {
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

		answer = obj;

		if (!throw_on_fail) {
			Value *answered_no = b.CreateTrunc (cached_word, b.getInt1Ty (), "answered_no");

			answer = b.CreateSelect (answered_no, null, obj);
		}

		b.CreateBr (done);
		b.SetInsertPoint (miss);

		// Castclass reports a failed cast as a pending InvalidCastException. Only
		// the wrapper's check after the call turns that into a throw.
		SmallVector<Value *, 3> args = adapt_to_callee (b, icall, { obj, target, cache });

		if (pad != nullptr) {
			slow = b.CreateInvoke (icall, done, pad, args);
			new_pad_preds.push_back (miss);
		} else {
			CallInst *plain = b.CreateCall (icall, args);

			// A managed frame is observable, so a call in tail position stays a
			// call. emit_protected_call () marks the sites it writes the same way.
			plain->setTailCallKind (CallInst::TCK_NoTail);
			slow = plain;
			b.CreateBr (done);
		}
	};

	/*
	 * The inline subtype and conclusive interface tests produce the same answer
	 * as the runtime helper, so they can bypass the per-site cache.
	 *
	 * A failed isinst only needs the uncached helper for a transparent proxy.
	 * castclass always needs it to report InvalidCastException.
	 *
	 * The dynamic subtype chain is the same test with the depth resolved at run
	 * time, and it can resolve to zero - klass, once known, turns out to be a
	 * shape the chain does not decide - so that arm also builds the general,
	 * cached path as its own fallback.
	 */
	if (via_conclusive_test) {
		told_yes = BasicBlock::Create (c, "cast_inline_yes", f);
		first = BasicBlock::Create (
			c, via_dynamic_subtype_chain ? "cast_subtype_dynamic"
			                             : via_static_subtype_chain ? "cast_subtype" : "cast_interface",
			f);
		remote = BasicBlock::Create (c, "cast_remote", f);

		if (via_dynamic_subtype_chain) {
			probe = BasicBlock::Create (c, "cast_probe", f);
			hit = BasicBlock::Create (c, "cast_hit", f);
			miss = BasicBlock::Create (c, "cast_miss", f);
		}

		b.SetInsertPoint (first);

		if (via_dynamic_subtype_chain) {
			BasicBlock *has_depth = BasicBlock::Create (c, "cast_subtype_dynamic_has_depth", f);

			b.CreateCondBr (b.CreateICmpNE (rgctx_depth, b.getInt16 (0)), has_depth, probe);
			b.SetInsertPoint (has_depth);
			emit_subtype_test (b, f, rgctx_depth, obj, target, told_yes, remote);
		} else if (via_static_subtype_chain) {
			emit_subtype_test (b, f, b.getInt16 (subtype_depth), obj, target, told_yes, remote);
		} else {
			emit_interface_test (b, f, klass, obj, told_yes, remote);
		}

		b.SetInsertPoint (told_yes);
		b.CreateBr (done);

		if (via_dynamic_subtype_chain)
			emit_probe ();
	} else {
		probe = BasicBlock::Create (c, "cast_probe", f);
		hit = BasicBlock::Create (c, "cast_hit", f);
		miss = BasicBlock::Create (c, "cast_miss", f);
		first = probe;

		if (klass != nullptr && to_interface && interface_test_applies (klass)) {
			told_yes = BasicBlock::Create (c, "cast_inline_yes", f);
			first = BasicBlock::Create (c, "cast_interface", f);

			b.SetInsertPoint (first);
			emit_interface_test (b, f, klass, obj, told_yes, probe);

			b.SetInsertPoint (told_yes);
			b.CreateBr (done);
		}

		emit_probe ();
	}

	b.SetInsertPoint (done);

	PHINode *result = b.CreatePHI (site->getType (), 4, "cast_result");

	// Before the incoming values name it, so that replacing the site's uses
	// does not reach into the phi's own operand for the wrapper's answer.
	site->replaceAllUsesWith (result);

	// Both forms answer null for a null reference, and neither one reads the
	// vtable for it. The tests above do, so this one comes first.
	result->addIncoming (null, head);

	if (told_yes != nullptr)
		result->addIncoming (obj, told_yes);

	if (via_conclusive_test) {
		b.SetInsertPoint (remote);

		if (throw_on_fail) {
			SmallVector<Value *, 2> remote_args = adapt_to_callee (b, remote_icall, { obj, target });

			// Every failed castclass test must call the helper to report an
			// InvalidCastException, so checking for a proxy first saves nothing.
			CallBase *call;

			if (pad != nullptr) {
				call = b.CreateInvoke (remote_icall, done, pad, remote_args);
				new_pad_preds.push_back (remote);
			} else {
				CallInst *plain = b.CreateCall (remote_icall, remote_args);

				plain->setTailCallKind (CallInst::TCK_NoTail);
				call = plain;
				b.CreateBr (done);
			}

			result->addIncoming (call, remote);
		} else if (to_valuetype) {
			// A transparent proxy cannot be assignable to a value type, so the
			// failed subtype test is conclusive.
			b.CreateBr (done);
			result->addIncoming (null, remote);
		} else {
			Value *its_class = mark_internal_load (
				b.CreateAlignedLoad (
					ptr,
					b.CreateGEP (b.getInt8Ty (), load_vtable (b, obj),
				                     b.getInt32 (MONO_STRUCT_OFFSET (MonoVTable, klass))),
					Align (TARGET_SIZEOF_VOID_P), "remote_class"),
				vtable_tbaa_leaf, InternalLife::fixed);

			BasicBlock *ask = BasicBlock::Create (c, "cast_remote_ask", f);
			BasicBlock *not_remote = BasicBlock::Create (c, "cast_not_remote", f);

			b.CreateCondBr (b.CreateICmpEQ (its_class, proxy_class), ask, not_remote);

			b.SetInsertPoint (not_remote);
			b.CreateBr (done);
			result->addIncoming (null, not_remote);

			b.SetInsertPoint (ask);

			SmallVector<Value *, 2> remote_args = adapt_to_callee (b, remote_icall, { obj, target });
			CallBase *call;

			if (pad != nullptr) {
				call = b.CreateInvoke (remote_icall, done, pad, remote_args);
				new_pad_preds.push_back (ask);
			} else {
				CallInst *plain = b.CreateCall (remote_icall, remote_args);

				plain->setTailCallKind (CallInst::TCK_NoTail);
				call = plain;
				b.CreateBr (done);
			}

			result->addIncoming (call, ask);
		}
	}

	// hit/miss exist either because the general path built them, or because the
	// dynamic subtype chain's own zero-depth fallback did.
	if (hit != nullptr) {
		result->addIncoming (answer, hit);
		result->addIncoming (slow, miss);
	}

	// The builder may now point at one of the fallback blocks. Finish done
	// explicitly.
	b.SetInsertPoint (done);
	b.CreateBr (tail);

	// The phis of the two blocks the site reached name it as their predecessor.
	// Adding the same incoming value under each new name and then dropping
	// head's own keeps what they take, and leaves nothing naming head once the
	// site goes. The dynamic subtype chain is the one shape with two new blocks
	// that can unwind into pad on this site's behalf: its own remote or ask, and
	// the zero-depth fallback's miss.
	tail->replacePhiUsesWith (head, done);

	if (pad != nullptr) {
		for (PHINode &phi : pad->phis ()) {
			Value *from_head = phi.getIncomingValueForBlock (head);

			for (BasicBlock *pred : new_pad_preds)
				phi.addIncoming (from_head, pred);

			phi.removeIncomingValue (head, false);
		}
	}

	site->eraseFromParent ();

	// head is left without a terminator either way: an invoke was the
	// terminator, and a call left the branch the split wrote.
	if (Instruction *stale = head->getTerminatorOrNull ())
		stale->eraseFromParent ();

	b.SetInsertPoint (head);
	b.CreateCondBr (b.CreateIsNull (obj), done, first);
}

/// Lowers every call to the declaration \p name holds in \p m, and erases it.
bool
lower_all (Module &m, StringRef name, bool throw_on_fail)
{
	for (CallBase *site : builtin_sites (m, name))
		lower (site, throw_on_fail);

	return erase_builtin (m, name);
}

} // namespace

Function *
cast_func_decl (Module &m, bool throw_on_fail)
{
	StringRef name = throw_on_fail ? cast_castclass_name : cast_isinst_name;
	LLVMContext &c = m.getContext ();
	Type *ptr = PointerType::get (c, 0);
	Type *object = object_pointer_type (c);

	return builtin_decl (
		m, name,
		FunctionType::get (
			object,
			{ object, ptr, ptr, ptr, ptr, ptr, Type::getInt16Ty (c), Type::getInt16Ty (c) },
			false));
}

bool
lower_type_tests (Module &m)
{
	bool changed = lower_all (m, cast_isinst_name, false);

	return lower_all (m, cast_castclass_name, true) || changed;
}

} // namespace mono

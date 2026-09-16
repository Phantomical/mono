#include "builtins.hpp"

#include "alloc-func.hpp"
#include "analysis/builtins.hpp"
#include "analysis/constant-values.hpp"
#include "analysis/operand-class.hpp"
#include "analysis/strip-casts.hpp"
#include "analysis/vtable-info.hpp"
#include "array-address.hpp"
#include "array-shape.hpp"
#include "cast-func.hpp"
#include "compile-state.hpp"
#include "devirtualize.hpp"
#include "direct-call.hpp"
#include "gc-barrier.hpp"
#include "hidden-return.hpp"
#include "lower-builtins.hpp"
#include "method-symbols.hpp"
#include "runtime/naming.hpp"
#include "runtime/options.hpp"
#include "vtable-func.hpp"

#include "mono/metadata/abi-details.h"
#include "mono/metadata/class-internals.h"
#include "mono/metadata/metadata.h"
#include "mono/metadata/object-internals.h"

#include <llvm/ADT/SmallPtrSet.h>
#include <llvm/ADT/SmallVector.h>
#include <llvm/ADT/Twine.h>
#include <llvm/Analysis/BlockFrequencyInfo.h>
#include <llvm/Analysis/ValueTracking.h>
#include <llvm/IR/Attributes.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/GlobalObject.h>
#include <llvm/IR/GlobalValue.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/IR/InstrTypes.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Intrinsics.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/MDBuilder.h>
#include <llvm/IR/Metadata.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/ErrorHandling.h>

#include <algorithm>
#include <cstdint>
#include <optional>

using namespace llvm;

namespace mono {
namespace {

/// Times the eliminations take up a function's sites again.
///
/// One elimination exposes another. A type test that settles a receiver's
/// class settles the dispatch below it. A dispatch eliminated to a direct call
/// hands the next round an operand it could not read. Each round walks the
/// sites again, which is what this bounds.
constexpr unsigned elimination_rounds = 4;

SmallVector<CallBase *, 8>
sites_of (Function *decl, const Function *inside)
{
	SmallVector<CallBase *, 8> found;

	if (decl == nullptr)
		return found;

	for (User *user : decl->users ()) {
		auto *site = dyn_cast<CallBase> (user);

		if (site != nullptr && (inside == nullptr || site->getFunction () == inside))
			found.push_back (site);
	}

	return found;
}

} // namespace

Function *
builtin_decl (Module &m, StringRef name, FunctionType *shape)
{
	if (Function *existing = m.getFunction (name))
		return existing;

	return Function::Create (shape, GlobalValue::ExternalLinkage, name, m);
}

SmallVector<CallBase *, 8>
builtin_sites (Module &m, StringRef name)
{
	return sites_of (m.getFunction (name), nullptr);
}

SmallVector<CallBase *, 8>
builtin_sites (Function &f, StringRef name)
{
	return sites_of (f.getParent ()->getFunction (name), &f);
}

bool
erase_builtin (Module &m, StringRef name)
{
	Function *decl = m.getFunction (name);

	if (decl == nullptr)
		return false;

	if (!decl->use_empty ())
		report_fatal_error (Twine ("unlowered use of ") + name);

	decl->eraseFromParent ();
	return true;
}

namespace {

/// Replaces \p site with \p value, keeping the block structure the site's own
/// shape needs.
void
answer_with (CallBase *site, Value *value)
{
	site->replaceAllUsesWith (value);

	// An invoke is a terminator, and the answer raises nothing, so the unwind
	// edge goes with it. A pad left with no predecessor is dead code the
	// simplification behind this pass removes.
	if (auto *invoke = dyn_cast<InvokeInst> (site)) {
		BasicBlock *head = invoke->getParent ();

		UncondBrInst::Create (invoke->getNormalDest (), invoke->getIterator ());
		invoke->getUnwindDest ()->removePredecessor (head);
	}

	site->eraseFromParent ();
}

/**
 * How a test against \p target comes out for every value \p v can be.
 *
 * The answer is settled only where every value reaching \p v agrees. Two of
 * them can name two classes and still agree, which is why this combines the
 * answer over the sources rather than reading the one class they settle to.
 *
 * A null source agrees with either answer, because both rewrites leave null
 * where the operand is null.
 */
CastAnswer
answer_for (Value *v, MonoClass *target, const Function &f,
            const ConstantValues &values)
{
	const ValueSources &from_v = values.sources (v);
	CastAnswer agreed = CastAnswer::Unknown;
	bool constrained = false;

	for (Value *from : from_v.sources) {
		if (isa<ConstantPointerNull> (from))
			continue;

		std::pair<MonoClass *, bool> held = stated_class (from, f);
		CastAnswer answer = cast_answer (target, held.first, held.second);

		if (answer == CastAnswer::Unknown)
			return CastAnswer::Unknown;
		if (constrained && answer != agreed)
			return CastAnswer::Unknown;

		agreed = answer;
		constrained = true;
	}

	// Every source null, or none at all. Nothing is known about what \p v
	// holds.
	return constrained ? agreed : CastAnswer::Unknown;
}

/// The class the site tests against, or null where an rgctx fetch answered for
/// it and the IR holds no class.
MonoClass *
tested_class (const CallBase *site, const ConstantValues &values)
{
	const GlobalValue *global = values.global (site->getArgOperand (1));

	return global != nullptr ? get_class (*global) : nullptr;
}

/// Eliminates what it can of the sites in \p f that call the declaration \p name.
bool
eliminate_sites (Function &f, StringRef name, bool throw_on_fail,
                FunctionAnalysisManager &fam)
{
	bool changed = false;
	const ConstantValues *values = nullptr;

	for (CallBase *site : builtin_sites (f, name)) {
		if (values == nullptr)
			values = &fam.getResult<MonoConstantValues> (f);

		Value *obj = site->getArgOperand (0);
		MonoClass *target = tested_class (site, *values);
		CastAnswer answer = answer_for (obj, target, f, *values);

		// Both forms answer the operand where the test passes, since both
		// answer null for null and neither changes what it is handed.
		if (answer == CastAnswer::Yes) {
			answer_with (site, obj);
			changed = true;
			continue;
		}

		// Only isinst has a value for a test that fails. castclass raises
		// InvalidCastException for every operand but null, which is a site this
		// leaves for the lowering to write as it stands.
		if (answer == CastAnswer::No && !throw_on_fail) {
			answer_with (site, ConstantPointerNull::get (
						   PointerType::get (f.getContext (), 0)));
			changed = true;
			continue;
		}

		// A merge with no single answer can still answer edge by edge.
		// castclass keeps its cost here too: a "no" edge has to raise, and a
		// phi cannot raise on one edge alone.
		auto *phi = dyn_cast<PHINode> (const_cast<Value *> (strip_casts (obj)));

		if (answer != CastAnswer::Unknown || throw_on_fail || phi == nullptr)
			continue;

		Value *rebuilt = rebuild_isinst_over_incoming (*phi, [&] (Value *incoming) {
			return answer_for (incoming, target, f, *values);
		});

		if (rebuilt != nullptr) {
			answer_with (site, rebuilt);
			changed = true;
		}
	}

	return changed;
}

} // namespace

bool
eliminate_type_tests (Function &f, FunctionAnalysisManager &fam)
{
	// The classes ride as pointers into this process. An offline run over a
	// dumped module would read them as addresses of its own.
	if (current_compile ().domain == nullptr || !eliminate_casts ())
		return false;

	bool changed = eliminate_sites (f, cast_isinst_name, false, fam);

	return eliminate_sites (f, cast_castclass_name, true, fam) || changed;
}

namespace {

/// What a site of the declaration \p name reads, taken off \p info.
Constant *
field_of (StringRef name, const VTableInfo &info, Type *held)
{
	if (name == vtable_klass_name)
		return info.klass;

	if (name == vtable_type_name)
		return info.type;

	if (name == vtable_rank_name)
		return ConstantInt::get (held, info.rank);

	llvm_unreachable ("a vtable field with no value");
}

bool
eliminate_field (Function &f, StringRef name, FunctionAnalysisManager &fam)
{
	bool changed = false;
	const ConstantValues *values = nullptr;

	for (CallBase *site : builtin_sites (f, name)) {
		if (values == nullptr)
			values = &fam.getResult<MonoConstantValues> (f);

		const auto *vtable = dyn_cast_or_null<GlobalObject> (
			values->global (site->getArgOperand (0)));

		if (vtable == nullptr)
			continue;

		std::optional<VTableInfo> info = vtable_info (*vtable);

		if (!info)
			continue;

		site->replaceAllUsesWith (field_of (name, *info, site->getType ()));
		site->eraseFromParent ();
		changed = true;
	}

	return changed;
}

} // namespace

bool
eliminate_object_vtables (Function &f, FunctionAnalysisManager &fam)
{
	const CompileState &compile = current_compile ();

	if (compile.domain == nullptr || !compile.vtable_of)
		return false;

	// The reads are collected before any is erased, because erasing one moves
	// the iterator this walks with.
	SmallVector<LoadInst *, 8> reads;

	for (Instruction &i : instructions (f))
		if (object_vtable_read (&i) != nullptr)
			reads.push_back (cast<LoadInst> (&i));

	bool changed = false;

	const ConstantValues *values = nullptr;

	for (LoadInst *read : reads) {
		if (values == nullptr)
			values = &fam.getResult<MonoConstantValues> (f);

		MonoClass *klass = exact_class (object_vtable_read (read), f, *values);

		if (klass == nullptr)
			continue;

		Constant *vtable = compile.vtable_of (*f.getParent (), klass);

		if (vtable == nullptr)
			continue;

		read->replaceAllUsesWith (vtable);
		read->eraseFromParent ();
		changed = true;
	}

	return changed;
}

bool
eliminate_vtable_fields (Function &f, FunctionAnalysisManager &fam)
{
	bool changed = eliminate_field (f, vtable_klass_name, fam);

	changed |= eliminate_field (f, vtable_type_name, fam);

	return eliminate_field (f, vtable_rank_name, fam) || changed;
}

bool
eliminate_stack_barriers (Function &f)
{
	bool changed = false;

	for (CallBase *site : builtin_sites (f, gc_barrier_name)) {
		// The declaration is nounwind, so every site is a call.
		auto *call = dyn_cast<CallInst> (site);

		if (call == nullptr)
			continue;

		if (!points_to_the_frame (call->getArgOperand (0)))
			continue;

		// The call names the destination, so the alloca escapes and SROA
		// promotes none of it. Erasing the call is what lets a local holding a
		// reference reach a register, and the store beside it go with it.
		call->eraseFromParent ();
		changed = true;
	}

	return changed;
}

bool
open_value_copies (Function &f)
{
	Module &m = *f.getParent ();
	bool changed = false;

	if (m.getFunction (gc_value_copy_name) == nullptr)
		return false;

	for (CallBase *site : builtin_sites (f, gc_value_copy_name)) {
		// The declaration is nounwind, so every site is a call.
		auto *call = dyn_cast<CallInst> (site);

		if (call == nullptr)
			continue;

		Value *dest = call->getArgOperand (0);

		/*
		 * A destination in the frame owes no card, so the whole call becomes the
		 * copy it was made of. The collector scans a frame as a root at each
		 * collection, and a card only ever records a reference in the heap.
		 *
		 * A destination in the heap keeps the call. The copy and the cards have
		 * to reach the collector together, which is what the icall behind this
		 * builtin does and what a copy standing in the open cannot.
		 */
		if (!points_to_the_frame (dest))
			continue;

		IRBuilder<> b (call);

		b.SetCurrentDebugLocation (call->getDebugLoc ());

		Value *src = call->getArgOperand (1);
		Value *count = b.CreateZExt (call->getArgOperand (2), b.getInt64Ty ());
		Value *bytes = b.CreateMul (count, call->getArgOperand (3));
		Align dest_align = call->getParamAlign (0).valueOrOne ();
		Align src_align = call->getParamAlign (1).valueOrOne ();

		if (call->hasFnAttr (gc_no_overlap_attr))
			b.CreateMemCpy (dest, dest_align, src, src_align, bytes);
		else
			b.CreateMemMove (dest, dest_align, src, src_align, bytes);

		call->eraseFromParent ();
		changed = true;
	}

	return changed;
}

namespace {

/// What a guard's true edge weighs where the function carries no profile.
///
/// Only the ratio against the zero below reaches BranchProbabilityInfo, so this
/// stands in for a count rather than claiming one.
constexpr uint64_t unprofiled_guard_weight = 1000;

/// How the target takes the receiver the delegate holds.
enum class Receiver {
	/// A static target: the delegate does not travel into the call at all.
	none,

	/// An instance target bound to delegate->target, which becomes its `this`.
	bound,
};

/// How \p target takes the receiver of a delegate whose Invoke declares
/// \p invoke_params parameters, or nothing where this is a shape the
/// elimination does not write.
///
/// mono_delegate_trampoline () settles the same question by the same counts
/// (mini-trampolines.c). Both shapes left out have a parameter count that
/// differs from Invoke's by one, so the equality below is what refuses them: a
/// closed static takes delegate->target as its own first argument, and an open
/// instance takes Invoke's first argument as `this`.
std::optional<Receiver>
receiver_of (MonoMethod *target, unsigned invoke_params)
{
	MonoMethodSignature *tsig = mono_method_signature_internal (target);

	if (tsig == nullptr || tsig->param_count != invoke_params)
		return std::nullopt;

	return tsig->hasthis ? Receiver::bound : Receiver::none;
}

/// Whether the site's own prototype is one the mapping below can rewrite.
///
/// A key rides the IMT register, not an argument, so the mapping below has no
/// slot to put it in. A hidden return pointer is an ordinary argument, and
/// the mapping below relocates it.
bool
plainly_shaped (const CallBase &site)
{
	for (unsigned i = 0; i < site.arg_size (); ++i)
		if (site.paramHasAttr (i, Attribute::Nest))
			return false;

	return true;
}

/// The argument index of \p site's hidden return pointer, or nothing where its
/// return travels in the return registers.
std::optional<unsigned>
hidden_return_argument (const CallBase &site)
{
	for (unsigned i = 0; i < site.arg_size (); ++i)
		if (site.paramHasAttr (i, Attribute::StructRet))
			return i;

	return std::nullopt;
}

/// Loads the field at \p offset off \p delegate.
Value *
delegate_field (IRBuilderBase &b, Value *delegate, int offset, const Twine &name)
{
	return b.CreateAlignedLoad (b.getPtrTy (),
	                            b.CreateGEP (b.getInt8Ty (), delegate,
	                                         b.getInt32 (offset)),
	                            Align (TARGET_SIZEOF_VOID_P), name);
}

/// The prototype \p target's entry is called with, given the site's and where
/// its hidden return pointer sits, if it has one.
///
/// A bound instance target keeps the site's own shape: `this` takes the
/// delegate's slot, so the arity the pointer is positioned against does not
/// change. A static target loses the delegate argument, so the returned shape
/// puts the pointer where create_method_decl () computes it for the smaller
/// arity.
FunctionType *
entry_shape (FunctionType *site, Receiver receiver, Type *hidden)
{
	if (receiver == Receiver::bound)
		return site;

	FunctionType *natural = hidden != nullptr ? natural_prototype (site, hidden) : site;
	FunctionType *dropped =
		FunctionType::get (natural->getReturnType (), natural->params ().drop_front (),
		                   false);

	return hidden != nullptr ? hidden_return_prototype (dropped, hidden) : dropped;
}

/// The arguments the direct call is made with, given where \p site's hidden
/// return pointer sits, if it has one.
///
/// The delegate's own slot never travels: it either goes away or is taken by a
/// receiver this pass loaded. The hidden return pointer travels too, but the
/// drop can move it: hidden_return_index () is asked again at the new arity,
/// the same way entry_shape () placed it.
void
direct_arguments (IRBuilderBase &b, CallBase &site, Receiver receiver,
                  std::optional<unsigned> hidden, SmallVectorImpl<Value *> &out)
{
	SmallVector<Value *, 8> natural;

	if (receiver == Receiver::bound)
		natural.push_back (delegate_field (b, site.getArgOperand (0),
		                                   MONO_STRUCT_OFFSET (MonoDelegate, target),
		                                   "delegate_target"));

	for (unsigned i = 1; i < site.arg_size (); ++i)
		if (i != hidden)
			natural.push_back (site.getArgOperand (i));

	if (!hidden) {
		out.append (natural.begin (), natural.end ());
		return;
	}

	auto at = natural.begin () + hidden_return_index (natural.size () + 1);

	out.append (natural.begin (), at);
	out.push_back (site.getArgOperand (*hidden));
	out.append (at, natural.end ());
}

/// What the site said about its arguments, moved onto the direct call's, given
/// where its hidden return pointer sits, if it has one.
///
/// Mirrors direct_arguments ()'s mapping, so the two line up position for
/// position: the delegate's slot carries none of its own, and the hidden
/// pointer's attributes move to the same new position.
AttributeList
direct_attributes (const CallBase &site, Receiver receiver, std::optional<unsigned> hidden)
{
	AttributeList was = site.getAttributes ();
	SmallVector<AttributeSet, 8> natural;

	if (receiver == Receiver::bound)
		natural.push_back (AttributeSet ());

	for (unsigned i = 1; i < site.arg_size (); ++i)
		if (i != hidden)
			natural.push_back (was.getParamAttrs (i));

	if (!hidden)
		return AttributeList::get (site.getContext (), was.getFnAttrs (),
		                           was.getRetAttrs (), natural);

	auto at = natural.begin () + hidden_return_index (natural.size () + 1);
	SmallVector<AttributeSet, 8> params (natural.begin (), at);

	params.push_back (was.getParamAttrs (*hidden));
	params.append (at, natural.end ());

	return AttributeList::get (site.getContext (), was.getFnAttrs (),
	                           was.getRetAttrs (), params);
}

/// Gives \p direct everything the site it stands for said about itself.
void
carry_site (const CallBase &site, CallBase &direct, Receiver receiver,
           std::optional<unsigned> hidden)
{
	direct.setCallingConv (site.getCallingConv ());
	direct.setAttributes (direct_attributes (site, receiver, hidden));
	direct.setDebugLoc (site.getDebugLoc ());

	if (const auto *was = dyn_cast<CallInst> (&site))
		if (auto *now = dyn_cast<CallInst> (&direct))
			now->setTailCallKind (was->getTailCallKind ());
}

/// Writes a call of \p entry standing for \p site, landing where \p site landed.
CallBase *
call_entry (IRBuilderBase &b, CallBase &site, Function *entry, Receiver receiver,
            BasicBlock *normal)
{
	std::optional<unsigned> hidden = hidden_return_argument (site);
	SmallVector<Value *, 8> args;

	direct_arguments (b, site, receiver, hidden, args);

	CallBase *direct;

	if (auto *unwinds = dyn_cast<InvokeInst> (&site))
		direct = b.CreateInvoke (entry, normal, unwinds->getUnwindDest (), args);
	else
		direct = b.CreateCall (entry, args);

	carry_site (site, *direct, receiver, hidden);

	return direct;
}

/// The entry \p site may enter \p named through, with the receiver shape that
/// needs, or nothing where the site and the method are not a pair the
/// elimination writes.
std::optional<std::pair<Function *, Receiver>>
entry_at (CallBase &site, MonoMethod *named, const CompileState &compile)
{
	MonoMethod *target = nameable (named);

	if (target == nullptr || !plainly_shaped (site))
		return std::nullopt;

	/*
	 * A value type's instance method is published at its unboxing entry, which
	 * expects the receiver already stepped past the header. The delegate holds
	 * the boxed object, so entering that address with it is a wrong receiver
	 * rather than a slower call.
	 */
	if (publishes_unbox_entry (target))
		return std::nullopt;

	std::optional<unsigned> hidden_at = hidden_return_argument (site);

	// The delegate rides argument 0, and a hidden return pointer rides one
	// more where the site has one. What is left is Invoke's own parameter
	// count, without naming Invoke.
	unsigned invoke_params = site.arg_size () - 1 - (hidden_at ? 1 : 0);
	std::optional<Receiver> receiver = receiver_of (target, invoke_params);

	if (!receiver)
		return std::nullopt;

	Type *hidden = hidden_at ? site.getParamStructRetType (*hidden_at) : nullptr;
	Function *entry = entry_for (*site.getModule (), target,
	                             entry_shape (site.getFunctionType (), *receiver, hidden),
	                             compile);

	if (entry == nullptr)
		return std::nullopt;

	return std::make_pair (entry, *receiver);
}

/// Every Invoke in \p f, read off the shape the translator writes.
SmallVector<CallBase *, 8>
invoke_sites (Function &f)
{
	SmallVector<CallBase *, 8> found;

	for (BasicBlock &block : f)
		for (Instruction &at : block)
			if (auto *site = dyn_cast<CallBase> (&at))
				if (reads_callee_off_delegate (*site))
					found.push_back (site);

	return found;
}

/// Whether \p call is the keep_alive () marker call.cpp writes for
/// \p delegate.
bool
is_keep_alive_of (const CallBase &call, const Value &delegate)
{
	return call.getIntrinsicID () == Intrinsic::fake_use
	       && call.arg_size () == 1
	       && strip_casts (call.getArgOperand (0)) == strip_casts (&delegate);
}

/// The keep_alive () marker written for \p site's delegate, or null where
/// call.cpp emitted none.
CallInst *
keep_alive_for (CallBase &site)
{
	BasicBlock *scope = site.getParent ();
	// Starts right after site, not at the block's front, so an earlier
	// Invoke on the same delegate in this block cannot donate its marker.
	BasicBlock::iterator start = std::next (site.getIterator ());

	if (auto *unwinds = dyn_cast<InvokeInst> (&site)) {
		scope = unwinds->getNormalDest ();
		start = scope->begin ();
	}

	for (Instruction &at : llvm::make_range (start, scope->end ()))
		if (auto *call = dyn_cast<CallInst> (&at))
			if (is_keep_alive_of (*call, *site.getArgOperand (0)))
				return call;

	return nullptr;
}

/// Replaces \p site with a call that enters \p entry.
void
enter_directly (CallBase &site, Function *entry, Receiver receiver)
{
	IRBuilder<> b (&site);

	CallBase *direct = call_entry (b, site, entry, receiver,
	                               isa<InvokeInst> (site)
	                                       ? cast<InvokeInst> (site).getNormalDest ()
	                                       : nullptr);

	site.replaceAllUsesWith (direct);
	site.eraseFromParent ();
}

/// Adds an incoming for \p arm to every phi in \p block that has one for
/// \p had, carrying the same value.
///
/// A block both arms reach needs two incomings where it had one. Renaming the
/// single incoming, which is what a lowering with one calling arm does, would
/// leave the other arm's edge unnamed.
void
share_phis_with (BasicBlock *block, BasicBlock *had, BasicBlock *arm)
{
	for (PHINode &phi : block->phis ()) {
		int at = phi.getBasicBlockIndex (had);

		if (at >= 0)
			phi.addIncoming (phi.getIncomingValue (at), arm);
	}
}

/// The weights a guard's branch carries, given the count its block had.
///
/// The zero is the point: the site's whole count goes to the direct call, which
/// is what a cost model reading block counts weighs the target at. The other
/// arm is a call nothing can inline, so splitting the count with it buys
/// nothing.
MDNode *
guard_weights (LLVMContext &c, std::optional<uint64_t> count)
{
	MDBuilder md (c);
	uint64_t hot = std::max<uint64_t> (count.value_or (unprofiled_guard_weight), 1);

	return md.createBranchWeights (
		(uint32_t) std::min<uint64_t> (hot, UINT32_MAX), 0);
}

/**
 * Whether the block around \p site is a shape guard_entry () below can split
 * into two arms and merge back into one answer.
 *
 * guard_entry () writes one merge phi in the block execution returns to,
 * with exactly one incoming value for the fast arm and one for the slow
 * arm. An invoke returns to a block that already exists in the function,
 * and that block can already have another predecessor. The merge phi would
 * then need an incoming value for that edge too, and no value the rewrite
 * writes reaches it.
 *
 * A plain call always fits. guard_entry () splits its own block to make the
 * block it returns to, so that block gets no predecessor but the guard.
 */
bool
guard_fits (CallBase *site)
{
	auto *unwinds = dyn_cast<InvokeInst> (site);

	if (unwinds == nullptr)
		return true;

	return unwinds->getNormalDest ()->getUniquePredecessor () == site->getParent ();
}

/// Sends \p site through a compare of the delegate's entry against \p entry,
/// with a direct call on the arm that matches.
///
/// \p marker, where not null, is call.cpp's keep_alive () for this delegate.
/// The compare only proves \p entry on the direct-call arm, so \p marker
/// comes off there and stays on the dispatch arm. \p target_dynamic keeps
/// it on both arms, because only the delegate roots a dynamic method's
/// code across the call.
void
guard_entry (CallBase &site, Function *entry, Receiver receiver, MDNode *weights,
            CallInst *marker, bool target_dynamic)
{
	LLVMContext &c = site.getContext ();
	Function *f = site.getFunction ();
	BasicBlock *head = site.getParent ();
	Value *delegate = site.getArgOperand (0);
	auto *unwinds = dyn_cast<InvokeInst> (&site);

	BasicBlock *tail = unwinds != nullptr
	                           ? unwinds->getNormalDest ()
	                           : head->splitBasicBlock (site.getIterator (),
	                                                    "delegate_done");
	BasicBlock *pad = unwinds != nullptr ? unwinds->getUnwindDest () : nullptr;

	BasicBlock *fast = BasicBlock::Create (c, "delegate_direct", f, tail);
	BasicBlock *slow = BasicBlock::Create (c, "delegate_dispatch", f, tail);

	// tail's real predecessor on the dispatching arm: slow itself, unless the
	// marker below needs a block of its own past slow's own terminator.
	BasicBlock *slow_pred = slow;

	// The site keeps its own identity on the dispatching arm, so everything it
	// carried travels without being copied.
	site.removeFromParent ();
	site.insertInto (slow, slow->end ());

	if (marker != nullptr && !target_dynamic) {
		marker->removeFromParent ();

		if (unwinds == nullptr) {
			marker->insertInto (slow, slow->end ());
		} else {
			slow_pred = BasicBlock::Create (c, "delegate_dispatch_done", f, tail);
			marker->insertInto (slow_pred, slow_pred->end ());
			IRBuilder<> (slow_pred).CreateBr (tail);
			unwinds->setNormalDest (slow_pred);
		}
	}

	IRBuilder<> b (fast);

	b.SetCurrentDebugLocation (site.getDebugLoc ());

	CallBase *direct = call_entry (b, site, entry, receiver, tail);

	if (unwinds == nullptr) {
		b.CreateBr (tail);
		IRBuilder<> (slow).CreateBr (tail);
	} else {
		// Both arms reach the pad and the continuation now, where the one
		// invoke reached each of them from the block above. tail's real
		// predecessor here is slow_pred, defined above, not slow itself.
		tail->replacePhiUsesWith (head, slow_pred);
		share_phis_with (tail, slow_pred, fast);
		pad->replacePhiUsesWith (head, slow);
		share_phis_with (pad, slow, fast);
	}

	if (!site.getType ()->isVoidTy ()) {
		PHINode *merged = PHINode::Create (site.getType (), 2, "delegate_result",
		                                   tail->getFirstNonPHIIt ());

		// Before the incoming values name it, so that replacing the site's uses
		// does not reach into the phi's own operand for the dispatched answer.
		site.replaceAllUsesWith (merged);
		merged->addIncoming (direct, fast);
		merged->addIncoming (&site, slow_pred);
	}

	// head is left without a terminator either way: an invoke was the
	// terminator, and a call left the branch the split wrote.
	if (Instruction *stale = head->getTerminatorOrNull ())
		stale->eraseFromParent ();

	IRBuilder<> guard (head);

	guard.SetCurrentDebugLocation (site.getDebugLoc ());

	auto *held = cast<LoadInst> (delegate_field (
		guard, delegate, MONO_STRUCT_OFFSET (MonoDelegate, method_ptr),
		"delegate_entry"));

	mark_delegate_method_ptr_read (held);

	guard.CreateCondBr (guard.CreateICmpEQ (held, entry, "delegate_hit"), fast, slow)
		->setMetadata (LLVMContext::MD_prof, weights);
}

/// Whether this compile can name a method at all.
bool
can_name_methods ()
{
	const CompileState &compile = current_compile ();

	// The methods ride as pointers into this process. An offline run over a
	// dumped module would read them as addresses of its own.
	return compile.domain != nullptr && compile.publish && eliminate_delegates ();
}

} // namespace

bool
eliminate_delegate_invokes (Function &f, BlockFrequencyInfo &counts, const ConstantValues &values)
{
	if (!can_name_methods ())
		return false;

	SmallVector<CallBase *, 8> sites = invoke_sites (f);

	if (sites.empty ())
		return false;

	const CompileState &compile = current_compile ();

	/// A site to rewrite, with the weights its guard will carry. Null weights
	/// are a settled target, which is entered without one.
	struct Pending {
		CallBase *site;
		MonoMethod *target;
		MDNode *weights;
	};

	// Every weight is read before the first split, because a block this pass
	// makes has no count of its own and the analysis is stale the moment one
	// appears.
	SmallVector<Pending, 8> pending;

	for (CallBase *site : sites) {
		DelegateTarget found = delegate_target_at (site->getArgOperand (0), values);

		if (found.method == nullptr)
			continue;

		// enter_directly () below leaves the site's block alone, so only the
		// guarded arm needs a shape guard_fits () can split and merge.
		if (!found.settled && !guard_fits (site))
			continue;

		pending.push_back (
			{ site, found.method,
		          found.settled ? nullptr
		                        : guard_weights (f.getContext (),
		                                         counts.getBlockProfileCount (
							         site->getParent ())) });
	}

	bool changed = false;

	for (const Pending &at : pending) {
		std::optional entry = entry_at (*at.site, at.target, compile);

		if (!entry)
			continue;

		CallInst *marker = keep_alive_for (*at.site);

		if (at.weights == nullptr) {
			enter_directly (*at.site, entry->first, entry->second);

			// No dispatch survives this site, so only a dynamic method still
			// needs the marker to keep its code alive.
			if (marker != nullptr && !at.target->dynamic)
				marker->eraseFromParent ();
		} else {
			guard_entry (*at.site, entry->first, entry->second, at.weights,
			            marker, at.target->dynamic);
		}

		changed = true;
	}

	return changed;
}

PreservedAnalyses
MonoBuiltinConstProp::run (Function &f, FunctionAnalysisManager &fam)
{
	bool changed = false;

	for (unsigned round = 0; round < elimination_rounds; ++round) {
		// A round rewrites what the round before it read, so each one asks
		// again rather than answering against a stale answer.
		if (changed)
			fam.invalidate (f, PreservedAnalyses::none ());

		// Type tests first: eliminating one is what delivers the allocation a
		// chain's receiver comes from.
		bool again = eliminate_type_tests (f, fam);

		again |= eliminate_object_vtables (f, fam);
		again |= eliminate_vtable_fields (f, fam);
		again |= eliminate_dispatch_sites (f, fam);
		again |= eliminate_array_shapes (f, fam);

		/*
		 * Placed with the eliminations, ahead of the post_optimization
		 * lowering, which leaves a barrier as open code and a value copy as
		 * an icall. A pass reads neither one back. This point sits behind
		 * SROA, which is what settles the two pointers a value copy asks
		 * about.
		 */
		again |= eliminate_stack_barriers (f);
		again |= open_value_copies (f);

		if (!again)
			break;

		changed = true;
	}

	return changed ? PreservedAnalyses::none () : PreservedAnalyses::all ();
}

PreservedAnalyses
MonoBuiltinLower::run (Module &m, ModuleAnalysisManager &)
{
	bool changed = false;

	switch (stage) {
	case LowerStage::pre_simplification:
		changed = lower_array_addresses (m);
		changed |= lower_runtime_builtins (m);
		break;

	case LowerStage::pre_profile:
		changed = lower_array_shapes (m);
		break;

	case LowerStage::post_inline:
		changed = lower_vtable_reads (m);
		changed |= lower_type_tests (m);
		break;

	case LowerStage::post_optimization:
		changed = lower_allocations (m);
		changed |= lower_gc_barriers (m);
		changed |= lower_gc_value_copies (m);
		break;
	}

	return changed ? PreservedAnalyses::none () : PreservedAnalyses::all ();
}

} // namespace mono

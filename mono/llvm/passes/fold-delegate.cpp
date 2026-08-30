/**
 * \file
 * \brief Entering a delegate's target instead of reading it off the delegate.
 *
 * The two entry points differ only in whether the receiver's identity is known,
 * so the walk that finds the target, the gates on entering it and the argument
 * mapping are shared and only the emission forks.
 */

#include "fold-delegate.hpp"

#include "analysis/constant-values.hpp"
#include "analysis/operand-class.hpp"
#include "analysis/strip-casts.hpp"
#include "compile-state.hpp"
#include "direct-call.hpp"
#include "runtime/naming.hpp"
#include "runtime/options.hpp"

#include "mono/metadata/abi-details.h"
#include "mono/metadata/class-internals.h"
#include "mono/metadata/metadata.h"
#include "mono/metadata/object-internals.h"

#include <llvm/ADT/SmallPtrSet.h>
#include <llvm/ADT/SmallVector.h>
#include <llvm/Analysis/BlockFrequencyInfo.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/InstrTypes.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/MDBuilder.h>
#include <llvm/IR/Metadata.h>
#include <llvm/IR/Module.h>

#include <algorithm>
#include <cstdint>
#include <optional>

using namespace llvm;

namespace mono {
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
/// \p invoke_params parameters, or nothing where this is a shape the fold does
/// not write.
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
/// A hidden return pointer sits at the argument the site's arity puts it at, so
/// dropping the delegate can move it. A key rides a slot of its own that the
/// target does not have. Neither shape reaches a delegate built by a C#
/// compiler, so both are refused rather than mapped.
bool
plainly_shaped (const CallBase &site)
{
	for (unsigned i = 0; i < site.arg_size (); ++i)
		if (site.paramHasAttr (i, Attribute::StructRet)
		    || site.paramHasAttr (i, Attribute::Nest))
			return false;

	return true;
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

/// The prototype \p target's entry is called with, given the site's.
///
/// A bound instance target is called with the shape the site already had: its
/// `this` takes the slot the delegate travelled in and Invoke declares the rest
/// of the arguments exactly as the target does.
FunctionType *
entry_shape (FunctionType *site, Receiver receiver)
{
	if (receiver == Receiver::bound)
		return site;

	return FunctionType::get (site->getReturnType (), site->params ().drop_front (),
	                          false);
}

/// The arguments the direct call is made with.
void
direct_arguments (IRBuilderBase &b, CallBase &site, Receiver receiver,
                  SmallVectorImpl<Value *> &out)
{
	if (receiver == Receiver::bound)
		out.push_back (delegate_field (b, site.getArgOperand (0),
		                               MONO_STRUCT_OFFSET (MonoDelegate, target),
		                               "delegate_target"));

	out.append (site.arg_begin () + 1, site.arg_end ());
}

/// What the site said about its arguments, moved onto the direct call's.
///
/// The delegate's own slot never travels: it either goes away or is taken by a
/// receiver this pass loaded, and what the site said about a delegate does not
/// describe either.
AttributeList
direct_attributes (const CallBase &site, Receiver receiver)
{
	AttributeList was = site.getAttributes ();
	SmallVector<AttributeSet, 8> params;

	if (receiver == Receiver::bound)
		params.push_back (AttributeSet ());

	for (unsigned i = 1; i < site.arg_size (); ++i)
		params.push_back (was.getParamAttrs (i));

	return AttributeList::get (site.getContext (), was.getFnAttrs (),
	                           was.getRetAttrs (), params);
}

/// Gives \p direct everything the site it stands for said about itself.
void
carry_site (const CallBase &site, CallBase &direct, Receiver receiver)
{
	direct.setCallingConv (site.getCallingConv ());
	direct.setAttributes (direct_attributes (site, receiver));
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
	SmallVector<Value *, 8> args;

	direct_arguments (b, site, receiver, args);

	CallBase *direct;

	if (auto *unwinds = dyn_cast<InvokeInst> (&site))
		direct = b.CreateInvoke (entry, normal, unwinds->getUnwindDest (), args);
	else
		direct = b.CreateCall (entry, args);

	carry_site (site, *direct, receiver);

	return direct;
}

/// The entry \p site may enter \p named through, with the receiver shape that
/// needs, or nothing where the site and the method are not a pair the fold
/// writes.
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

	// The delegate rides argument 0 and Invoke declares the rest, so the site
	// states Invoke's parameter count without naming Invoke.
	std::optional<Receiver> receiver = receiver_of (target, site.arg_size () - 1);

	if (!receiver)
		return std::nullopt;

	Function *entry = entry_for (*site.getModule (), target,
	                             entry_shape (site.getFunctionType (), *receiver),
	                             compile);

	if (entry == nullptr)
		return std::nullopt;

	return std::make_pair (entry, *receiver);
}

/**
 * Whether \p site reads its callee out of the delegate it passes.
 *
 * `delegate_invoke_callee ()` (`method-to-llvm/call.cpp`) writes every Invoke as
 * one shape, and this is that shape read back:
 *
 *     %impl   = load ptr, ptr (getelementptr i8, %d, invoke_impl)
 *     %isnull = icmp eq ptr %impl, null
 *     %callee = select i1 %isnull, %dispatch, %impl
 *     call %callee (%d, ...)
 *
 * The delegate the site passes has to be the object the load reads, which is
 * what separates this from any other call through a selected pointer.
 *
 * Reading the shape rather than a mark on the call is what lets the fold see a
 * site an inliner moved. Metadata does not survive a transform that builds a new
 * instruction, and inlining a body into a try does exactly that: it writes each
 * call again as an invoke of the caller's pad.
 */
bool
reads_callee_off_delegate (const CallBase &site)
{
	if (site.arg_size () < 1 || site.getCalledFunction () != nullptr)
		return false;

	const auto *pick = dyn_cast<SelectInst> (strip_casts (site.getCalledOperand ()));

	if (pick == nullptr)
		return false;

	const auto *test = dyn_cast<ICmpInst> (pick->getCondition ());
	const auto *impl = dyn_cast<LoadInst> (strip_casts (pick->getFalseValue ()));

	// The arms sit the way CreateSelect (CreateIsNull (impl), dispatch, impl)
	// wrote them, so the load answers on the arm where it is not null.
	if (test == nullptr || impl == nullptr
	    || test->getPredicate () != ICmpInst::ICMP_EQ
	    || !isa<ConstantPointerNull> (test->getOperand (1))
	    || strip_casts (test->getOperand (0)) != impl)
		return false;

	const DataLayout &layout = site.getModule ()->getDataLayout ();
	const Value *address = impl->getPointerOperand ();
	APInt offset (layout.getIndexTypeSizeInBits (address->getType ()), 0);
	const Value *object = address->stripAndAccumulateConstantOffsets (
		layout, offset, /*AllowNonInbounds=*/true);

	// stripAndAccumulateConstantOffsets () peels offsets and stops, so it leaves
	// a freeze that the other side's peel takes off.
	return strip_casts (object) == strip_casts (site.getArgOperand (0))
	       && offset == MONO_STRUCT_OFFSET (MonoDelegate, invoke_impl);
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
void
guard_entry (CallBase &site, Function *entry, Receiver receiver, MDNode *weights)
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

	// The site keeps its own identity on the dispatching arm, so everything it
	// carried travels without being copied.
	site.removeFromParent ();
	site.insertInto (slow, slow->end ());

	IRBuilder<> b (fast);

	b.SetCurrentDebugLocation (site.getDebugLoc ());

	CallBase *direct = call_entry (b, site, entry, receiver, tail);

	if (unwinds == nullptr) {
		b.CreateBr (tail);
		IRBuilder<> (slow).CreateBr (tail);
	} else {
		// Both arms reach the pad and the continuation now, where the one
		// invoke reached each of them from the block above.
		tail->replacePhiUsesWith (head, slow);
		share_phis_with (tail, slow, fast);
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
		merged->addIncoming (&site, slow);
	}

	// head is left without a terminator either way: an invoke was the
	// terminator, and a call left the branch the split wrote.
	if (Instruction *stale = head->getTerminator ())
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
	return compile.domain != nullptr && compile.publish && fold_delegates ();
}

} // namespace

void
mark_delegate_method_ptr_read (LoadInst *load)
{
	load->setMetadata (LLVMContext::MD_invariant_group,
	                   MDNode::get (load->getContext (), {}));
}

DelegateTarget
delegate_target_at (Value *receiver, const ConstantValues &values)
{
	const ValueSources &from = values.sources (receiver);
	DelegateTarget answer;

	/*
	 * A delegate copied into a field and read back is answered by the store
	 * walk behind `sources ()`. That answer leaves out the field's own
	 * zero-filled initial value, which is safe here for a reason no class
	 * caller has: the Invoke this receiver feeds dereferences it, so a null
	 * delegate faults at the site instead of reaching a wrong target.
	 *
	 * A field whose object escapes still names the delegates the stores this
	 * walk can see put there. Such an answer is a candidate rather than the
	 * target, which is what leaves `settled` false and sends the site to the
	 * guarded form.
	 */
	bool opaque = false;

	for (const Value *at : from.sources) {
		if (isa<ConstantPointerNull> (at))
			continue;

		MonoMethod *named = delegate_target (at);

		// A source naming no method is one more path the compare covers, so
		// it leaves a candidate rather than emptying the answer.
		if (named == nullptr) {
			opaque = true;
			continue;
		}

		// Two sources naming different methods leave nothing to compare
		// against: picking one is a guess a profile would have to settle,
		// and there is no profile here.
		if (answer.method != nullptr && answer.method != named)
			return {};

		answer.method = named;
	}

	answer.settled = answer.method != nullptr && !opaque;

	return answer;
}

PreservedAnalyses
FoldDelegateInvokesPass::run (Function &f, FunctionAnalysisManager &fam)
{
	if (!can_name_methods ())
		return PreservedAnalyses::all ();

	SmallVector<CallBase *, 8> sites = invoke_sites (f);

	if (sites.empty ())
		return PreservedAnalyses::all ();

	const CompileState &compile = current_compile ();
	BlockFrequencyInfo &counts = fam.getResult<BlockFrequencyAnalysis> (f);
	const ConstantValues &values = fam.getResult<MonoMemoryValues> (f);

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

		if (at.weights == nullptr)
			enter_directly (*at.site, entry->first, entry->second);
		else
			guard_entry (*at.site, entry->first, entry->second, at.weights);

		changed = true;
	}

	return changed ? PreservedAnalyses::none () : PreservedAnalyses::all ();
}

} // namespace mono

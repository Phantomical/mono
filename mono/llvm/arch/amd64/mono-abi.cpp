/**
 * \file
 * \brief Lowering calls that cross into C.
 *
 * Generated code carries every value in its natural IR type. A call that
 * crosses into C has to classify those values the way the psABI does instead.
 * Three kinds of site do: a raw entry point, a `calli` through a native
 * signature, and a wrapper that native code enters.
 *
 * The translator marks such a call with the `monocc` attribute and emits it
 * naturally. MonoAbiPass rewrites it after the optimization pipeline has run,
 * so nothing upstream of it ever sees a lowered call.
 *
 * The classification is done from IR types and the DataLayout:
 *
 *   - a value type of up to 16 bytes travels as one or two register words,
 *     each in the integer or the float file;
 *   - one bigger than 16 bytes, or one that arrives after the argument
 *     registers have run out, is copied onto the stack - a byval pointer here;
 *   - a return that does not fit the return registers travels through a
 *     pointer the caller passes as the first argument.
 *
 * The walk recurses to leaf fields the way collect_field_info_nested does
 * (mono/mini/arch-amd64.c), and agrees with it without ever seeing the
 * metadata. What makes that hold is the layout: the translator emits the same
 * shape mini reads, with padding spelled the one way (layout.hpp) that keeps
 * it distinguishable from data.
 */

#include "arch/arch.hpp"
#include "hidden-return.hpp"
#include "layout.hpp"

#include <llvm/ADT/STLFunctionalExtras.h>
#include <llvm/ADT/SmallVector.h>
#include <llvm/IR/Attributes.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DataLayout.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/InstrTypes.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/Alignment.h>
#include <llvm/Support/ErrorHandling.h>

#include <utility>

using namespace llvm;

namespace mono::arch {

namespace {

/// One eightbyte of a value type, classified as the register file it rides in.
enum class Quad { None, Integer, Sse };

/// How a value type crosses a call, before register availability is known.
struct AggShape {
	bool memory = false; ///< always a stack copy, no matter the registers
	unsigned nquads = 0; ///< 0 when nothing travels at all
	Quad cls[2] = { Quad::None, Quad::None };
	unsigned qsize[2] = { 0, 0 };
};

struct Leaf {
	uint64_t offset;
	uint64_t size;
	bool sse;
};

/// Collects every leaf field of \p t into \p out, each with its flattened
/// offset - mini's collect_field_info_nested, walked over the IR layout. The
/// walk steps over what the translator spelled as padding (layout.hpp), where
/// mini's walk finds no field either.
void
collect_leaves (Type *t, uint64_t offset, const DataLayout &dl,
                SmallVectorImpl<Leaf> &out)
{
	if (is_padding_type (t))
		return;

	if (auto *st = dyn_cast<StructType> (t)) {
		const StructLayout *layout = dl.getStructLayout (st);

		for (unsigned i = 0; i < st->getNumElements (); ++i)
			collect_leaves (st->getElementType (i),
			                offset + layout->getElementOffset (i), dl, out);
		return;
	}
	if (auto *at = dyn_cast<ArrayType> (t)) {
		Type *element = at->getElementType ();
		uint64_t stride = dl.getTypeAllocSize (element);

		for (uint64_t i = 0; i < at->getNumElements (); ++i)
			collect_leaves (element, offset + i * stride, dl, out);
		return;
	}

	out.push_back ({ offset, dl.getTypeStoreSize (t), t->isFloatingPointTy () });
}

/// Rounds \p n up to the next power of two, the width a register load moves
/// at.
unsigned
pow2_width (uint64_t n)
{
	unsigned width = 1;

	while (width < n)
		width <<= 1;
	return width;
}

AggShape
classify_aggregate (Type *t, const DataLayout &dl)
{
	AggShape shape;
	uint64_t size = dl.getTypeAllocSize (t);
	uint64_t aligned = alignTo (size, 8);

	if (size == 0)
		return shape;

	if (size > 16) {
		shape.memory = true;
		return shape;
	}

	SmallVector<Leaf, 8> leaves;
	collect_leaves (t, 0, dl, leaves);

	for (const Leaf &leaf : leaves)
		if (leaf.offset < 8 && leaf.offset + leaf.size > 8) {
			/*
			 * mini refuses to marshal a straddling field (NOT_IMPLEMENTED in
			 * add_valuetype), so no call like this has two working ends to
			 * agree with.
			 */
			report_fatal_error ("mono: a field of a native by-value struct "
			                    "straddles an eightbyte, which the runtime "
			                    "does not marshal");
		}

	/* A type whose bytes are all padding travels as nothing at all. */
	if (leaves.empty ())
		return shape;

	shape.nquads = aligned > 8 ? 2 : 1;

	for (unsigned quad = 0; quad < shape.nquads; ++quad) {
		for (const Leaf &leaf : leaves) {
			if (quad == 0 && leaf.offset >= 8)
				continue;
			if (quad == 1 && leaf.offset < 8)
				continue;

			shape.qsize[quad] = (unsigned) (leaf.offset + leaf.size - quad * 8);

			Quad cls = leaf.sse ? Quad::Sse : Quad::Integer;
			if (shape.cls[quad] == Quad::None)
				shape.cls[quad] = cls;
			else if (shape.cls[quad] != cls)
				shape.cls[quad] = Quad::Integer;
		}

		shape.qsize[quad] = pow2_width (shape.qsize[quad]);
		assert (shape.qsize[quad] <= 8);
	}

	return shape;
}

/// Returns the IR type \p shape's register words travel as. LLVM's own lowering
/// of a first-class struct argument passes each element separately, which is
/// exactly what puts one word per register.
Type *
travel_type (LLVMContext &ctx, const AggShape &shape)
{
	SmallVector<Type *, 2> words;

	for (unsigned quad = 0; quad < shape.nquads; ++quad) {
		switch (shape.cls[quad]) {
		case Quad::None:
			break;
		case Quad::Integer:
			words.push_back (Type::getIntNTy (ctx, shape.qsize[quad] * 8));
			break;
		case Quad::Sse:
			words.push_back (shape.qsize[quad] <= 4
			                         ? Type::getFloatTy (ctx)
			                         : Type::getDoubleTy (ctx));
			break;
		}
	}

	if (words.empty ())
		return StructType::get (ctx);
	if (words.size () == 1)
		return words[0];
	return StructType::get (ctx, { words[0], words[1] });
}

/// How one argument crosses the boundary.
struct ParamLowering {
	enum Kind {
		Direct,  ///< the natural type itself: scalars, pointers, vectors
		Coerced, ///< a value type travelling as register-sized words
		Memory,  ///< a value type copied onto the stack: a byval pointer
	} kind = Direct;
	Type *travel = nullptr; ///< the lowered parameter type when Coerced
};

/// A whole call's lowering against the C convention.
struct CallLowering {
	SmallVector<ParamLowering, 8> params; ///< one per natural parameter
	/// A value-type return travelling in registers travels as this.
	Type *ret_travel = nullptr;
	bool ret_by_address = false;
};

bool
is_aggregate (Type *t)
{
	return t->isStructTy () || t->isArrayTy ();
}

CallLowering
compute_lowering (FunctionType *type, function_ref<bool (unsigned)> is_nest,
                  const DataLayout &dl, LLVMContext &ctx)
{
	constexpr unsigned param_gregs = 6, param_fregs = 8;

	CallLowering low;
	unsigned gr = 0, fr = 0;

	Type *ret = type->getReturnType ();

	if (is_aggregate (ret)) {
		AggShape shape = classify_aggregate (ret, dl);

		if (!shape.memory) {
			low.ret_travel = travel_type (ctx, shape);
		} else {
			/*
			 * The hidden pointer is spelled out, in front of everything,
			 * rather than left to LLVM. Left alone, LLVM does not demote a
			 * memory-class aggregate at all. Three doubles come back in
			 * XMM0, XMM1 and ST0, which no C callee writes. The first
			 * argument then takes the register the hidden pointer was
			 * owed.
			 */
			low.ret_by_address = true;
			if (gr < param_gregs)
				gr++;
		}
	}

	for (unsigned i = 0; i < type->getNumParams (); ++i) {
		Type *t = type->getParamType (i);
		ParamLowering p;

		/* The nest key rides its own register, outside the convention. */
		if (is_nest (i)) {
			low.params.push_back (p);
			continue;
		}

		if (is_aggregate (t)) {
			AggShape shape = classify_aggregate (t, dl);
			unsigned need_gr = 0, need_fr = 0;

			for (unsigned quad = 0; quad < shape.nquads; ++quad) {
				if (shape.cls[quad] == Quad::Integer)
					need_gr++;
				else if (shape.cls[quad] == Quad::Sse)
					need_fr++;
			}

			/*
			 * A value type that does not fit skips counting its
			 * registers, so they stay free for later arguments, matching
			 * the rewind mini's own classifier performs.
			 */
			if (shape.memory || gr + need_gr > param_gregs
			    || fr + need_fr > param_fregs) {
				p.kind = ParamLowering::Memory;
			} else {
				gr += need_gr;
				fr += need_fr;
				p.kind = ParamLowering::Coerced;
				p.travel = travel_type (ctx, shape);
			}
		} else if (t->isFloatingPointTy () || t->isVectorTy ()) {
			if (fr < param_fregs)
				fr++;
		} else {
			if (gr < param_gregs)
				gr++;
		}

		low.params.push_back (p);
	}

	return low;
}

/// Whether \p low leaves this frame out of the call it describes, which is what
/// lets the call hand the frame away:
///
///   - a Memory argument travels through memory of this frame, which is the one
///     thing a tail call promises the callee never sees;
///   - a return that travels - through a hidden pointer or as register words - is
///     read back after the call, so there is no longer a call the ret follows.
///
/// A Coerced argument is none of that. Its spill is loaded before the call,
/// and what crosses is the loaded word, so the alloca is dead by the time the
/// frame goes.
bool
frame_stays_out_of_the_call (const CallLowering &low)
{
	if (low.ret_by_address || low.ret_travel != nullptr)
		return false;

	for (const ParamLowering &p : low.params)
		if (p.kind == ParamLowering::Memory)
			return false;

	return true;
}

/// Returns the tail-call kind the lowered site can carry over from \p call,
/// which crossed the boundary as \p low describes.
///
/// A refusal to be a jump carries as it is. It constrains nothing. A
/// permission to be one survives only where the lowering left this frame out
/// of the call.
///
/// And only ever as the permission. The lowering rebuilds the argument list,
/// so the site no longer has the caller's own prototype, and musttail
/// demands exactly that.
CallInst::TailCallKind
carried_tail_kind (const CallInst *call, const CallLowering &low)
{
	CallInst::TailCallKind kind = call->getTailCallKind ();

	if (kind != CallInst::TCK_Tail && kind != CallInst::TCK_MustTail)
		return kind;

	return frame_stays_out_of_the_call (low) ? CallInst::TCK_Tail : CallInst::TCK_None;
}

/// Creates a block of \p invoke's own on its normal edge, so a value read
/// back after the call has somewhere to sit ahead of every use of it.
///
/// The normal destination is no good on its own. A PHI there comes before
/// anything else the block can hold. A PHI taking the call's result is
/// exactly what the optimizer leaves behind when the value flows into a
/// loop. A read-back placed directly in the destination comes after that
/// PHI, not before it, so the PHI cannot use it.
BasicBlock *
split_normal_edge (InvokeInst *invoke)
{
	BasicBlock *from = invoke->getParent ();
	BasicBlock *to = invoke->getNormalDest ();
	BasicBlock *edge =
		BasicBlock::Create (to->getContext (), to->getName () + ".readback",
		                    from->getParent (), to);

	BranchInst::Create (to, edge);
	invoke->setNormalDest (edge);
	to->replacePhiUsesWith (from, edge);
	return edge;
}

void
rewrite_call (CallBase *call)
{
	Function *fn = call->getFunction ();
	Module *m = fn->getParent ();
	const DataLayout &dl = m->getDataLayout ();
	LLVMContext &ctx = m->getContext ();
	FunctionType *old_type = call->getFunctionType ();

	/*
	 * The lowering below puts the hidden return pointer in first. A site that
	 * arrives with one already spelled out has been lowered once, and doing it
	 * twice gives the callee a pointer to a pointer.
	 */
	if (call->hasStructRetAttr ())
		report_fatal_error ("mono: a call marked for the C convention already "
		                    "carries a hidden return pointer");

	CallLowering low = compute_lowering (
		old_type,
		[&] (unsigned i) { return call->paramHasAttr (i, Attribute::Nest); }, dl,
		ctx);

	IRBuilder<> entry (&fn->getEntryBlock (), fn->getEntryBlock ().begin ());
	IRBuilder<> b (call);

	AttributeList old_attrs = call->getAttributes ();
	SmallVector<Value *, 8> args;
	SmallVector<Type *, 8> types;
	SmallVector<AttributeSet, 8> attrs;

	for (unsigned i = 0; i < old_type->getNumParams (); ++i) {
		Value *v = call->getArgOperand (i);
		const ParamLowering &p = low.params[i];

		switch (p.kind) {
		case ParamLowering::Direct:
			args.push_back (v);
			types.push_back (v->getType ());
			attrs.push_back (old_attrs.getParamAttrs (i));
			break;
		case ParamLowering::Coerced: {
			if (dl.getTypeStoreSize (p.travel) == 0) {
				args.push_back (PoisonValue::get (p.travel));
				types.push_back (p.travel);
				attrs.push_back (AttributeSet ());
				break;
			}

			/*
			 * The register words load from a spill of the value. A
			 * 12-byte type, for instance, travels as two full words -
			 * wider than the value itself - so the spill is sized to the
			 * travel type. The bytes past the value are as undefined as
			 * the register bits mini leaves unwritten.
			 */
			AllocaInst *slot = entry.CreateAlloca (p.travel);

			slot->setAlignment (Align (8));
			b.CreateAlignedStore (v, slot, Align (8));
			args.push_back (b.CreateAlignedLoad (p.travel, slot, Align (8)));
			types.push_back (p.travel);
			attrs.push_back (AttributeSet ());
			break;
		}
		case ParamLowering::Memory: {
			/*
			 * byval makes LLVM place the pointee itself in the outgoing
			 * argument area. Without it the pointer rides a register, and
			 * the callee reads the argument area, which holds other data.
			 * Alignment 8 matches mini's argument slots.
			 */
			AllocaInst *slot = entry.CreateAlloca (v->getType ());

			slot->setAlignment (Align (8));
			b.CreateAlignedStore (v, slot, Align (8));

			AttrBuilder ab (ctx);

			ab.addByValAttr (v->getType ());
			ab.addAlignmentAttr (8);
			args.push_back (slot);
			types.push_back (PointerType::get (ctx, 0));
			attrs.push_back (AttributeSet::get (ctx, ab));
			break;
		}
		}
	}

	AllocaInst *ret_slot = nullptr;
	Type *ret_type = old_type->getReturnType ();

	if (low.ret_by_address) {
		ret_slot = entry.CreateAlloca (ret_type);
		ret_slot->setAlignment (Align (8));
		args.insert (args.begin (), ret_slot);
		types.insert (types.begin (), PointerType::get (ctx, 0));
		attrs.insert (attrs.begin (), AttributeSet ());
		ret_type = Type::getVoidTy (ctx);
	} else if (low.ret_travel != nullptr) {
		ret_type = low.ret_travel;
	}

	FunctionType *new_type =
		FunctionType::get (ret_type, types, old_type->isVarArg ());

	AttrBuilder fn_attrs (ctx, old_attrs.getFnAttrs ());

	fn_attrs.removeAttribute (mono_cc_attribute);

	AttributeList new_attrs = AttributeList::get (
		ctx, AttributeSet::get (ctx, fn_attrs),
		ret_type == old_type->getReturnType () ? old_attrs.getRetAttrs ()
		                                       : AttributeSet (),
		attrs);

	SmallVector<OperandBundleDef, 2> bundles;
	call->getOperandBundlesAsDefs (bundles);

	CallBase *lowered;

	if (auto *invoke = dyn_cast<InvokeInst> (call))
		lowered = b.CreateInvoke (new_type, call->getCalledOperand (),
		                          invoke->getNormalDest (),
		                          invoke->getUnwindDest (), args, bundles);
	else
		lowered = b.CreateCall (new_type, call->getCalledOperand (), args,
		                        bundles);

	lowered->setAttributes (new_attrs);
	lowered->setCallingConv (CallingConv::C);

	if (auto *plain = dyn_cast<CallInst> (call))
		cast<CallInst> (lowered)->setTailCallKind (
			carried_tail_kind (plain, low));

	Value *result = lowered;

	if (low.ret_by_address || low.ret_travel != nullptr) {
		/*
		 * The natural value reads back after the call. For an invoke, that
		 * happens in a block of the normal edge's own - the only edge the
		 * result was ever usable on.
		 */
		IRBuilder<> after (ctx);

		if (auto *invoke = dyn_cast<InvokeInst> (lowered)) {
			BasicBlock *edge = split_normal_edge (invoke);

			after.SetInsertPoint (edge, edge->begin ());
		} else
			after.SetInsertPoint (lowered->getParent (),
			                      std::next (lowered->getIterator ()));

		if (low.ret_by_address) {
			result = after.CreateAlignedLoad (old_type->getReturnType (),
			                                  ret_slot, Align (8));
		} else if (dl.getTypeStoreSize (low.ret_travel) == 0) {
			result = PoisonValue::get (old_type->getReturnType ());
		} else {
			AllocaInst *slot = entry.CreateAlloca (low.ret_travel);

			slot->setAlignment (Align (8));
			after.CreateAlignedStore (lowered, slot, Align (8));
			result = after.CreateAlignedLoad (old_type->getReturnType (),
			                                  slot, Align (8));
		}
	}

	call->replaceAllUsesWith (result);
	call->eraseFromParent ();
}

} // namespace

PreservedAnalyses
MonoAbiPass::run (Module &m, ModuleAnalysisManager &)
{
	SmallVector<CallBase *, 8> marked;

	for (Function &f : m)
		for (BasicBlock &bb : f)
			for (Instruction &i : bb) {
				auto *call = dyn_cast<CallBase> (&i);

				if (call == nullptr)
					continue;

				/* Reads the call site, then the callee's declaration. */
				if (call->getFnAttr (mono_cc_attribute).isValid ())
					marked.push_back (call);
			}

	if (marked.empty ())
		return PreservedAnalyses::all ();

	for (CallBase *call : marked)
		rewrite_call (call);

	return PreservedAnalyses::none ();
}

Function *
create_mono_entry_thunk (Module &m, StringRef name, Function *target, Value *through)
{
	LLVMContext &ctx = m.getContext ();
	const DataLayout &dl = m.getDataLayout ();
	AttributeList target_attrs = target->getAttributes ();

	/*
	 * The two conventions spell a return too wide for the registers
	 * differently. The target names the pointer as an explicit parameter,
	 * and C leaves it to the `sret` lowering. So the lowering is computed
	 * from the signature both ends were derived from, and the bridging
	 * happens at the call.
	 */
	Type *hidden = hidden_return_type (target);
	FunctionType *natural = hidden != nullptr
	                                ? natural_prototype (target->getFunctionType (), hidden)
	                                : target->getFunctionType ();
	/* Natural argument i's parameter on the target, which is what carries its attributes. */
	auto target_param = [&] (unsigned i) {
		return target_attrs.getParamAttrs (natural_parameter_index (i, target));
	};

	CallLowering low =
		compute_lowering (natural, [] (unsigned) { return false; }, dl, ctx);

	SmallVector<Type *, 8> types;
	SmallVector<AttributeSet, 8> attrs;

	for (unsigned i = 0; i < natural->getNumParams (); ++i) {
		const ParamLowering &p = low.params[i];

		switch (p.kind) {
		case ParamLowering::Direct:
			types.push_back (natural->getParamType (i));
			attrs.push_back (target_param (i));
			break;
		case ParamLowering::Coerced:
			types.push_back (p.travel);
			attrs.push_back (AttributeSet ());
			break;
		case ParamLowering::Memory: {
			AttrBuilder ab (ctx);

			ab.addByValAttr (natural->getParamType (i));
			ab.addAlignmentAttr (8);
			types.push_back (PointerType::get (ctx, 0));
			attrs.push_back (AttributeSet::get (ctx, ab));
			break;
		}
		}
	}

	Type *ret_type = natural->getReturnType ();

	if (low.ret_by_address) {
		types.insert (types.begin (), PointerType::get (ctx, 0));
		attrs.insert (attrs.begin (), AttributeSet ());
		ret_type = Type::getVoidTy (ctx);
	} else if (low.ret_travel != nullptr) {
		ret_type = low.ret_travel;
	}

	Function *thunk =
		Function::Create (FunctionType::get (ret_type, types, false),
		                  GlobalValue::ExternalLinkage, name, m);

	thunk->setAttributes (AttributeList::get (
		ctx, AttributeSet (),
		ret_type == natural->getReturnType () ? target_attrs.getRetAttrs ()
		                                      : AttributeSet (),
		attrs));
	/*
	 * This frame needs unwind info like any other: an exception thrown below
	 * the call unwinds through it on its way back to whoever entered here.
	 */
	thunk->setUWTableKind (UWTableKind::Default);

	BasicBlock *bb = BasicBlock::Create (ctx, "entry", thunk);
	IRBuilder<> b (bb);

	SmallVector<Value *, 8> args;

	for (unsigned i = 0, at = 0; i < natural->getNumParams (); ++i, ++at) {
		if (low.ret_by_address && at == 0)
			++at;

		Argument *incoming = thunk->getArg (at);
		const ParamLowering &p = low.params[i];
		Type *want = natural->getParamType (i);

		switch (p.kind) {
		case ParamLowering::Direct:
			args.push_back (incoming);
			break;
		case ParamLowering::Coerced: {
			if (dl.getTypeStoreSize (p.travel) == 0) {
				args.push_back (PoisonValue::get (want));
				break;
			}

			AllocaInst *slot = b.CreateAlloca (p.travel);

			slot->setAlignment (Align (8));
			b.CreateAlignedStore (incoming, slot, Align (8));
			args.push_back (b.CreateAlignedLoad (want, slot, Align (8)));
			break;
		}
		case ParamLowering::Memory:
			args.push_back (b.CreateAlignedLoad (want, incoming, Align (8)));
			break;
		}
	}

	/*
	 * A slot of the thunk's own, rather than whatever pointer this
	 * convention was handed. The value type's own alignment is what the
	 * target will store at, and the only thing known about the caller's
	 * slot is where it is.
	 */
	AllocaInst *returned = nullptr;
	unsigned target_vret = hidden_return_index (args.size () + 1);

	if (hidden != nullptr) {
		returned = b.CreateAlloca (hidden);
		returned->setAlignment (Align (8));
		args.insert (args.begin () + target_vret, returned);
	}

	CallInst *call = b.CreateCall (target->getFunctionType (),
	                               through != nullptr ? through : target, args);

	call->setCallingConv (target->getCallingConv ());

	SmallVector<AttributeSet, 8> call_attrs;

	for (unsigned i = 0; i < natural->getNumParams (); ++i)
		call_attrs.push_back (target_param (i));
	if (hidden != nullptr)
		call_attrs.insert (call_attrs.begin () + target_vret,
		                   hidden_return_attributes (ctx, hidden));
	call->setAttributes (AttributeList::get (ctx, AttributeSet (),
	                                         target_attrs.getRetAttrs (),
	                                         call_attrs));

	/*
	 * The thunk adapts a convention and does nothing else, so it hands its
	 * frame to the body. A frame of its own is observable: it stands
	 * between the caller and the method in the stack trace of a method the
	 * runtime entered through it.
	 *
	 * A hidden return is read back out of a slot of this frame after the
	 * call, so that shape keeps its frame.
	 */
	if (returned == nullptr && frame_stays_out_of_the_call (low))
		call->setTailCallKind (CallInst::TCK_Tail);

	Value *result = returned != nullptr
	                        ? b.CreateAlignedLoad (natural->getReturnType (), returned,
	                                               Align (8))
	                        : static_cast<Value *> (call);

	if (low.ret_by_address) {
		/*
		 * The caller's slot is only as aligned as the value type itself
		 * asks. Claim nothing stronger.
		 */
		b.CreateAlignedStore (result, thunk->getArg (0), Align (1));
		b.CreateRetVoid ();
	} else if (low.ret_travel != nullptr) {
		if (dl.getTypeStoreSize (low.ret_travel) == 0) {
			b.CreateRet (PoisonValue::get (low.ret_travel));
		} else {
			AllocaInst *slot = b.CreateAlloca (low.ret_travel);

			slot->setAlignment (Align (8));
			b.CreateAlignedStore (result, slot, Align (8));
			b.CreateRet (
				b.CreateAlignedLoad (low.ret_travel, slot, Align (8)));
		}
	} else if (ret_type->isVoidTy ()) {
		b.CreateRetVoid ();
	} else {
		b.CreateRet (result);
	}

	return thunk;
}

} // namespace mono::arch

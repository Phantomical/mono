/**
 * \file
 * \brief Lowering calls that cross into code compiled by mini.
 *
 * Generated code speaks the C convention with every value in its natural IR
 * type; only the boundary with the rest of the runtime - code mini compiled,
 * raw C entry points, and every function pointer the runtime hands out - still
 * speaks mini's convention, which is a different classification of the same
 * arguments rather than a different tag. The translator marks those calls with the
 * `mono-legacycc` attribute and emits them naturally; LegacyAbiPass rewrites
 * them into the legacy convention after the optimization pipeline has run, so
 * nothing upstream of it ever sees a lowered call.
 *
 * The convention restated here is mini's (get_call_info / add_valuetype,
 * mini-amd64.c), classified from IR types and the DataLayout:
 *
 *   - a managed value type of up to 16 bytes travels as one or two integer
 *     register words (never float registers, unlike the C ABI);
 *   - one whose fields straddle an 8-byte boundary, or that is bigger than 16
 *     bytes, or that arrives after the integer argument registers have run
 *     out, is copied onto the stack instead - a byval pointer here;
 *   - a managed return of up to 8 bytes comes back in RAX; anything bigger
 *     travels through a pointer the caller passes, whose position is the
 *     flavor's business - a hidden argument LLVM inserted on its own would sit
 *     in front of `this`, which the runtime's trampolines insist on finding in
 *     the first register;
 *   - a native (pinvoke) signature classifies each word as integer or float
 *     the way the C ABI does, since the other side is C.
 *
 * The classification recurses to leaf fields exactly as mini's
 * collect_field_info_nested does, which is why it agrees with mini despite
 * never seeing the metadata: the translator emits the same layout mini reads,
 * with padding spelled the one way (layout.hpp) that keeps it distinguishable
 * from data.
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

StringRef
legacy_flavor_value (LegacyFlavor flavor)
{
	switch (flavor) {
	case LegacyFlavor::Managed:
		return "managed";
	case LegacyFlavor::ManagedVret1:
		return "managed-vret1";
	case LegacyFlavor::Pinvoke:
		return "pinvoke";
	}
	llvm_unreachable ("unhandled flavor");
}

namespace {

LegacyFlavor
parse_flavor (StringRef value)
{
	if (value == "managed")
		return LegacyFlavor::Managed;
	if (value == "managed-vret1")
		return LegacyFlavor::ManagedVret1;
	if (value == "pinvoke")
		return LegacyFlavor::Pinvoke;
	report_fatal_error ("mono: unrecognized mono-legacycc flavor '" + value + "'");
}

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

/// Every leaf field of T with its flattened offset - mini's
/// collect_field_info_nested, walked over the IR layout. What the translator
/// spelled as padding (layout.hpp) is no field of mini's walk either.
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

/// Register loads come in power-of-two widths.
unsigned
pow2_width (uint64_t n)
{
	unsigned width = 1;

	while (width < n)
		width <<= 1;
	return width;
}

AggShape
classify_aggregate (Type *t, const DataLayout &dl, bool pinvoke, bool is_return)
{
	AggShape shape;
	uint64_t size = dl.getTypeAllocSize (t);
	uint64_t aligned = alignTo (size, 8);

	if (size == 0)
		return shape;

	bool in_registers = pinvoke ? size <= 16
	                            : (is_return ? aligned == 8 : aligned <= 16);

	if (!in_registers) {
		shape.memory = true;
		return shape;
	}

	SmallVector<Leaf, 8> leaves;
	collect_leaves (t, 0, dl, leaves);

	for (const Leaf &leaf : leaves)
		if (leaf.offset < 8 && leaf.offset + leaf.size > 8) {
			/*
			 * mini refuses to marshal a native straddling field
			 * (NOT_IMPLEMENTED in add_valuetype), so no call like this has
			 * two working ends to agree with.
			 */
			if (pinvoke)
				report_fatal_error ("mono: a field of a native by-value "
				                    "struct straddles an eightbyte, which "
				                    "the runtime does not marshal");
			shape.memory = true;
			return shape;
		}

	/* A native type whose bytes are all padding travels as nothing at all. */
	if (pinvoke && leaves.empty ())
		return shape;

	shape.nquads = aligned > 8 ? 2 : 1;

	if (!pinvoke) {
		/* Managed data always rides the integer file, floats included. */
		shape.cls[0] = Quad::Integer;
		shape.qsize[0] = pow2_width (size > 8 ? 8 : size);
		if (shape.nquads == 2) {
			shape.cls[1] = Quad::Integer;
			shape.qsize[1] = 8;
		}
	} else {
		for (unsigned quad = 0; quad < shape.nquads; ++quad) {
			for (const Leaf &leaf : leaves) {
				if (quad == 0 && leaf.offset >= 8)
					continue;
				if (quad == 1 && leaf.offset < 8)
					continue;

				shape.qsize[quad] =
					(unsigned) (leaf.offset + leaf.size - quad * 8);

				Quad cls = leaf.sse ? Quad::Sse : Quad::Integer;
				if (shape.cls[quad] == Quad::None)
					shape.cls[quad] = cls;
				else if (shape.cls[quad] != cls)
					shape.cls[quad] = Quad::Integer;
			}

			shape.qsize[quad] = pow2_width (shape.qsize[quad]);
			assert (shape.qsize[quad] <= 8);
		}
	}

	return shape;
}

/// The IR type SHAPE's register words travel as. LLVM's own lowering of a
/// first-class struct argument passes each element separately, which is
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

/// A whole call's lowering against mini's convention.
struct CallLowering {
	SmallVector<ParamLowering, 8> params; ///< one per natural parameter
	/// A value-type return travelling in registers travels as this.
	Type *ret_travel = nullptr;
	bool ret_by_address = false;
	unsigned vret_index = 0; ///< the hidden pointer's lowered position
};

bool
is_aggregate (Type *t)
{
	return t->isStructTy () || t->isArrayTy ();
}

CallLowering
compute_lowering (FunctionType *type, function_ref<bool (unsigned)> is_nest,
                  LegacyFlavor flavor, const DataLayout &dl, LLVMContext &ctx)
{
	constexpr unsigned param_gregs = 6, param_fregs = 8;

	bool pinvoke = flavor == LegacyFlavor::Pinvoke;
	CallLowering low;
	unsigned gr = 0, fr = 0;

	Type *ret = type->getReturnType ();

	if (is_aggregate (ret)) {
		AggShape shape = classify_aggregate (ret, dl, pinvoke, true);

		if (!shape.memory) {
			low.ret_travel = travel_type (ctx, shape);
		} else {
			/*
			 * The hidden pointer is spelled out rather than left to LLVM,
			 * for native returns as much as managed ones. Left alone LLVM
			 * does not demote a memory-class aggregate at all: three
			 * doubles come back in XMM0, XMM1 and ST0, which no C callee
			 * writes, and the argument that should have followed the
			 * pointer takes the register the pointer was owed.
			 *
			 * Its position is the flavor's business. A C function keeps it
			 * in front of everything; a managed one puts it behind a
			 * receiver the trampolines insist on finding first. A nest key
			 * is a trailing parameter and no argument at all, so it never
			 * comes between them.
			 */
			low.ret_by_address = true;
			low.vret_index = flavor == LegacyFlavor::ManagedVret1 ? 1 : 0;
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
			AggShape shape = classify_aggregate (t, dl, pinvoke, false);
			unsigned need_gr = 0, need_fr = 0;

			for (unsigned quad = 0; quad < shape.nquads; ++quad) {
				if (shape.cls[quad] == Quad::Integer)
					need_gr++;
				else if (shape.cls[quad] == Quad::Sse)
					need_fr++;
			}

			/*
			 * A value type that no longer fits leaves the registers it
			 * would have taken free for later arguments, exactly as mini
			 * rewinds its counters.
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

/// The tail-call kind the lowered site may carry over from CALL, which crossed the
/// boundary as LOW describes.
///
/// A refusal to be a jump carries as it is; it constrains nothing. A permission to
/// be one survives only where the lowering left this frame out of the call:
///
///   - a Memory argument is a pointer into an alloca of this frame, which is the one
///     thing a tail call promises the callee never sees;
///   - a return that travels - through a hidden pointer or as register words - is
///     read back after the call, so there is no longer a call the ret follows.
///
/// A Coerced argument is none of that: its spill is loaded before the call and what
/// crosses is the loaded word, so the alloca is dead by the time the frame goes.
///
/// And only ever as the permission - the lowering rebuilds the argument list, so the
/// site no longer has the caller's own prototype, and musttail demands exactly that.
CallInst::TailCallKind
carried_tail_kind (const CallInst *call, const CallLowering &low)
{
	CallInst::TailCallKind kind = call->getTailCallKind ();

	if (kind != CallInst::TCK_Tail && kind != CallInst::TCK_MustTail)
		return kind;

	if (low.ret_by_address || low.ret_travel != nullptr)
		return CallInst::TCK_None;

	for (const ParamLowering &p : low.params)
		if (p.kind == ParamLowering::Memory)
			return CallInst::TCK_None;

	return CallInst::TCK_Tail;
}

/// A block of INVOKE's own on its normal edge, so a value read back after the
/// call has somewhere to sit ahead of every use of it.
///
/// The normal destination is no good on its own: a PHI there comes before
/// anything else the block can hold, and a PHI taking the call's result is
/// exactly what the optimizer leaves behind when the value flows into a loop.
/// Inserting the read-back after it would name it before it is defined.
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
rewrite_call (CallBase *call, LegacyFlavor flavor)
{
	Function *fn = call->getFunction ();
	Module *m = fn->getParent ();
	const DataLayout &dl = m->getDataLayout ();
	LLVMContext &ctx = m->getContext ();
	FunctionType *old_type = call->getFunctionType ();

	/*
	 * This convention's own hidden return pointer goes in a place the runtime's
	 * trampolines fixed, so a site reaching here has to still be in the
	 * signature's terms for the lowering below to put it there.
	 */
	if (call->hasStructRetAttr ())
		report_fatal_error ("mono: a call marked for the legacy convention already "
		                    "carries a hidden return pointer");

	CallLowering low = compute_lowering (
		old_type,
		[&] (unsigned i) { return call->paramHasAttr (i, Attribute::Nest); },
		flavor, dl, ctx);

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
			 * The register words load from a spill of the value: the words
			 * can be wider than the value itself (a 12-byte type travels
			 * as two full ones), so the spill is sized for the travel
			 * type, and the bytes past the value are as undefined as the
			 * register bits mini leaves unwritten.
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
			 * byval is what makes LLVM place the pointee itself in the
			 * outgoing argument area; without it the pointer would ride a
			 * register and the callee would read the wrong memory.
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
		args.insert (args.begin () + low.vret_index, ret_slot);
		types.insert (types.begin () + low.vret_index,
		              PointerType::get (ctx, 0));
		attrs.insert (attrs.begin () + low.vret_index, AttributeSet ());
		ret_type = Type::getVoidTy (ctx);
	} else if (low.ret_travel != nullptr) {
		ret_type = low.ret_travel;
	}

	FunctionType *new_type =
		FunctionType::get (ret_type, types, old_type->isVarArg ());

	AttrBuilder fn_attrs (ctx, old_attrs.getFnAttrs ());

	fn_attrs.removeAttribute (legacy_cc_attribute);

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
		 * The natural value reads back after the call - for an invoke, in a
		 * block of the normal edge's own, which is the only edge the result
		 * was ever usable on.
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
LegacyAbiPass::run (Module &m, ModuleAnalysisManager &)
{
	SmallVector<std::pair<CallBase *, LegacyFlavor>, 8> marked;

	for (Function &f : m)
		for (BasicBlock &bb : f)
			for (Instruction &i : bb) {
				auto *call = dyn_cast<CallBase> (&i);

				if (call == nullptr)
					continue;

				/* Reads the call site, then the callee's declaration. */
				Attribute attr = call->getFnAttr (legacy_cc_attribute);

				if (!attr.isValid ())
					continue;
				marked.push_back (
					{ call, parse_flavor (attr.getValueAsString ()) });
			}

	if (marked.empty ())
		return PreservedAnalyses::all ();

	for (auto &[call, flavor] : marked)
		rewrite_call (call, flavor);

	return PreservedAnalyses::none ();
}

Function *
create_unbox_entry (Module &m, StringRef name, Function *target, Value *through,
                    unsigned adjust)
{
	LLVMContext &ctx = m.getContext ();
	AttributeList target_attrs = target->getAttributes ();
	SmallVector<AttributeSet, 8> params;

	for (unsigned i = 0; i < target->arg_size (); ++i)
		params.push_back (target_attrs.getParamAttrs (i));

	Function *entry = Function::Create (target->getFunctionType (),
	                                    GlobalValue::ExternalLinkage, name, m);

	/*
	 * The return and the parameters, and nothing of the function itself: the
	 * body's own attributes say things about the body that are false here, and
	 * `mono-has-eh-clauses` is the one that bites - the side channel would then
	 * carry two records for one method and the LSDA could not be attributed.
	 */
	entry->setAttributes (
		AttributeList::get (ctx, AttributeSet (), target_attrs.getRetAttrs (), params));
	entry->setUWTableKind (UWTableKind::Default);

	BasicBlock *bb = BasicBlock::Create (ctx, "entry", entry);
	IRBuilder<> b (bb);
	SmallVector<Value *, 8> args;

	for (Argument &a : entry->args ())
		args.push_back (&a);

	/* The receiver is argument 0 of this convention, whatever else it carries. */
	args[0] = b.CreateConstInBoundsGEP1_64 (b.getInt8Ty (), args[0], adjust);

	CallInst *call = b.CreateCall (target->getFunctionType (), through, args);

	call->setAttributes (
		AttributeList::get (ctx, AttributeSet (), target_attrs.getRetAttrs (), params));
	call->setTailCallKind (CallInst::TCK_MustTail);

	if (call->getType ()->isVoidTy ())
		b.CreateRetVoid ();
	else
		b.CreateRet (call);

	return entry;
}

Function *
create_legacy_entry_thunk (Module &m, StringRef name, Function *target,
                           LegacyFlavor flavor, Value *through)
{

	LLVMContext &ctx = m.getContext ();
	const DataLayout &dl = m.getDataLayout ();
	AttributeList target_attrs = target->getAttributes ();

	/*
	 * The two conventions each have their own answer for a return too wide for the
	 * registers, and they do not agree - this one puts the pointer where the
	 * runtime's trampolines expect it, the target's puts it first. So the lowering
	 * is computed from the signature both ends were derived from, and the bridging
	 * happens at the call.
	 */
	Type *hidden = hidden_return_type (target);
	FunctionType *natural = hidden != nullptr
	                                ? natural_prototype (target->getFunctionType (), hidden)
	                                : target->getFunctionType ();
	/* Natural argument I's parameter on the target, which is what carries its attributes. */
	auto target_param = [&] (unsigned i) {
		return target_attrs.getParamAttrs (natural_parameter_index (i, target));
	};

	CallLowering low =
		compute_lowering (natural, [] (unsigned) { return false; }, flavor,
		                  dl, ctx);

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
		types.insert (types.begin () + low.vret_index,
		              PointerType::get (ctx, 0));
		attrs.insert (attrs.begin () + low.vret_index, AttributeSet ());
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
	 * Every frame needs a description mono's unwinder can walk through: an
	 * exception thrown below the call unwinds through this frame on its way
	 * back to whoever entered here.
	 */
	thunk->setUWTableKind (UWTableKind::Default);

	BasicBlock *bb = BasicBlock::Create (ctx, "entry", thunk);
	IRBuilder<> b (bb);

	SmallVector<Value *, 8> args;

	for (unsigned i = 0, at = 0; i < natural->getNumParams (); ++i, ++at) {
		if (low.ret_by_address && at == low.vret_index)
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
	 * A slot of the thunk's own rather than whatever pointer this convention was
	 * handed: the value type's own alignment is what the target will store at, and
	 * the only thing known about the caller's slot is where it is.
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

	Value *result = returned != nullptr
	                        ? b.CreateAlignedLoad (natural->getReturnType (), returned,
	                                               Align (8))
	                        : static_cast<Value *> (call);

	if (low.ret_by_address) {
		/*
		 * The caller's slot is only as aligned as the value type itself
		 * asks; claim nothing stronger.
		 */
		b.CreateAlignedStore (result, thunk->getArg (low.vret_index), Align (1));
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

Function *
create_fastcc_entry_thunk (Module &m, StringRef name, Function *shape,
                           LegacyFlavor flavor)
{
	LLVMContext &ctx = m.getContext ();
	FunctionType *proto = shape->getFunctionType ();
	AttributeList proto_attrs = shape->getAttributes ();

	/*
	 * The key register is not an argument register, so the callee's address
	 * rides behind the prototype without moving anything in it. Behind rather
	 * than in front because a hidden return pointer is only ever parameter 0 or
	 * 1, and a leading key would push it out of both.
	 */
	SmallVector<Type *, 8> types;
	SmallVector<AttributeSet, 8> attrs;

	for (unsigned i = 0; i < proto->getNumParams (); ++i) {
		types.push_back (proto->getParamType (i));
		attrs.push_back (proto_attrs.getParamAttrs (i));
	}
	types.push_back (PointerType::get (ctx, 0));
	attrs.push_back (AttributeSet::get (ctx, AttrBuilder (ctx).addAttribute (Attribute::Nest)));

	Function *thunk =
		Function::Create (FunctionType::get (proto->getReturnType (), types,
	                                            proto->isVarArg ()),
	                      GlobalValue::ExternalLinkage, name, m);

	thunk->setAttributes (AttributeList::get (ctx, AttributeSet (),
	                                          proto_attrs.getRetAttrs (), attrs));
	/* Its callee can throw, and mono's unwinder walks back out through here. */
	thunk->setUWTableKind (UWTableKind::Default);

	BasicBlock *bb = BasicBlock::Create (ctx, "entry", thunk);
	IRBuilder<> b (bb);

	/*
	 * The call is made in the signature's own terms and marked; LegacyAbiPass
	 * is what turns it into the convention, exactly as it does for a call the
	 * translator emitted.
	 */
	Type *hidden = hidden_return_type (shape);
	FunctionType *natural =
		hidden != nullptr ? natural_prototype (proto, hidden) : proto;

	SmallVector<Value *, 8> args;
	SmallVector<AttributeSet, 8> call_attrs;

	for (unsigned i = 0; i < natural->getNumParams (); ++i) {
		unsigned at = natural_parameter_index (i, shape);

		args.push_back (thunk->getArg (at));
		call_attrs.push_back (proto_attrs.getParamAttrs (at));
	}

	CallInst *call = b.CreateCall (natural, thunk->getArg (thunk->arg_size () - 1), args);

	call->setAttributes (AttributeList::get (ctx, AttributeSet (),
	                                         hidden != nullptr
	                                                 ? AttributeSet ()
	                                                 : proto_attrs.getRetAttrs (),
	                                         call_attrs));
	call->addFnAttr (Attribute::get (ctx, legacy_cc_attribute,
	                                 legacy_flavor_value (flavor)));

	if (hidden != nullptr) {
		/* Claim nothing about the caller's slot but where it is. */
		b.CreateAlignedStore (
			call, thunk->getArg (hidden_return_index (shape->arg_size ())),
			Align (1));
		b.CreateRetVoid ();
	} else if (proto->getReturnType ()->isVoidTy ()) {
		b.CreateRetVoid ();
	} else {
		b.CreateRet (call);
	}

	return thunk;
}

} // namespace mono::arch

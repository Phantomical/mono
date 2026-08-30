/**
 * \file
 * \brief Making a call whose prototype the caller only knows at run time.
 *
 * The convention restated here is the one interp-entry.cpp reads a call out
 * of, in the other direction. arch/amd64/leaf-layout.cpp is the one statement
 * of it both files place a leaf through, and it is LLVM's own lowering of the
 * backend's ccc declarations, not the SysV classification that mini's own
 * dyn-call code is built on.
 *
 * plan_dyn_call () walks the callee's declaration rather than its
 * MonoMethodSignature, for the same reason plan_interp_entry () does: only
 * the declaration says where a hidden return pointer sits, or that the
 * method is a shared body needing a context register this cannot carry.
 *
 * A value type argument or return flattens into the leaves LLVM lowers it
 * into, the same convention an aggregate already crossing the seam incoming
 * uses.
 */

/*
 * Before anything else, so that MonoError is the internal struct the rest of
 * the runtime passes around rather than the opaque public one.
 */
#include "runtime-error.hpp"

#include "arch/amd64/leaf-layout.hpp"
#include "arch/arch.hpp"
#include "hidden-return.hpp"

#include "mini.h"

#include "mono/metadata/class-internals.h"

#include <llvm/IR/Attributes.h>
#include <llvm/IR/DataLayout.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Module.h>

#include <cstring>

using namespace llvm;

extern "C" {
void mono_llvm_dyn_call_thunk (mono::arch::DynCallFrame *frame, void *target);
}

namespace mono::arch {

namespace {

Error
unsupported (const Twine &what)
{
	return createStringError (inconvertibleErrorCode (),
	                          "the dyn call cannot carry " + what);
}

} // namespace

Expected<std::unique_ptr<DynCallPlan>>
plan_dyn_call (Function *shape, MonoMethodSignature *sig)
{
	const DataLayout &dl = shape->getParent ()->getDataLayout ();
	FunctionType *type = shape->getFunctionType ();
	Type *hidden = hidden_return_type (shape);
	unsigned params = type->getNumParams ();
	unsigned hidden_at = hidden != nullptr ? hidden_return_index (placed_parameter_count (shape))
	                                       : params;
	unsigned natural = hidden != nullptr ? params - 1 : params;

	/*
	 * A mismatch here means a trailing nest parameter: the method is a shared
	 * body, and this states no rule for the context register such a call
	 * needs.
	 */
	if (natural != (unsigned) (sig->hasthis + sig->param_count))
		return unsupported ("a prototype the signature does not account for");

	auto plan = std::make_unique<DynCallPlan> ();
	LeafAssigner assign;
	std::vector<DynCallArg> args (natural);

	for (unsigned p = 0; p < params; ++p) {
		if (p == hidden_at) {
			Leaf leaf { 0, type->getParamType (p) };
			ArgPiece piece = assign.place (leaf, dl);

			/*
			 * Only ever parameter 0 or 1, so the integer file cannot have run
			 * out underneath it.
			 */
			if (piece.file != ArgPiece::File::Greg)
				return unsupported ("a hidden return pointer that missed a "
				                    "register");
			plan->ret.hidden_greg = piece.at;
			continue;
		}

		unsigned i = p < hidden_at ? p : p - 1;
		SmallVector<Leaf, 4> leaves;

		if (Error err = flatten (type->getParamType (p), 0, dl, leaves))
			return std::move (err);

		DynCallArg &arg = args[i];

		arg.first_piece = (uint32_t) plan->pieces.size ();
		arg.piece_count = (uint32_t) leaves.size ();
		for (const Leaf &leaf : leaves)
			plan->pieces.push_back (assign.place (leaf, dl));

		if (shape->hasParamAttribute (p, Attribute::SExt))
			arg.extend = DynCallArg::Extend::Sign;
		else if (shape->hasParamAttribute (p, Attribute::ZExt))
			arg.extend = DynCallArg::Extend::Zero;
	}

	plan->args = std::move (args);

	if (hidden != nullptr) {
		plan->ret.kind = ReturnPlan::Kind::Hidden;
	} else {
		Expected<ReturnPlan> ret = place_return (type->getReturnType (), dl);

		if (!ret)
			return ret.takeError ();
		plan->ret = std::move (*ret);
	}

	plan->wants_fp = assign.used_fregs ();
	plan->stack_words = (uint32_t) (assign.stack_bytes () / 8);
	plan->frame_size =
		(uint32_t) (sizeof (DynCallFrame) + assign.stack_bytes ());

	return plan;
}

void
dyn_call (const DynCallPlan &plan, void *target, void **args, void *ret, void *frame)
{
	auto *f = (DynCallFrame *) frame;

	/*
	 * Only the slots the plan names are written. A register the call does not
	 * pass an argument in rides as whatever the frame held, which no callee
	 * reads, and zeroing the rest would cost a memset on every call.
	 */
	f->has_fp = plan.wants_fp;
	f->nstack = plan.stack_words;

	for (size_t i = 0; i < plan.args.size (); ++i) {
		const DynCallArg &arg = plan.args[i];
		const auto *from = (const uint8_t *) args[i];

		/*
		 * signature.cpp's integer_extension () promises the callee its narrow
		 * argument arrives sign- or zero-extended, and this keeps that
		 * promise. No aggregate leaf carries it, so this is always the
		 * argument's one piece.
		 */
		if (arg.extend != DynCallArg::Extend::None) {
			const ArgPiece &piece = plan.pieces[arg.first_piece];
			const uint8_t *src = from + piece.offset;
			uint64_t bits;

			if (arg.extend == DynCallArg::Extend::Sign)
				bits = (uint64_t) (int64_t) (piece.width == 1 ? *(const int8_t *) src
				                                              : *(const int16_t *) src);
			else
				bits = piece.width == 1 ? *(const uint8_t *) src
				                        : *(const uint16_t *) src;

			switch (piece.file) {
			case ArgPiece::File::Greg:
				f->gregs[piece.at] = bits;
				break;
			case ArgPiece::File::Freg:
				memcpy (f->fregs[piece.at], &bits, sizeof (bits));
				break;
			case ArgPiece::File::Stack:
				memcpy ((uint8_t *) f->stack + piece.at, &bits, sizeof (bits));
				break;
			}
			continue;
		}

		for (uint32_t p = 0; p < arg.piece_count; ++p) {
			const ArgPiece &piece = plan.pieces[arg.first_piece + p];
			const uint8_t *src = from + piece.offset;

			switch (piece.file) {
			case ArgPiece::File::Greg:
				memcpy (&f->gregs[piece.at], src, piece.width);
				break;
			case ArgPiece::File::Freg:
				memcpy (f->fregs[piece.at], src, piece.width);
				break;
			case ArgPiece::File::Stack:
				memcpy ((uint8_t *) f->stack + piece.at, src, piece.width);
				break;
			}
		}
	}

	if (plan.ret.kind == ReturnPlan::Kind::Hidden)
		f->gregs[plan.ret.hidden_greg] = (uint64_t) ret;

	mono_llvm_dyn_call_thunk (f, target);

	if (plan.ret.kind == ReturnPlan::Kind::Registers)
		for (const ArgPiece &piece : plan.ret.pieces) {
			const uint8_t *from = piece.file == ArgPiece::File::Freg
			                              ? f->ret_fregs[piece.at]
			                              : (const uint8_t *) &f->ret_gregs[piece.at];

			memcpy ((uint8_t *) ret + piece.offset, from, piece.width);
		}
}

} // namespace mono::arch

#include "arch/amd64/leaf-layout.hpp"

#include <llvm/IR/DerivedTypes.h>
#include <llvm/Support/MathExtras.h>

using namespace llvm;

namespace mono::arch {

namespace {

Error
unrepresentable (const Twine &what)
{
	return createStringError (inconvertibleErrorCode (), what);
}

} // namespace

Error
flatten (Type *t, uint64_t offset, const DataLayout &dl, SmallVectorImpl<Leaf> &out)
{
	if (auto *st = dyn_cast<StructType> (t)) {
		const StructLayout *layout = dl.getStructLayout (st);

		for (unsigned i = 0; i < st->getNumElements (); ++i)
			if (Error err = flatten (st->getElementType (i),
			                         offset + layout->getElementOffset (i), dl, out))
				return err;
		return Error::success ();
	}

	if (auto *at = dyn_cast<ArrayType> (t)) {
		Type *element = at->getElementType ();
		uint64_t stride = dl.getTypeAllocSize (element);

		for (uint64_t i = 0; i < at->getNumElements (); ++i)
			if (Error err = flatten (element, offset + i * stride, dl, out))
				return err;
		return Error::success ();
	}

	if (t->isIntegerTy ()) {
		/*
		 * Wider than a machine word is CC_X86_64_I128's consecutive-register
		 * rule. It is the one place the convention refuses to split a value
		 * between registers and the stack, and flatten () does not model it.
		 */
		if (t->getIntegerBitWidth () > 64)
			return unrepresentable ("an integer wider than a machine word");
	} else if (t->isFloatingPointTy ()) {
		if (!t->isHalfTy () && !t->isFloatTy () && !t->isDoubleTy ())
			return unrepresentable ("an extended-precision float");
	} else if (t->isVectorTy ()) {
#ifdef HOST_WIN32
		/*
		 * The Microsoft convention passes a vector by reference rather than in
		 * a register, and neither direction models an argument that arrives as
		 * a pointer to a copy the caller made.
		 */
		return unrepresentable ("a vector argument");
#else
		// Anything wider rides a YMM or a ZMM, which neither thunk saves.
		if (dl.getTypeSizeInBits (t).getFixedValue () > 128)
			return unrepresentable ("a vector wider than an SSE register");
#endif
	} else if (!t->isPointerTy ()) {
		return unrepresentable ("a value of an unclassifiable type");
	}

	out.push_back ({ offset, t });
	return Error::success ();
}

bool
rides_sse (Type *t)
{
	return t->isFloatingPointTy () || t->isVectorTy ();
}

ArgPiece
LeafAssigner::place (const Leaf &leaf, const DataLayout &dl)
{
	ArgPiece piece;

	piece.offset = (uint32_t) leaf.offset;
	piece.width = (uint8_t) dl.getTypeStoreSize (leaf.type).getFixedValue ();

#ifdef HOST_WIN32
	if (slots_ < param_gregs) {
		bool sse = rides_sse (leaf.type);

		piece.file = sse ? ArgPiece::File::Freg : ArgPiece::File::Greg;
		piece.at = slots_++;
		fregs_ += sse;
		return piece;
	}
#else
	if (rides_sse (leaf.type) && fregs_ < param_fregs) {
		piece.file = ArgPiece::File::Freg;
		piece.at = fregs_++;
		return piece;
	}
	if (!rides_sse (leaf.type) && gregs_ < param_gregs) {
		piece.file = ArgPiece::File::Greg;
		piece.at = gregs_++;
		return piece;
	}
#endif

	uint64_t slot = leaf.type->isVectorTy () ? 16 : 8;

	stack_ = alignTo (stack_, slot);
	piece.file = ArgPiece::File::Stack;
	piece.at = (uint32_t) stack_;
	stack_ += slot;
	return piece;
}

Expected<ReturnPlan>
place_return (Type *ret, const DataLayout &dl)
{
	ReturnPlan plan;

	if (ret->isVoidTy ())
		return plan;

	SmallVector<Leaf, 4> leaves;

	if (Error err = flatten (ret, 0, dl, leaves))
		return std::move (err);

	unsigned gregs = 0, fregs = 0;

	for (const Leaf &leaf : leaves) {
		ArgPiece piece;

		piece.offset = (uint32_t) leaf.offset;
		piece.width = (uint8_t) dl.getTypeStoreSize (leaf.type).getFixedValue ();

		if (rides_sse (leaf.type)) {
			unsigned available =
				leaf.type->isVectorTy () ? ret_vector_fregs : ret_scalar_fregs;

			if (fregs >= available)
				return unrepresentable ("a return with more SSE parts than there "
				                        "are registers for them");
			piece.file = ArgPiece::File::Freg;
			piece.at = fregs++;
		} else {
			if (gregs >= ret_gregs)
				return unrepresentable ("a return with more integer parts than "
				                        "there are registers for them");
			piece.file = ArgPiece::File::Greg;
			piece.at = gregs++;
		}

		plan.pieces.push_back (piece);
	}

	plan.kind = ReturnPlan::Kind::Registers;
	plan.size = (uint32_t) dl.getTypeStoreSize (ret).getFixedValue ();
	return plan;
}

} // namespace mono::arch

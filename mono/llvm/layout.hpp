/**
 * \file
 * \brief The shape a value type takes in IR, as both halves of the JIT read it.
 *
 * A value type converts to a packed struct spelling out its real layout, with
 * every byte no field claims filled in by padding_type (). The translator
 * writes that shape (set_packed_body (), method-to-llvm/signature.cpp), and
 * the C calling convention reads it back when it classifies (collect_leaves (),
 * arch/<target>/mono-abi.cpp). The two agree on which bytes carry data without
 * the calling convention ever seeing the metadata the translator read.
 */

#ifndef MONO_LLVM_LAYOUT_HPP
#define MONO_LLVM_LAYOUT_HPP

#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Type.h>

namespace mono {

/// Returns a type spanning \p bytes of layout no field claims.
///
/// Storage is byte-wide, not bit-wide, because padding is not always dead.
/// An explicit layout can overlap fields. A byte the classifier treats as
/// padding can still be live data the program reads through a different
/// field. A first-class copy of the value has to carry that byte through
/// unchanged. A bit-packed layout cannot: a load reads only the
/// low bit, and the matching store zero-fills the rest.
inline llvm::Type *
padding_type (llvm::LLVMContext &ctx, unsigned bytes)
{
	llvm::Type *filler = llvm::ArrayType::get (llvm::Type::getInt8Ty (ctx), bytes);

	return llvm::StructType::get (ctx, llvm::ArrayRef<llvm::Type *> (filler));
}

/// Whether t is what padding_type () builds, which classification skips over.
inline bool
is_padding_type (llvm::Type *t)
{
	auto *st = llvm::dyn_cast<llvm::StructType> (t);

	// A literal struct, not a named one. Every type a field converts to is
	// a primitive, a pointer, a vector, an array of bytes or a *named*
	// struct. No field produces this exact shape. A name cannot give that
	// promise: LLVM uniques colliding struct names per context instead of
	// refusing them.
	if (st == nullptr || !st->isLiteral () || st->getNumElements () != 1)
		return false;

	auto *filler = llvm::dyn_cast<llvm::ArrayType> (st->getElementType (0));

	return filler != nullptr && filler->getElementType ()->isIntegerTy (8);
}

} // namespace mono

#endif /* MONO_LLVM_LAYOUT_HPP */

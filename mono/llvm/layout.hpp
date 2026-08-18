/**
 * \file
 * \brief The shape a value type takes in IR, as both halves of the JIT read it.
 *
 * A value type converts to a packed struct spelling out its real layout, with
 * every byte no field claims filled in by padding_type (). The translator
 * writes that shape (set_packed_body (), method-to-llvm/signature.cpp) and the
 * C calling convention reads it back when it classifies (collect_leaves (),
 * arch/<target>/mono-abi.cpp), so the two agree on which bytes carry data
 * without the second one ever seeing the metadata the first one read.
 */

#ifndef MONO_LLVM_LAYOUT_HPP
#define MONO_LLVM_LAYOUT_HPP

#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Type.h>

namespace mono {

/// The BYTES of a value type that no field claims.
///
/// Byte-wide storage, because padding is not always dead: an explicit layout
/// can overlap fields, and the arms the packed struct cannot spell out are
/// padding to the classification while still being live data to the program.
/// A first-class copy of the value has to carry them through, which rules out
/// the [n x i1] this used to be - a load of an i1 keeps one bit of the byte
/// and the matching store writes the other seven back as zero.
///
/// The literal struct wrapper is what keeps padding recognizable. Every type a
/// field converts to is a primitive, a pointer, a vector, an array of bytes or
/// a *named* struct, so a literal struct wrapping an array of bytes is a shape
/// no field ever takes - and unlike a name, which LLVM uniques per context and
/// would quietly hand back as `mono.pad.1`, it cannot be taken by accident.
inline llvm::Type *
padding_type (llvm::LLVMContext &ctx, unsigned bytes)
{
	llvm::Type *filler = llvm::ArrayType::get (llvm::Type::getInt8Ty (ctx), bytes);

	return llvm::StructType::get (ctx, llvm::ArrayRef<llvm::Type *> (filler));
}

/// Whether T is what padding_type () builds, which classification skips over.
inline bool
is_padding_type (llvm::Type *t)
{
	auto *st = llvm::dyn_cast<llvm::StructType> (t);

	if (st == nullptr || !st->isLiteral () || st->getNumElements () != 1)
		return false;

	auto *filler = llvm::dyn_cast<llvm::ArrayType> (st->getElementType (0));

	return filler != nullptr && filler->getElementType ()->isIntegerTy (8);
}

} // namespace mono

#endif /* MONO_LLVM_LAYOUT_HPP */

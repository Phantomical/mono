/**
 * \file
 * \brief Flattening an LLVM type into the scalar leaves the convention visits,
 * and where each one lands.
 *
 * interp-entry.cpp reads a call out of this convention and dyn-call.cpp writes
 * one into it. LLVM assigns the same leaves to the same registers in the same
 * order either way, so this is that one statement of it rather than two.
 */

#ifndef MONO_LLVM_ARCH_AMD64_LEAF_LAYOUT_HPP
#define MONO_LLVM_ARCH_AMD64_LEAF_LAYOUT_HPP

#include "arch/amd64/amd64.hpp"

#include <llvm/ADT/SmallVector.h>
#include <llvm/IR/DataLayout.h>
#include <llvm/Support/Error.h>

namespace llvm {
class Type;
} // namespace llvm

namespace mono::arch {

/// A scalar leaf the convention visits on its own, with where it sits within
/// the value it was flattened out of.
struct Leaf {
	uint64_t offset;
	llvm::Type *type;
};

/// Flatten \p t into the scalars LLVM's argument lowering visits, in order.
llvm::Error flatten (llvm::Type *t, uint64_t offset, const llvm::DataLayout &dl,
                     llvm::SmallVectorImpl<Leaf> &out);

/// Whether the convention passes a leaf of this type in the SSE registers.
bool rides_sse (llvm::Type *t);

/// Hands out the places the convention gives each leaf, in the order it visits
/// parameters.
///
/// The Microsoft convention numbers the arguments rather than the registers:
/// leaf n rides register n of whichever file its type wants, and the register
/// of the other file with that number is spent too. So one counter runs the
/// pair, where System V runs a counter for each. The first stack argument
/// there also starts past the shadow space a caller reserves for the four
/// register arguments -- CCInfo.AllocateStack (32) in X86ISelLowering.
class LeafAssigner {
public:
	ArgPiece place (const Leaf &leaf, const llvm::DataLayout &dl);

	/// Whether any leaf placed so far rode an SSE register.
	bool used_fregs () const { return fregs_ > 0; }

	/// Stack bytes the call passes its arguments in, the Microsoft shadow
	/// space included. Always a multiple of 8: place () aligns every stack slot
	/// to a whole register width or wider.
	uint64_t stack_bytes () const { return stack_; }

private:
#ifdef HOST_WIN32
	/// Argument slots spent, and how many of them rode an SSE register.
	unsigned slots_ = 0, fregs_ = 0;
	uint64_t stack_ = shadow_space;
#else
	unsigned gregs_ = 0, fregs_ = 0;
	uint64_t stack_ = 0;
#endif
};

/// Where each leaf of a return value of type \p ret comes back.
llvm::Expected<ReturnPlan> place_return (llvm::Type *ret, const llvm::DataLayout &dl);

} // namespace mono::arch

#endif

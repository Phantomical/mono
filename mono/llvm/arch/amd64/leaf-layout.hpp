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
class LeafAssigner {
public:
	ArgPiece place (const Leaf &leaf, const llvm::DataLayout &dl);

	/// Whether any leaf placed so far rode an SSE register.
	bool used_fregs () const { return fregs_ > 0; }

	/// Stack bytes reserved so far. Always a multiple of 8: place () aligns
	/// every stack slot to a whole register width or wider.
	uint64_t stack_bytes () const { return stack_; }

private:
	static constexpr unsigned param_gregs = 6, param_fregs = 8;

	unsigned gregs_ = 0, fregs_ = 0;
	uint64_t stack_ = 0;
};

/// How many registers of each file a return value's leaves can be spread over.
///
/// A scalar float comes back in XMM0 or XMM1 and nowhere else, so the two SSE
/// counts run out at different points even though they share the register file.
constexpr unsigned ret_gregs = 3, ret_scalar_fregs = 2, ret_vector_fregs = 4;

/// Where each leaf of a return value of type \p ret comes back.
llvm::Expected<ReturnPlan> place_return (llvm::Type *ret, const llvm::DataLayout &dl);

} // namespace mono::arch

#endif

/**
 * \file
 * \brief Recognizing the front end's finally-body stackmap markers from IR.
 */

#ifndef MONO_LLVM_PASSES_FINALLY_MARKER_HPP
#define MONO_LLVM_PASSES_FINALLY_MARKER_HPP

#include "../mono_lsda_format.hpp"

#include <cstdint>

#include <llvm/IR/Constants.h>
#include <llvm/IR/Instruction.h>
#include <llvm/IR/IntrinsicInst.h>
#include <llvm/IR/Intrinsics.h>

namespace mono {

/// Whether i is one of the front end's finally-body markers: an
/// `llvm.experimental.stackmap` call whose id names a finally clause
/// (method-to-llvm/exceptions.cpp, mono_lsda_format.hpp). When it is,
/// *clause and *opening, if given, name which clause and which end.
inline bool
finally_body_marker (const llvm::Instruction &i, std::uint32_t *clause = nullptr,
                     bool *opening = nullptr)
{
	const auto *call = llvm::dyn_cast<llvm::IntrinsicInst> (&i);

	if (call == nullptr || call->getIntrinsicID () != llvm::Intrinsic::experimental_stackmap)
		return false;

	const auto *id = llvm::dyn_cast<llvm::ConstantInt> (call->getArgOperand (0));

	if (id == nullptr)
		return false;

	std::uint64_t value = id->getZExtValue ();
	std::uint64_t base = value & ~MONO_LLVM_FINALLY_STACKMAP_ID_MASK;
	bool is_opening;

	if (base == MONO_LLVM_FINALLY_STACKMAP_ID_BASE)
		is_opening = true;
	else if (base == MONO_LLVM_FINALLY_END_STACKMAP_ID_BASE)
		is_opening = false;
	else
		return false;

	if (clause != nullptr)
		*clause = static_cast<std::uint32_t> (value & MONO_LLVM_FINALLY_STACKMAP_ID_MASK);
	if (opening != nullptr)
		*opening = is_opening;

	return true;
}

} // namespace mono

#endif

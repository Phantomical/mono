/** \file Which IL try region a machine instruction's location lies in. */

#include "try-region.hpp"

#include "../il-line-table.hpp"

#include <llvm/IR/Constants.h>
#include <llvm/IR/DebugInfoMetadata.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Metadata.h>
#include <llvm/Support/ErrorHandling.h>

using namespace llvm;

namespace mono {

TryRegions::TryRegions (const Function &f)
{
	const MDNode *list = f.getMetadata ("mono.clauses");

	if (list == nullptr)
		return;

	clauses_.reserve (list->getNumOperands ());
	for (const MDOperand &operand : list->operands ()) {
		const auto *node = dyn_cast_or_null<MDNode> (operand.get ());

		/* This metadata is emitted and consumed by the same compiler. */
		if (node == nullptr || node->getNumOperands () != 4)
			report_fatal_error ("mono: a !mono.clauses entry is not four words - our own emission or reader is wrong");

		std::uint64_t words[4];

		for (unsigned i = 0; i < 4; ++i) {
			const auto *word =
				mdconst::dyn_extract<ConstantInt> (node->getOperand (i));

			if (word == nullptr)
				report_fatal_error ("mono: a !mono.clauses word is not a constant - our own emission or reader is wrong");

			words[i] = word->getZExtValue ();
		}

		clauses_.push_back ({ (int) words[0], (std::uint32_t) words[2],
		                      (std::uint32_t) words[3] });
	}
}

int
TryRegions::innermost (int il) const
{
	int found = -1;
	std::uint32_t narrowest = 0;

	if (il < 0)
		return -1;

	for (const Clause &clause : clauses_) {
		if ((std::uint32_t) il < clause.try_offset
		    || (std::uint32_t) il - clause.try_offset >= clause.try_len)
			continue;

		if (found < 0 || clause.try_len < narrowest) {
			found = clause.index;
			narrowest = clause.try_len;
		}
	}

	return found;
}

int
TryRegions::il_offset (const DILocation *loc)
{
	if (loc == nullptr)
		return -1;

	while (loc->getInlinedAt () != nullptr)
		loc = loc->getInlinedAt ();

	return (int) loc->getLine () - (int) IL_OFFSET_LINE_BIAS;
}

} // namespace mono

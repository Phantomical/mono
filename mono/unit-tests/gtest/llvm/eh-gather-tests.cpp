/** \file Tests for fault-handler lookup in the EH gatherer. */

#include "passes/eh-gather.hpp"

#include <llvm/ADT/DenseMap.h>
#include <llvm/ADT/SmallVector.h>

#include <gtest/gtest.h>

using namespace llvm;

namespace mono {
namespace test {
namespace {

struct Block {
	llvm::SmallVector<const Block *, 2> successors;

	auto succ_begin () const { return successors.begin (); }
	size_t succ_size () const { return successors.size (); }
};

TEST (EHGather, FindsAChainPastAHandlerTrampoline)
{
	Block invoke;
	Block trampoline;
	Block handler;

	handler.successors.push_back (&trampoline);
	trampoline.successors.push_back (&invoke);

	DenseMap<const Block *, unsigned> chains;
	chains[&invoke] = 7;

	EXPECT_EQ (faulting_handler_chain (&handler, chains), 7u);
}

TEST (EHGather, UsesAChainOnTheHandlerItself)
{
	Block handler;
	DenseMap<const Block *, unsigned> chains;
	chains[&handler] = 3;

	EXPECT_EQ (faulting_handler_chain (&handler, chains), 3u);
}

TEST (EHGather, DoesNotChooseAChainPastAHandlerBranch)
{
	Block chained;
	Block other;
	Block handler;
	handler.successors.push_back (&chained);
	handler.successors.push_back (&other);

	DenseMap<const Block *, unsigned> chains;
	chains[&chained] = 5;

	EXPECT_EQ (faulting_handler_chain (&handler, chains), std::nullopt);
}

TEST (EHGather, StopsAtAHandlerCycle)
{
	Block first;
	Block second;
	first.successors.push_back (&second);
	second.successors.push_back (&first);

	DenseMap<const Block *, unsigned> chains;

	EXPECT_EQ (faulting_handler_chain (&first, chains), std::nullopt);
}

} // namespace
} // namespace test
} // namespace mono

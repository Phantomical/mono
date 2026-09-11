/*
 * Tests for TrapUnreachablePass, which writes the trap an `unreachable` costs
 * into the IR, where FastISel selects it.
 *
 * Pure LLVM. No test here reads a MonoMethod or any other runtime state.
 */

#include "passes/trap-unreachable.hpp"

#include <llvm/AsmParser/Parser.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Intrinsics.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/Support/SourceMgr.h>

#include <gtest/gtest.h>

#include <memory>
#include <string>

using namespace llvm;

namespace mono {
namespace test {
namespace {

/// What every module below declares.
constexpr const char *preamble = "declare void @thrower() noreturn\n"
                                 "declare void @sink()\n";

/// A module the pass has run over.
struct Trapped {
	LLVMContext context;
	std::unique_ptr<Module> module;
	Function *caller = nullptr;
	bool changed = false;

	explicit Trapped (const std::string &ir)
	{
		SMDiagnostic problem;

		module = parseAssemblyString (ir + preamble, problem, context);

		if (module == nullptr) {
			std::string complaint;
			raw_string_ostream out (complaint);

			problem.print ("", out);
			ADD_FAILURE () << complaint;
			return;
		}

		caller = module->getFunction ("caller");

		if (caller == nullptr) {
			ADD_FAILURE () << "the module declares no caller";
			return;
		}

		FunctionAnalysisManager fam;
		PassBuilder pb;

		pb.registerFunctionAnalyses (fam);

		changed = !TrapUnreachablePass ().run (*caller, fam).areAllPreserved ();

		std::string complaint;
		raw_string_ostream out (complaint);

		EXPECT_FALSE (verifyModule (*module, &out)) << complaint;
	}

	/// How many `llvm.trap` calls caller holds.
	unsigned traps () const
	{
		unsigned seen = 0;

		for (const Instruction &at : instructions (*caller))
			if (const auto *call = dyn_cast<CallInst> (&at))
				if (call->getIntrinsicID () == Intrinsic::trap)
					++seen;

		return seen;
	}

	/// How many of caller's `unreachable` terminators a `llvm.trap` stands in
	/// front of.
	unsigned trapped_ends () const
	{
		unsigned seen = 0;

		for (const BasicBlock &block : *caller) {
			const auto *end = dyn_cast<UnreachableInst> (block.getTerminator ());

			if (end == nullptr)
				continue;

			const auto *call = dyn_cast_or_null<CallInst> (end->getPrevNode ());

			if (call != nullptr && call->getIntrinsicID () == Intrinsic::trap)
				++seen;
		}

		return seen;
	}

	/// How many `unreachable` terminators caller holds.
	unsigned ends () const
	{
		unsigned seen = 0;

		for (const BasicBlock &block : *caller)
			if (isa<UnreachableInst> (block.getTerminator ()))
				++seen;

		return seen;
	}
};

TEST (TrapUnreachableTest, WritesATrapInFrontOfAnUnreachable)
{
	Trapped m (R"(
define void @caller() {
entry:
  unreachable
}
)");

	ASSERT_TRUE (m.changed);
	EXPECT_EQ (m.traps (), 1u);
	EXPECT_EQ (m.trapped_ends (), 1u);
}

TEST (TrapUnreachableTest, TrapsAnUnreachableBehindANoreturnCall)
{
	Trapped m (R"(
define void @caller() {
entry:
  call void @thrower()
  unreachable
}
)");

	ASSERT_TRUE (m.changed);
	EXPECT_EQ (m.traps (), 1u);
	EXPECT_EQ (m.trapped_ends (), 1u);
}

TEST (TrapUnreachableTest, WritesATrapIntoEveryUnreachableBlock)
{
	Trapped m (R"(
define void @caller(i1 %c) {
entry:
  br i1 %c, label %left, label %right
left:
  call void @thrower()
  unreachable
right:
  unreachable
}
)");

	ASSERT_TRUE (m.changed);
	ASSERT_EQ (m.ends (), 2u);
	EXPECT_EQ (m.traps (), 2u);
	EXPECT_EQ (m.trapped_ends (), 2u);
}

TEST (TrapUnreachableTest, LeavesABlockThatAlreadyTrapsAlone)
{
	Trapped m (R"(
declare void @llvm.trap()

define void @caller() {
entry:
  call void @llvm.trap()
  unreachable
}
)");

	EXPECT_FALSE (m.changed);
	EXPECT_EQ (m.traps (), 1u);
	EXPECT_EQ (m.trapped_ends (), 1u);
}

TEST (TrapUnreachableTest, LeavesANakedFunctionAlone)
{
	Trapped m (R"(
define void @caller() naked {
entry:
  unreachable
}
)");

	EXPECT_FALSE (m.changed);
	EXPECT_EQ (m.traps (), 0u);
}

TEST (TrapUnreachableTest, LeavesAFunctionWithNoUnreachableUntouched)
{
	Trapped m (R"(
define void @caller() {
entry:
  call void @sink()
  ret void
}
)");

	EXPECT_FALSE (m.changed);
	EXPECT_EQ (m.traps (), 0u);
}

} // namespace
} // namespace test
} // namespace mono

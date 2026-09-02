/*
 * Tests for LowerKeepAlivePass, which rewrites call.cpp's keep_alive ()
 * marker - `llvm.fake.use` - into the inline asm read FastISel does not
 * drop.
 *
 * Pure LLVM. Nothing here reads a MonoMethod or any other runtime state.
 */

#include "passes/lower-keepalive.hpp"

#include <llvm/AsmParser/Parser.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/InlineAsm.h>
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
constexpr const char *preamble = "declare void @llvm.fake.use(...)\n";

/// A module the pass has run over.
struct Lowered {
	LLVMContext context;
	std::unique_ptr<Module> module;
	Function *caller = nullptr;
	bool changed = false;

	explicit Lowered (const std::string &ir)
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

		changed = !LowerKeepAlivePass ().run (*caller, fam).areAllPreserved ();

		std::string complaint;
		raw_string_ostream out (complaint);

		EXPECT_FALSE (verifyModule (*module, &out)) << complaint;
	}

	/// How many `llvm.fake.use` calls caller still holds.
	unsigned fake_uses () const
	{
		unsigned seen = 0;

		for (const Instruction &at : instructions (*caller))
			if (const auto *call = dyn_cast<CallInst> (&at))
				if (call->getIntrinsicID () == Intrinsic::fake_use)
					++seen;

		return seen;
	}

	/// How many inline asm reads of \p value caller holds.
	unsigned asm_reads_of (Value *value) const
	{
		unsigned seen = 0;

		for (const Instruction &at : instructions (*caller))
			if (const auto *call = dyn_cast<CallInst> (&at))
				if (isa<InlineAsm> (call->getCalledOperand ())
				    && call->arg_size () == 1 && call->getArgOperand (0) == value)
					++seen;

		return seen;
	}
};

TEST (LowerKeepAliveTest, RewritesAFakeUseIntoAnAsmRead)
{
	Lowered m (R"(
define void @caller(ptr %delegate) {
entry:
  call void (...) @llvm.fake.use(ptr %delegate)
  ret void
}
)");

	ASSERT_TRUE (m.changed);
	EXPECT_EQ (m.fake_uses (), 0u);
	EXPECT_EQ (m.asm_reads_of (m.caller->getArg (0)), 1u);
}

TEST (LowerKeepAliveTest, RewritesEveryFakeUseInTheFunction)
{
	Lowered m (R"(
define void @caller(ptr %a, ptr %b) {
entry:
  call void (...) @llvm.fake.use(ptr %a)
  call void (...) @llvm.fake.use(ptr %b)
  ret void
}
)");

	ASSERT_TRUE (m.changed);
	EXPECT_EQ (m.fake_uses (), 0u);
	EXPECT_EQ (m.asm_reads_of (m.caller->getArg (0)), 1u);
	EXPECT_EQ (m.asm_reads_of (m.caller->getArg (1)), 1u);
}

TEST (LowerKeepAliveTest, ReadsEveryArgumentOfAMultiArgFakeUse)
{
	Lowered m (R"(
define void @caller(ptr %a, ptr %b) {
entry:
  call void (...) @llvm.fake.use(ptr %a, ptr %b)
  ret void
}
)");

	ASSERT_TRUE (m.changed);
	EXPECT_EQ (m.fake_uses (), 0u);
	EXPECT_EQ (m.asm_reads_of (m.caller->getArg (0)), 1u);
	EXPECT_EQ (m.asm_reads_of (m.caller->getArg (1)), 1u);
}

TEST (LowerKeepAliveTest, LeavesAFunctionWithNoFakeUseUntouched)
{
	Lowered m (R"(
define void @caller(ptr %delegate) {
entry:
  ret void
}
)");

	EXPECT_FALSE (m.changed);
	EXPECT_EQ (m.fake_uses (), 0u);
}

} // namespace
} // namespace test
} // namespace mono

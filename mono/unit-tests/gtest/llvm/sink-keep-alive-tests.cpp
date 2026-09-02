/*
 * Tests for SinkKeepAlivePass, which moves a delegate keep_alive () marker
 * out of a loop where the delegate it names stays one value for the whole
 * run.
 *
 * Each case gives the pass a module built from raw IR - the shape
 * call.cpp's keep_alive () writes, `call void asm sideeffect "", "r"(ptr
 * %d)` - and reads back how many markers a named block holds afterward.
 *
 * Pure LLVM. Nothing here reads a MonoMethod or any other runtime state.
 */

#include "passes/sink-keep-alive.hpp"

#include <llvm/AsmParser/Parser.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/IR/Instructions.h>
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

/// A module the pass has run over.
struct Sunk {
	LLVMContext context;
	std::unique_ptr<Module> module;
	Function *caller = nullptr;
	bool changed = false;

	explicit Sunk (const std::string &ir)
	{
		SMDiagnostic problem;

		module = parseAssemblyString (ir, problem, context);

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

		ModuleAnalysisManager mam;
		FunctionAnalysisManager fam;
		LoopAnalysisManager lam;
		CGSCCAnalysisManager cgam;
		PassBuilder pb;

		pb.registerModuleAnalyses (mam);
		pb.registerFunctionAnalyses (fam);
		pb.registerLoopAnalyses (lam);
		pb.registerCGSCCAnalyses (cgam);
		pb.crossRegisterProxies (lam, fam, cgam, mam);

		changed = !SinkKeepAlivePass ().run (*caller, fam).areAllPreserved ();

		std::string complaint;
		raw_string_ostream out (complaint);

		EXPECT_FALSE (verifyModule (*module, &out)) << complaint;
	}

	/// How many keep_alive () markers the block named \p name holds.
	unsigned markers_in (StringRef name) const
	{
		for (const BasicBlock &block : *caller) {
			if (block.getName () != name)
				continue;

			unsigned seen = 0;

			for (const Instruction &at : block)
				if (const auto *call = dyn_cast<CallInst> (&at))
					if (isa<InlineAsm> (call->getCalledOperand ()))
						++seen;

			return seen;
		}

		ADD_FAILURE () << "no block named " << name.str ();
		return 0;
	}
};

TEST (SinkKeepAliveTest, MovesAnInvariantMarkerToTheLoopExit)
{
	Sunk m (R"(
define void @caller(ptr %delegate, i1 %cond) {
entry:
  br label %header

header:
  br i1 %cond, label %body, label %exit

body:
  call void asm sideeffect "", "r"(ptr %delegate)
  br label %header

exit:
  ret void
}
)");

	ASSERT_TRUE (m.changed);
	EXPECT_EQ (m.markers_in ("body"), 0u);
	EXPECT_EQ (m.markers_in ("exit"), 1u);
}

TEST (SinkKeepAliveTest, LeavesAMarkerWhoseDelegateTheLoopDefines)
{
	// %delegate is a fresh load every turn, so it is not the loop-invariant
	// case this pass moves.
	Sunk m (R"(
define void @caller(ptr %ref, i1 %cond) {
entry:
  br label %header

header:
  br i1 %cond, label %body, label %exit

body:
  %delegate = load ptr, ptr %ref, align 8
  call void asm sideeffect "", "r"(ptr %delegate)
  br label %header

exit:
  ret void
}
)");

	EXPECT_FALSE (m.changed);
	EXPECT_EQ (m.markers_in ("body"), 1u);
	EXPECT_EQ (m.markers_in ("exit"), 0u);
}

TEST (SinkKeepAliveTest, LeavesAMarkerInALoopWithNoExit)
{
	Sunk m (R"(
define void @caller(ptr %delegate) {
entry:
  br label %body

body:
  call void asm sideeffect "", "r"(ptr %delegate)
  br label %body
}
)");

	EXPECT_FALSE (m.changed);
	EXPECT_EQ (m.markers_in ("body"), 1u);
}

TEST (SinkKeepAliveTest, LeavesAMarkerOutsideAnyLoop)
{
	Sunk m (R"(
define void @caller(ptr %delegate) {
entry:
  call void asm sideeffect "", "r"(ptr %delegate)
  ret void
}
)");

	EXPECT_FALSE (m.changed);
	EXPECT_EQ (m.markers_in ("entry"), 1u);
}

TEST (SinkKeepAliveTest, MergesTwoMarkersForTheSameDelegateIntoOneCopy)
{
	Sunk m (R"(
define void @caller(ptr %delegate, i1 %cond) {
entry:
  br label %header

header:
  br i1 %cond, label %body, label %exit

body:
  call void asm sideeffect "", "r"(ptr %delegate)
  call void asm sideeffect "", "r"(ptr %delegate)
  br label %header

exit:
  ret void
}
)");

	ASSERT_TRUE (m.changed);
	EXPECT_EQ (m.markers_in ("body"), 0u);
	EXPECT_EQ (m.markers_in ("exit"), 1u);
}

TEST (SinkKeepAliveTest, ReachesEveryExitOfTheLoop)
{
	Sunk m (R"(
define void @caller(ptr %delegate, i1 %cond, i1 %early) {
entry:
  br label %header

header:
  br i1 %cond, label %body, label %exit1

body:
  br i1 %early, label %exit2, label %continue

continue:
  call void asm sideeffect "", "r"(ptr %delegate)
  br label %header

exit1:
  ret void

exit2:
  ret void
}
)");

	ASSERT_TRUE (m.changed);
	EXPECT_EQ (m.markers_in ("continue"), 0u);
	EXPECT_EQ (m.markers_in ("exit1"), 1u);
	EXPECT_EQ (m.markers_in ("exit2"), 1u);
}

TEST (SinkKeepAliveTest, SinksPastANestedLoopToTheOutermostThatIsStillInvariant)
{
	Sunk m (R"(
define void @caller(ptr %delegate, i1 %outer_cond, i1 %inner_cond) {
entry:
  br label %outer_header

outer_header:
  br i1 %outer_cond, label %outer_body, label %outer_exit

outer_body:
  br label %inner_header

inner_header:
  br i1 %inner_cond, label %inner_body, label %inner_exit

inner_body:
  call void asm sideeffect "", "r"(ptr %delegate)
  br label %inner_header

inner_exit:
  br label %outer_header

outer_exit:
  ret void
}
)");

	ASSERT_TRUE (m.changed);
	EXPECT_EQ (m.markers_in ("inner_body"), 0u);
	EXPECT_EQ (m.markers_in ("inner_exit"), 0u);
	EXPECT_EQ (m.markers_in ("outer_exit"), 1u);
}

} // namespace
} // namespace test
} // namespace mono

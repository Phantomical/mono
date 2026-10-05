#include "passes/keep-alive-tail-calls.hpp"

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

constexpr const char *preamble = R"(
declare void @llvm.fake.use(...)
declare i32 @callee()
)";

struct TailCallPassRun {
	LLVMContext context;
	std::unique_ptr<Module> module;
	Function *caller = nullptr;
	bool changed = false;

	explicit TailCallPassRun (const std::string &ir)
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

		changed = !KeepAliveTailCallsPass ().run (*caller, fam).areAllPreserved ();

		std::string complaint;
		raw_string_ostream out (complaint);

		EXPECT_FALSE (verifyModule (*module, &out)) << complaint;
	}

	CallInst::TailCallKind callee_tail_kind () const
	{
		for (const Instruction &at : instructions (*caller))
			if (const auto *call = dyn_cast<CallInst> (&at))
				if (call->getCalledFunction () != nullptr
				    && call->getCalledFunction ()->getName () == "callee")
					return call->getTailCallKind ();

		ADD_FAILURE () << "caller holds no call to callee";
		return CallInst::TCK_None;
	}
};

TEST (KeepAliveTailCallsTest, ClearsTailInFrontOfAMarker)
{
	TailCallPassRun result (R"(
define i32 @caller(ptr %pinned) {
entry:
  %r = tail call i32 @callee()
  call void (...) @llvm.fake.use(ptr %pinned)
  ret i32 %r
}
)");

	EXPECT_TRUE (result.changed);
	EXPECT_EQ (result.callee_tail_kind (), CallInst::TCK_None);
}

TEST (KeepAliveTailCallsTest, ClearsTailInAnotherBlock)
{
	TailCallPassRun result (R"(
define i32 @caller(ptr %pinned, i1 %c) {
entry:
  br i1 %c, label %call, label %done
call:
  %r = tail call i32 @callee()
  br label %done
done:
  %v = phi i32 [ %r, %call ], [ 0, %entry ]
  call void (...) @llvm.fake.use(ptr %pinned)
  ret i32 %v
}
)");

	EXPECT_TRUE (result.changed);
	EXPECT_EQ (result.callee_tail_kind (), CallInst::TCK_None);
}

TEST (KeepAliveTailCallsTest, KeepsMustTail)
{
	TailCallPassRun result (R"(
define i32 @caller() {
entry:
  %pinned = call ptr @pinned()
  call void (...) @llvm.fake.use(ptr %pinned)
  %r = musttail call i32 @callee()
  ret i32 %r
}

declare ptr @pinned()
)");

	EXPECT_FALSE (result.changed);
	EXPECT_EQ (result.callee_tail_kind (), CallInst::TCK_MustTail);
}

TEST (KeepAliveTailCallsTest, LeavesAFunctionWithNoMarkerUntouched)
{
	TailCallPassRun result (R"(
define i32 @caller() {
entry:
  %r = tail call i32 @callee()
  ret i32 %r
}
)");

	EXPECT_FALSE (result.changed);
	EXPECT_EQ (result.callee_tail_kind (), CallInst::TCK_Tail);
}

} // namespace
} // namespace test
} // namespace mono

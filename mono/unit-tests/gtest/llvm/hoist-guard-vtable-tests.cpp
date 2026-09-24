/*
 * Tests for HoistGuardVtablePass. Each case is the shape GuardDispatchPass
 * leaves in a loop: a vtable read the dispatch on the missing arm still names,
 * and a compare of that read against a class's vtable.
 */

#include "passes/hoist-guard-vtable.hpp"

#include <llvm/AsmParser/Parser.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/Support/SourceMgr.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <memory>
#include <string>

using namespace llvm;

namespace mono {
namespace test {
namespace {

constexpr const char *preamble = R"(
@vtable_A = external global i8
@vtable_B = external global i8
declare ptr @mono.vtable.func(ptr, i32) memory(none) nounwind
declare i32 @direct(ptr)
declare void @throw()
)";

/// A module the pass has run over.
struct Hoisted {
	LLVMContext context;
	std::unique_ptr<Module> module;
	Function *caller = nullptr;
	bool changed = false;

	explicit Hoisted (const std::string &ir)
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

		changed = !HoistGuardVtablePass ().run (*caller, fam).areAllPreserved ();

		std::string complaint;
		raw_string_ostream out (complaint);

		EXPECT_FALSE (verifyModule (*module, &out)) << complaint;
	}

	/// The compare named \p name.
	ICmpInst *compare (StringRef name) const
	{
		for (BasicBlock &block : *caller)
			for (Instruction &at : block)
				if (at.getName () == name)
					return cast<ICmpInst> (&at);

		ADD_FAILURE () << "no compare named " << name.str ();
		return nullptr;
	}

	/// The load named \p name.
	LoadInst *load (StringRef name) const
	{
		for (BasicBlock &block : *caller)
			for (Instruction &at : block)
				if (at.getName () == name)
					return cast<LoadInst> (&at);

		ADD_FAILURE () << "no load named " << name.str ();
		return nullptr;
	}

	/// How many loads of \p object's first word sit in no block named in
	/// \p inside.
	unsigned reads_outside (Value *object, std::initializer_list<StringRef> inside) const
	{
		unsigned seen = 0;

		for (BasicBlock &block : *caller) {
			if (std::find (inside.begin (), inside.end (), block.getName ()) != inside.end ())
				continue;

			for (Instruction &at : block)
				if (auto *read = dyn_cast<LoadInst> (&at))
					if (read->getPointerOperand () == object)
						++seen;
		}

		return seen;
	}
};

// Nothing outside the loop proves %s is non-null, which is what keeps LICM
// from hoisting the read on its own.
TEST (HoistGuardVtableTest, ReadsAnInvariantReceiverInThePreheader)
{
	Hoisted m (R"(
define i32 @caller(ptr %s, i32 %n) {
entry:
  br label %header

header:
  %i = phi i32 [ 0, %entry ], [ %next, %done ]
  %isnull = icmp eq ptr %s, null
  br i1 %isnull, label %npe, label %checked

checked:
  %vt = load ptr, ptr %s, align 8
  %hit = icmp eq ptr %vt, @vtable_A
  br i1 %hit, label %fast, label %slow

fast:
  %a = call i32 @direct(ptr %s)
  br label %done

slow:
  %entry.fn = call ptr @mono.vtable.func(ptr %vt, i32 4)
  %b = call i32 %entry.fn(ptr %s)
  br label %done

done:
  %r = phi i32 [ %a, %fast ], [ %b, %slow ]
  %next = add i32 %i, 1
  %more = icmp slt i32 %next, %n
  br i1 %more, label %header, label %exit

npe:
  call void @throw()
  unreachable

exit:
  ret i32 %r
}
)");

	ASSERT_TRUE (m.changed);

	auto *hoisted = dyn_cast<PHINode> (m.compare ("hit")->getOperand (0));

	ASSERT_NE (hoisted, nullptr);
	EXPECT_EQ (hoisted->getNumIncomingValues (), 2u);

	// The dispatch keeps the read made inside the loop.
	LoadInst *kept = m.load ("vt");

	EXPECT_EQ (kept->getParent ()->getName (), "checked");
	EXPECT_FALSE (kept->use_empty ());

	Value *s = m.caller->getArg (0);

	EXPECT_EQ (m.reads_outside (s, { "checked" }), 1u);
}

// The block in front of the loop also branches to a throw, so the loop has no
// preheader.
TEST (HoistGuardVtableTest, LeavesALoopWithNoPreheader)
{
	Hoisted m (R"(
define void @caller(ptr %s, ptr %other, i32 %n) {
entry:
  %othernull = icmp eq ptr %other, null
  br i1 %othernull, label %npe, label %header

header:
  %i = phi i32 [ 0, %entry ], [ %next, %done ]
  %vt = load ptr, ptr %s, align 8
  %hit = icmp eq ptr %vt, @vtable_A
  br i1 %hit, label %done, label %slow

slow:
  %f = call ptr @mono.vtable.func(ptr %vt, i32 4)
  %x = call i32 %f(ptr %s)
  br label %done

done:
  %next = add i32 %i, 1
  %more = icmp slt i32 %next, %n
  br i1 %more, label %header, label %exit

npe:
  call void @throw()
  unreachable

exit:
  ret void
}
)");

	EXPECT_FALSE (m.changed);
	EXPECT_EQ (m.compare ("hit")->getOperand (0), m.load ("vt"));
}

TEST (HoistGuardVtableTest, SharesOneReadBetweenTwoGuardsOnOneReceiver)
{
	Hoisted m (R"(
define void @caller(ptr nonnull %s, i32 %n) {
entry:
  br label %header

header:
  %i = phi i32 [ 0, %entry ], [ %next, %done2 ]
  %vt1 = load ptr, ptr %s, align 8
  %hit1 = icmp eq ptr %vt1, @vtable_A
  br i1 %hit1, label %done1, label %slow1

slow1:
  %f1 = call ptr @mono.vtable.func(ptr %vt1, i32 4)
  %x1 = call i32 %f1(ptr %s)
  br label %done1

done1:
  %vt2 = load ptr, ptr %s, align 8
  %hit2 = icmp eq ptr %vt2, @vtable_B
  br i1 %hit2, label %done2, label %slow2

slow2:
  %f2 = call ptr @mono.vtable.func(ptr %vt2, i32 5)
  %x2 = call i32 %f2(ptr %s)
  br label %done2

done2:
  %next = add i32 %i, 1
  %more = icmp slt i32 %next, %n
  br i1 %more, label %header, label %exit

exit:
  ret void
}
)");

	ASSERT_TRUE (m.changed);
	EXPECT_EQ (m.compare ("hit1")->getOperand (0), m.compare ("hit2")->getOperand (0));
	EXPECT_EQ (m.reads_outside (m.caller->getArg (0), { "header", "done1" }), 1u);
}

TEST (HoistGuardVtableTest, LeavesAReceiverTheLoopDefines)
{
	Hoisted m (R"(
define void @caller(ptr %slot, i32 %n) {
entry:
  br label %header

header:
  %i = phi i32 [ 0, %entry ], [ %next, %done ]
  %s = load ptr, ptr %slot, align 8
  %vt = load ptr, ptr %s, align 8
  %hit = icmp eq ptr %vt, @vtable_A
  br i1 %hit, label %done, label %slow

slow:
  %f = call ptr @mono.vtable.func(ptr %vt, i32 4)
  %x = call i32 %f(ptr %s)
  br label %done

done:
  %next = add i32 %i, 1
  %more = icmp slt i32 %next, %n
  br i1 %more, label %header, label %exit

exit:
  ret void
}
)");

	EXPECT_FALSE (m.changed);
	EXPECT_EQ (m.compare ("hit")->getOperand (0), m.load ("vt"));
}

TEST (HoistGuardVtableTest, LeavesAReadOutsideAnyLoop)
{
	Hoisted m (R"(
define i32 @caller(ptr nonnull %s) {
entry:
  %vt = load ptr, ptr %s, align 8
  %hit = icmp eq ptr %vt, @vtable_A
  br i1 %hit, label %done, label %slow

slow:
  %f = call ptr @mono.vtable.func(ptr %vt, i32 4)
  %x = call i32 %f(ptr %s)
  br label %done

done:
  %r = phi i32 [ 0, %entry ], [ %x, %slow ]
  ret i32 %r
}
)");

	EXPECT_FALSE (m.changed);
}

// The load feeds no dispatch declaration, so nothing marks it as a vtable read.
TEST (HoistGuardVtableTest, LeavesALoadNoDispatchReads)
{
	Hoisted m (R"(
define void @caller(ptr nonnull %s, i32 %n) {
entry:
  br label %header

header:
  %i = phi i32 [ 0, %entry ], [ %next, %header ]
  %vt = load ptr, ptr %s, align 8
  %hit = icmp eq ptr %vt, @vtable_A
  %next = add i32 %i, 1
  %more = icmp slt i32 %next, %n
  br i1 %more, label %header, label %exit

exit:
  ret void
}
)");

	EXPECT_FALSE (m.changed);
}

TEST (HoistGuardVtableTest, LeavesACompareAgainstNull)
{
	Hoisted m (R"(
define void @caller(ptr nonnull %s, i32 %n) {
entry:
  br label %header

header:
  %i = phi i32 [ 0, %entry ], [ %next, %done ]
  %vt = load ptr, ptr %s, align 8
  %hit = icmp eq ptr %vt, null
  br i1 %hit, label %done, label %slow

slow:
  %f = call ptr @mono.vtable.func(ptr %vt, i32 4)
  %x = call i32 %f(ptr %s)
  br label %done

done:
  %next = add i32 %i, 1
  %more = icmp slt i32 %next, %n
  br i1 %more, label %header, label %exit

exit:
  ret void
}
)");

	EXPECT_FALSE (m.changed);
}

} // namespace
} // namespace test
} // namespace mono

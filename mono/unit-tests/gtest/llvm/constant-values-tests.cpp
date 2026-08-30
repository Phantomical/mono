/*
 * Tests for MonoConstantValues, the walk that settles what a function's values
 * hold.
 *
 * Each case names the value it asks about with a `!ask` mark, so it reads its
 * answer back without counting instructions.
 */

#include "analysis/constant-values.hpp"

#include <llvm/AsmParser/Parser.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/PassManager.h>
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
///
/// Each declaration carries the attributes the translator puts on it. The walk
/// reads them to decide that a call is not a way out, so a case that drops one
/// gets a different answer.
constexpr const char *preamble = R"(
declare noalias ptr @"mono.alloc.object"(ptr, i64, ptr)
    allockind("alloc,zeroed") memory(argmem: read, inaccessiblemem: readwrite)
declare noalias ptr @"mono.alloc.object.kept"(ptr, i64, ptr)
    memory(argmem: read, inaccessiblemem: readwrite)
declare void @"mono.gc.wbarrier"(ptr captures(none) readonly, ptr captures(none) readonly)
    memory(argmem: read, inaccessiblemem: readwrite)
declare void @"mono.gc.wbarrier.value.copy"(ptr captures(none), ptr captures(none) readonly,
                                            i32, i64, ptr)
    memory(argmem: readwrite, inaccessiblemem: readwrite)
declare void @llvm.memcpy.p0.p0.i64(ptr captures(none), ptr captures(none), i64, i1)
declare void @opaque(ptr)
declare ptr @allocator(ptr, i64)
@vtable_Foo = external global i8
@vtable_Bar = external global i8
!0 = !{}
)";

/// The mark a case puts on the instruction it asks about.
constexpr const char *asked = "ask";

/// A module the walk has run over.
struct Settled {
	LLVMContext context;
	std::unique_ptr<Module> module;
	Function *caller = nullptr;
	ConstantValues values;

	/// Runs `MonoMemoryValues` unless \p reads_memory says otherwise, because
	/// most cases below ask what a store left in a field.
	explicit Settled (const std::string &ir, bool reads_memory = true)
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

		std::string complaint;
		raw_string_ostream out (complaint);

		EXPECT_FALSE (verifyModule (*module, &out)) << complaint;

		FunctionAnalysisManager fam;
		ModuleAnalysisManager mam;
		LoopAnalysisManager lam;
		CGSCCAnalysisManager cgam;
		PassBuilder pb;

		// All four, because MemorySSA asks alias analysis for GlobalsAA, which is
		// a module analysis it reaches through the proxy.
		pb.registerModuleAnalyses (mam);
		pb.registerCGSCCAnalyses (cgam);
		pb.registerFunctionAnalyses (fam);
		pb.registerLoopAnalyses (lam);
		pb.crossRegisterProxies (lam, fam, cgam, mam);

		if (reads_memory)
			values = MonoMemoryValues ().run (*caller, fam);
		else
			values = MonoConstantValues ().run (*caller, fam);
	}

	/// The instruction the module marked `!ask`.
	Instruction &question () const
	{
		for (Instruction &at : instructions (*caller))
			if (at.getMetadata (asked) != nullptr)
				return at;

		ADD_FAILURE () << "no instruction carries !" << asked;
		return *caller->getEntryBlock ().begin ();
	}

	/// The constant the marked instruction settles to, as text, or "-" where
	/// it settles to no constant. Text, so a failure names what came back.
	std::string answer () const
	{
		const Constant *held = values.value (&question ());

		if (held == nullptr)
			return "-";

		if (const auto *named = dyn_cast<GlobalValue> (held))
			return named->getName ().str ();

		std::string text;
		raw_string_ostream out (text);

		held->printAsOperand (out, /*PrintType=*/false);

		return text;
	}

	const ValueSources &sources () const { return values.sources (&question ()); }

	/// Whether the walk settled every path into the marked instruction.
	bool complete () const { return !sources ().sources.contains (&question ()); }

	/// The values the marked instruction is reached by, named the way
	/// `answer ()` names one. The instruction itself is left out, because
	/// `complete ()` reports it.
	std::vector<std::string> reached () const
	{
		std::vector<std::string> named;

		for (const Value *v : sources ().sources) {
			if (v == &question ())
				continue;

			if (const auto *global = dyn_cast<GlobalValue> (v)) {
				named.push_back (global->getName ().str ());
				continue;
			}

			std::string text;
			raw_string_ostream out (text);

			v->printAsOperand (out, /*PrintType=*/false);
			named.push_back (text);
		}

		llvm::sort (named);

		return named;
	}
};

/// The walk sees through a `ptrtoint`, so the pointer is what reaches the
/// integer. A caller writes the answer back where it asked, so it comes back
/// under the integer's own type rather than as the pointer.
TEST (ConstantValuesTest, AnIntegerFromAPointerHoldsThePointerCastToIt)
{
	Settled m (R"(
define i64 @caller(i1 %c, ptr %p, i64 %n) {
entry:
  %word = ptrtoint ptr @vtable_Foo to i64, !ask !0
  ret i64 %word
}
)");

	const Constant *held = m.values.value (&m.question ());

	ASSERT_NE (held, nullptr);
	EXPECT_EQ (held->getType (), m.question ().getType ());
	EXPECT_EQ (m.reached (),
	           std::vector<std::string> { "ptrtoint (ptr @vtable_Foo to i64)" });
}

TEST (ConstantValuesTest, AConstantHoldsItself)
{
	Settled m (R"(
define i64 @caller(i1 %c, ptr %p, i64 %n) {
entry:
  %sum = add i64 4, 0, !ask !0
  ret i64 %sum
}
)");

	EXPECT_EQ (m.answer (), "4");
}

TEST (ConstantValuesTest, AGlobalComesBackFromValue)
{
	Settled m (R"(
define ptr @caller(i1 %c, ptr %p, i64 %n) {
entry:
  %o = call ptr @"mono.alloc.object"(ptr @vtable_Foo, i64 128, ptr @allocator)
  %v = load ptr, ptr @vtable_Foo, !ask !0
  ret ptr %o
}
)");

	EXPECT_EQ (m.values.value (m.caller->getEntryBlock ().begin ()->getOperand (0)),
	           m.module->getNamedValue ("vtable_Foo"));
}

TEST (ConstantValuesTest, ACastIsLookedThrough)
{
	Settled m (R"(
define ptr @caller(i1 %c, ptr %p, i64 %n) {
entry:
  %i = ptrtoint ptr @vtable_Foo to i64
  %back = inttoptr i64 %i to ptr, !ask !0
  ret ptr %back
}
)");

	EXPECT_EQ (m.answer (), "vtable_Foo");
}

TEST (ConstantValuesTest, AnOpaqueValueIsItsOwnSource)
{
	Settled m (R"(
define ptr @caller(i1 %c, ptr %p, i64 %n) {
entry:
  %o = call ptr @opaque2(), !ask !0
  ret ptr %o
}
declare ptr @opaque2()
)");

	EXPECT_EQ (m.answer (), "-");
	EXPECT_FALSE (m.complete ());
	EXPECT_TRUE (m.reached ().empty ());
}

TEST (ConstantValuesTest, AMergeOfOneConstantAnswersIt)
{
	Settled m (R"(
define i64 @caller(i1 %c, ptr %p, i64 %n) {
entry:
  br i1 %c, label %left, label %right
left:
  br label %merge
right:
  br label %merge
merge:
  %held = phi i64 [ 7, %left ], [ 7, %right ], !ask !0
  ret i64 %held
}
)");

	EXPECT_EQ (m.answer (), "7");
}

TEST (ConstantValuesTest, AMergeOfTwoConstantsIsReachedByBoth)
{
	Settled m (R"(
define i64 @caller(i1 %c, ptr %p, i64 %n) {
entry:
  br i1 %c, label %left, label %right
left:
  br label %merge
right:
  br label %merge
merge:
  %held = phi i64 [ 7, %left ], [ 9, %right ], !ask !0
  ret i64 %held
}
)");

	EXPECT_EQ (m.answer (), "-");
	EXPECT_TRUE (m.complete ());
	EXPECT_EQ (m.reached (), (std::vector<std::string> { "7", "9" }));
}

TEST (ConstantValuesTest, ASelectIsAMerge)
{
	Settled m (R"(
define ptr @caller(i1 %c, ptr %p, i64 %n) {
entry:
  %held = select i1 %c, ptr @vtable_Foo, ptr @vtable_Foo, !ask !0
  ret ptr %held
}
)");

	EXPECT_EQ (m.answer (), "vtable_Foo");
}

TEST (ConstantValuesTest, AFreezeIsLookedThrough)
{
	Settled m (R"(
define ptr @caller(i1 %c, ptr %p, i64 %n) {
entry:
  %held = freeze ptr @vtable_Foo, !ask !0
  ret ptr %held
}
)");

	EXPECT_EQ (m.answer (), "vtable_Foo");
}

TEST (ConstantValuesTest, AFreezeOfPoisonHoldsNothing)
{
	Settled m (R"(
define ptr @caller(i1 %c, ptr %p, i64 %n) {
entry:
  %held = freeze ptr poison, !ask !0
  ret ptr %held
}
)");

	EXPECT_EQ (m.answer (), "-");
	EXPECT_FALSE (m.complete ());
}

// A walk that drops a back edge to stop a cycle finds nothing here. This one
// folds the back edge in and converges.
TEST (ConstantValuesTest, ALoopCarriedValueConverges)
{
	Settled m (R"(
define ptr @caller(i1 %c, ptr %p, i64 %n) {
entry:
  br label %head
head:
  %carried = phi ptr [ @vtable_Foo, %entry ], [ %carried, %head ], !ask !0
  br i1 %c, label %head, label %done
done:
  ret ptr %carried
}
)");

	EXPECT_EQ (m.answer (), "vtable_Foo");
}

TEST (ConstantValuesTest, TwoPhisInACycleConverge)
{
	Settled m (R"(
define ptr @caller(i1 %c, ptr %p, i64 %n) {
entry:
  br label %head
head:
  %first = phi ptr [ @vtable_Foo, %entry ], [ %second, %latch ], !ask !0
  br i1 %c, label %latch, label %done
latch:
  %second = phi ptr [ %first, %head ]
  br label %head
done:
  ret ptr %first
}
)");

	EXPECT_EQ (m.answer (), "vtable_Foo");
}

TEST (ConstantValuesTest, ALoopCarriedCounterHoldsNoConstant)
{
	Settled m (R"(
define i64 @caller(i1 %c, ptr %p, i64 %n) {
entry:
  br label %head
head:
  %counter = phi i64 [ 0, %entry ], [ %next, %head ], !ask !0
  %next = add i64 %counter, 1
  br i1 %c, label %head, label %done
done:
  ret i64 %counter
}
)");

	EXPECT_EQ (m.answer (), "-");
}

/// A store that dominates the load runs on every path that reaches it, so the
/// field holds what it wrote and nothing else.
TEST (ConstantValuesTest, AFieldADominatingStoreSettles)
{
	Settled m (R"(
define ptr @caller(i1 %c, ptr %p, i64 %n) {
entry:
  %o = call ptr @"mono.alloc.object"(ptr @vtable_Foo, i64 128, ptr @allocator)
  %f = getelementptr inbounds i8, ptr %o, i32 32
  store ptr @vtable_Bar, ptr %f, align 8
  %held = load ptr, ptr %f, align 8, !ask !0
  ret ptr %held
}
)");

	EXPECT_TRUE (m.complete ());
	EXPECT_EQ (m.reached (), std::vector<std::string> { "vtable_Bar" });
}

/// The second store always runs after the first, so only what it wrote is
/// still there when the load reads.
/// A wider store names no value this walk can read, and it still covers the
/// store before it. So the value that store wrote is one no path reads, and
/// naming it would hand a caller a candidate that cannot occur.
TEST (ConstantValuesTest, AWiderStoreCoversTheOneBeforeIt)
{
	Settled m (R"(
define i32 @caller(i1 %c, ptr %p, i64 %n) {
entry:
  %o = call ptr @"mono.alloc.object"(ptr @vtable_Foo, i64 128, ptr @allocator)
  %f = getelementptr inbounds i8, ptr %o, i32 32
  store i32 9, ptr %f, align 8
  br i1 %c, label %wide, label %narrow
wide:
  store i64 7, ptr %f, align 8
  br label %merge
narrow:
  store i32 5, ptr %f, align 8
  br label %merge
merge:
  %held = load i32, ptr %f, align 8, !ask !0
  ret i32 %held
}
)");

	EXPECT_FALSE (m.complete ());
	EXPECT_TRUE (is_contained (m.reached (), "5"));
	EXPECT_FALSE (is_contained (m.reached (), "9"));
}

TEST (ConstantValuesTest, AStoreOverAnotherLeavesOnlyTheLater)
{
	Settled m (R"(
define ptr @caller(i1 %c, ptr %p, i64 %n) {
entry:
  %o = call ptr @"mono.alloc.object"(ptr @vtable_Foo, i64 128, ptr @allocator)
  %f = getelementptr inbounds i8, ptr %o, i32 32
  store ptr @vtable_Foo, ptr %f, align 8
  store ptr @vtable_Bar, ptr %f, align 8
  %held = load ptr, ptr %f, align 8, !ask !0
  ret ptr %held
}
)");

	EXPECT_TRUE (m.complete ());
	EXPECT_EQ (m.reached (), std::vector<std::string> { "vtable_Bar" });
}

/// The later store on one arm instead. It no longer runs on every path, so
/// the earlier one is still a value the load can read.
TEST (ConstantValuesTest, AStoreOnOneArmDoesNotCoverTheEarlierOne)
{
	Settled m (R"(
define ptr @caller(i1 %c, ptr %p, i64 %n) {
entry:
  %o = call ptr @"mono.alloc.object"(ptr @vtable_Foo, i64 128, ptr @allocator)
  %f = getelementptr inbounds i8, ptr %o, i32 32
  store ptr @vtable_Foo, ptr %f, align 8
  br i1 %c, label %wrote, label %merge
wrote:
  store ptr @vtable_Bar, ptr %f, align 8
  br label %merge
merge:
  %held = load ptr, ptr %f, align 8, !ask !0
  ret ptr %held
}
)");

	EXPECT_TRUE (m.complete ());
	EXPECT_EQ (m.reached (), (std::vector<std::string> { "vtable_Bar", "vtable_Foo" }));
}

/// The same store on one arm only. The load can now be reached without it, so
/// the zero the allocation left is a value the field still holds.
TEST (ConstantValuesTest, AFieldOneArmStoresIntoAlsoReadsTheZero)
{
	Settled m (R"(
define ptr @caller(i1 %c, ptr %p, i64 %n) {
entry:
  %o = call ptr @"mono.alloc.object"(ptr @vtable_Foo, i64 128, ptr @allocator)
  %f = getelementptr inbounds i8, ptr %o, i32 32
  br i1 %c, label %wrote, label %merge
wrote:
  store ptr @vtable_Bar, ptr %f, align 8
  br label %merge
merge:
  %held = load ptr, ptr %f, align 8, !ask !0
  ret ptr %held
}
)");

	EXPECT_TRUE (m.complete ());
	EXPECT_EQ (m.reached (), (std::vector<std::string> { "null", "vtable_Bar" }));
}

/// An allocation that does not declare `zeroed` hands back anything, so a load
/// no store dominates reads a value this walk cannot name.
TEST (ConstantValuesTest, AFieldOfAnUnzeroedAllocationIsIncomplete)
{
	Settled m (R"(
define ptr @caller(i1 %c, ptr %p, i64 %n) {
entry:
  %o = call ptr @"mono.alloc.object.kept"(ptr @vtable_Foo, i64 128, ptr @allocator)
  %f = getelementptr inbounds i8, ptr %o, i32 32
  br i1 %c, label %wrote, label %merge
wrote:
  store ptr @vtable_Bar, ptr %f, align 8
  br label %merge
merge:
  %held = load ptr, ptr %f, align 8, !ask !0
  ret ptr %held
}
)");

	EXPECT_FALSE (m.complete ());
	EXPECT_EQ (m.reached (), std::vector<std::string> { "vtable_Bar" });
}

/// A field nothing ever stores into holds the zero the allocation left, which
/// is an answer rather than an absence of one.
TEST (ConstantValuesTest, AFieldNoStoreReachesIsTheAllocationsZero)
{
	Settled m (R"(
define ptr @caller(i1 %c, ptr %p, i64 %n) {
entry:
  %o = call ptr @"mono.alloc.object"(ptr @vtable_Foo, i64 128, ptr @allocator)
  %f = getelementptr inbounds i8, ptr %o, i32 32
  %held = load ptr, ptr %f, align 8, !ask !0
  ret ptr %held
}
)");

	EXPECT_TRUE (m.complete ());
	EXPECT_EQ (m.reached (), std::vector<std::string> { "null" });
}

/// Each allocation in the merge answers for itself: the zeroed arm still names
/// its zero, and the arm that does not zero is what leaves the set incomplete.
TEST (ConstantValuesTest, AFieldOfAMixedMergeTakesTheUnzeroedAnswer)
{
	Settled m (R"(
define ptr @caller(i1 %c, ptr %p, i64 %n) {
entry:
  br i1 %c, label %left, label %right
left:
  %a = call ptr @"mono.alloc.object"(ptr @vtable_Foo, i64 128, ptr @allocator)
  %fa = getelementptr inbounds i8, ptr %a, i32 32
  store ptr @vtable_Bar, ptr %fa, align 8
  br label %merge
right:
  %b = call ptr @"mono.alloc.object.kept"(ptr @vtable_Foo, i64 128, ptr @allocator)
  %fb = getelementptr inbounds i8, ptr %b, i32 32
  store ptr @vtable_Bar, ptr %fb, align 8
  br label %merge
merge:
  %o = phi ptr [ %a, %left ], [ %b, %right ]
  %f = getelementptr inbounds i8, ptr %o, i32 32
  %held = load ptr, ptr %f, align 8, !ask !0
  ret ptr %held
}
)");

	EXPECT_FALSE (m.complete ());
	EXPECT_EQ (m.reached (), (std::vector<std::string> { "null", "vtable_Bar" }));
}

/// The same merge with both allocations zeroing, which is the control the case
/// above is read against.
TEST (ConstantValuesTest, AFieldOfAZeroedMergeReadsTheZeroToo)
{
	Settled m (R"(
define ptr @caller(i1 %c, ptr %p, i64 %n) {
entry:
  br i1 %c, label %left, label %right
left:
  %a = call ptr @"mono.alloc.object"(ptr @vtable_Foo, i64 128, ptr @allocator)
  %fa = getelementptr inbounds i8, ptr %a, i32 32
  store ptr @vtable_Bar, ptr %fa, align 8
  br label %merge
right:
  %b = call ptr @"mono.alloc.object"(ptr @vtable_Foo, i64 128, ptr @allocator)
  %fb = getelementptr inbounds i8, ptr %b, i32 32
  store ptr @vtable_Bar, ptr %fb, align 8
  br label %merge
merge:
  %o = phi ptr [ %a, %left ], [ %b, %right ]
  %f = getelementptr inbounds i8, ptr %o, i32 32
  %held = load ptr, ptr %f, align 8, !ask !0
  ret ptr %held
}
)");

	EXPECT_TRUE (m.complete ());
	EXPECT_EQ (m.reached (), (std::vector<std::string> { "null", "vtable_Bar" }));
}

/// The read carries no `!invariant.load`. MemorySSA would put such a load in
/// front of the header store, and the walk would read the allocation's zero.
TEST (ConstantValuesTest, AVtableReadForwardsToTheHeaderStore)
{
	Settled m (R"(
define ptr @caller(i1 %c, ptr %p, i64 %n) {
entry:
  %o = call ptr @"mono.alloc.object"(ptr @vtable_Foo, i64 128, ptr @allocator)
  store ptr @vtable_Foo, ptr %o, align 8
  %held = load ptr, ptr %o, align 8, !ask !0
  ret ptr %held
}
)");

	EXPECT_EQ (m.answer (), "vtable_Foo");
}

/// `MonoConstantValues` reads no memory, so a load stands as its own source
/// whatever store settles the field in front of it. The case above is the same
/// module under `MonoMemoryValues`, which is what forwards one.
TEST (ConstantValuesTest, TheWalkWithoutMemoryLeavesALoadItsOwnSource)
{
	Settled m (R"(
define ptr @caller(i1 %c, ptr %p, i64 %n) {
entry:
  %o = call ptr @"mono.alloc.object"(ptr @vtable_Foo, i64 128, ptr @allocator)
  store ptr @vtable_Foo, ptr %o, align 8
  %held = load ptr, ptr %o, align 8, !ask !0
  ret ptr %held
}
)",
	           /*reads_memory=*/false);

	EXPECT_EQ (m.answer (), "-");
	EXPECT_FALSE (m.complete ());
	EXPECT_TRUE (m.reached ().empty ());
}

TEST (ConstantValuesTest, AFieldTwoStoresAgreeOnIsReachedByOneValue)
{
	Settled m (R"(
define ptr @caller(i1 %c, ptr %p, i64 %n) {
entry:
  %o = call ptr @"mono.alloc.object"(ptr @vtable_Foo, i64 128, ptr @allocator)
  %f = getelementptr inbounds i8, ptr %o, i32 32
  br i1 %c, label %left, label %right
left:
  store ptr @vtable_Bar, ptr %f, align 8
  br label %merge
right:
  store ptr @vtable_Bar, ptr %f, align 8
  br label %merge
merge:
  %held = load ptr, ptr %f, align 8, !ask !0
  ret ptr %held
}
)");

	// Every path stores, so the zero the allocation was made holding is not
	// one of the values the load can read.
	EXPECT_TRUE (m.complete ());
	EXPECT_EQ (m.reached (), std::vector<std::string> { "vtable_Bar" });
}

TEST (ConstantValuesTest, AnObjectThatEscapesLeavesItsFieldsIncomplete)
{
	Settled m (R"(
define ptr @caller(i1 %c, ptr %p, i64 %n) {
entry:
  %o = call ptr @"mono.alloc.object"(ptr @vtable_Foo, i64 128, ptr @allocator)
  %f = getelementptr inbounds i8, ptr %o, i32 32
  store ptr @vtable_Bar, ptr %f, align 8
  call void @opaque(ptr %o)
  %held = load ptr, ptr %f, align 8, !ask !0
  ret ptr %held
}
)");

	EXPECT_FALSE (m.complete ());
	EXPECT_TRUE (is_contained (m.reached (), "vtable_Bar"));
}

TEST (ConstantValuesTest, TheWriteBarrierIsNotAWayOut)
{
	Settled m (R"(
define ptr @caller(i1 %c, ptr %p, i64 %n) {
entry:
  %o = call ptr @"mono.alloc.object"(ptr @vtable_Foo, i64 128, ptr @allocator)
  %f = getelementptr inbounds i8, ptr %o, i32 32
  store ptr @vtable_Bar, ptr %f, align 8
  call void @"mono.gc.wbarrier"(ptr %f, ptr @vtable_Bar)
  %held = load ptr, ptr %f, align 8, !ask !0
  ret ptr %held
}
)");

	EXPECT_TRUE (m.complete ());
	EXPECT_TRUE (is_contained (m.reached (), "vtable_Bar"));
}

TEST (ConstantValuesTest, AMemcpyOverAFieldClearsIt)
{
	Settled m (R"(
define ptr @caller(i1 %c, ptr %p, i64 %n) {
entry:
  %o = call ptr @"mono.alloc.object"(ptr @vtable_Foo, i64 128, ptr @allocator)
  %f = getelementptr inbounds i8, ptr %o, i32 32
  store ptr @vtable_Bar, ptr %f, align 8
  call void @llvm.memcpy.p0.p0.i64(ptr %f, ptr %p, i64 8, i1 false)
  %held = load ptr, ptr %f, align 8, !ask !0
  ret ptr %held
}
)");

	EXPECT_FALSE (m.complete ());
}

/// A store between the copy and the load runs on every path that reaches the
/// load, so what the copy wrote is gone by the time the load reads.
TEST (ConstantValuesTest, AStoreAfterAMemcpyLeavesNothingOfIt)
{
	Settled m (R"(
define ptr @caller(i1 %c, ptr %p, i64 %n) {
entry:
  %o = call ptr @"mono.alloc.object"(ptr @vtable_Foo, i64 128, ptr @allocator)
  %f = getelementptr inbounds i8, ptr %o, i32 32
  call void @llvm.memcpy.p0.p0.i64(ptr %f, ptr %p, i64 8, i1 false)
  store ptr @vtable_Bar, ptr %f, align 8
  %held = load ptr, ptr %f, align 8, !ask !0
  ret ptr %held
}
)");

	EXPECT_TRUE (m.complete ());
	EXPECT_EQ (m.reached (), std::vector<std::string> { "vtable_Bar" });
}

/// The same pair with the store on one arm. It no longer runs on every path,
/// so the copy is still one of the writes the load can read.
TEST (ConstantValuesTest, AStoreAfterAMemcpyOnOneArmDoesNotCoverIt)
{
	Settled m (R"(
define ptr @caller(i1 %c, ptr %p, i64 %n) {
entry:
  %o = call ptr @"mono.alloc.object"(ptr @vtable_Foo, i64 128, ptr @allocator)
  %f = getelementptr inbounds i8, ptr %o, i32 32
  call void @llvm.memcpy.p0.p0.i64(ptr %f, ptr %p, i64 8, i1 false)
  br i1 %c, label %wrote, label %merge
wrote:
  store ptr @vtable_Bar, ptr %f, align 8
  br label %merge
merge:
  %held = load ptr, ptr %f, align 8, !ask !0
  ret ptr %held
}
)");

	EXPECT_FALSE (m.complete ());
}

TEST (ConstantValuesTest, AMemcpyElsewhereLeavesTheFieldAlone)
{
	Settled m (R"(
define ptr @caller(i1 %c, ptr %p, i64 %n) {
entry:
  %o = call ptr @"mono.alloc.object"(ptr @vtable_Foo, i64 128, ptr @allocator)
  %f = getelementptr inbounds i8, ptr %o, i32 32
  %g = getelementptr inbounds i8, ptr %o, i32 64
  store ptr @vtable_Bar, ptr %f, align 8
  call void @llvm.memcpy.p0.p0.i64(ptr %g, ptr %p, i64 8, i1 false)
  %held = load ptr, ptr %f, align 8, !ask !0
  ret ptr %held
}
)");

	EXPECT_TRUE (m.complete ());
	EXPECT_TRUE (is_contained (m.reached (), "vtable_Bar"));
}

TEST (ConstantValuesTest, AValueCopyOverAFieldClearsIt)
{
	Settled m (R"(
define ptr @caller(i1 %c, ptr %p, i64 %n) {
entry:
  %o = call ptr @"mono.alloc.object"(ptr @vtable_Foo, i64 128, ptr @allocator)
  %f = getelementptr inbounds i8, ptr %o, i32 32
  store ptr @vtable_Bar, ptr %f, align 8
  call void @"mono.gc.wbarrier.value.copy"(ptr %f, ptr %p, i32 1, i64 8, ptr null)
  %held = load ptr, ptr %f, align 8, !ask !0
  ret ptr %held
}
)");

	EXPECT_FALSE (m.complete ());
}

TEST (ConstantValuesTest, AValueCopyElsewhereLeavesTheFieldAlone)
{
	Settled m (R"(
define ptr @caller(i1 %c, ptr %p, i64 %n) {
entry:
  %o = call ptr @"mono.alloc.object"(ptr @vtable_Foo, i64 128, ptr @allocator)
  %f = getelementptr inbounds i8, ptr %o, i32 32
  %g = getelementptr inbounds i8, ptr %o, i32 64
  store ptr @vtable_Bar, ptr %f, align 8
  call void @"mono.gc.wbarrier.value.copy"(ptr %g, ptr %p, i32 1, i64 8, ptr null)
  %held = load ptr, ptr %f, align 8, !ask !0
  ret ptr %held
}
)");

	EXPECT_TRUE (m.complete ());
	EXPECT_TRUE (is_contained (m.reached (), "vtable_Bar"));
}

TEST (ConstantValuesTest, AValueCopyOfAnUnreadableLengthClearsEveryField)
{
	Settled m (R"(
define ptr @caller(i1 %c, ptr %p, i64 %n) {
entry:
  %o = call ptr @"mono.alloc.object"(ptr @vtable_Foo, i64 128, ptr @allocator)
  %f = getelementptr inbounds i8, ptr %o, i32 32
  %g = getelementptr inbounds i8, ptr %o, i32 64
  store ptr @vtable_Bar, ptr %f, align 8
  call void @"mono.gc.wbarrier.value.copy"(ptr %g, ptr %p, i32 1, i64 %n, ptr null)
  %held = load ptr, ptr %f, align 8, !ask !0
  ret ptr %held
}
)");

	EXPECT_FALSE (m.complete ());
}

TEST (ConstantValuesTest, AValueCopyReadingTheObjectIsNotAWrite)
{
	Settled m (R"(
define ptr @caller(i1 %c, ptr %p, i64 %n) {
entry:
  %o = call ptr @"mono.alloc.object"(ptr @vtable_Foo, i64 128, ptr @allocator)
  %f = getelementptr inbounds i8, ptr %o, i32 32
  store ptr @vtable_Bar, ptr %f, align 8
  call void @"mono.gc.wbarrier.value.copy"(ptr %p, ptr %f, i32 1, i64 8, ptr null)
  %held = load ptr, ptr %f, align 8, !ask !0
  ret ptr %held
}
)");

	EXPECT_TRUE (m.complete ());
	EXPECT_TRUE (is_contained (m.reached (), "vtable_Bar"));
}

TEST (ConstantValuesTest, SourcesNameValuesWhereValueDoesNot)
{
	Settled m (R"(
define ptr @caller(i1 %c, ptr %p, i64 %n) {
entry:
  br i1 %c, label %left, label %right
left:
  %a = call ptr @"mono.alloc.object"(ptr @vtable_Foo, i64 128, ptr @allocator)
  br label %merge
right:
  %b = call ptr @"mono.alloc.object"(ptr @vtable_Bar, i64 128, ptr @allocator)
  br label %merge
merge:
  %held = phi ptr [ %a, %left ], [ %b, %right ], !ask !0
  ret ptr %held
}
)");

	EXPECT_EQ (m.answer (), "-");
	EXPECT_TRUE (m.complete ());
	EXPECT_EQ (m.reached (), (std::vector<std::string> { "%a", "%b" }));
}

/// Nothing bounds the set, so a wide merge keeps every value it is reached by
/// and stays complete.
TEST (ConstantValuesTest, AWideMergeKeepsEveryValue)
{
	Settled m (R"(
define i64 @caller(i1 %c, ptr %p, i64 %n) {
entry:
  switch i64 %n, label %merge [ i64 0, label %b0
                                i64 1, label %b1
                                i64 2, label %b2
                                i64 3, label %b3
                                i64 4, label %b4
                                i64 5, label %b5
                                i64 6, label %b6
                                i64 7, label %b7
                                i64 8, label %b8 ]
b0:
  br label %merge
b1:
  br label %merge
b2:
  br label %merge
b3:
  br label %merge
b4:
  br label %merge
b5:
  br label %merge
b6:
  br label %merge
b7:
  br label %merge
b8:
  br label %merge
merge:
  %held = phi i64 [ 100, %entry ], [ 0, %b0 ], [ 1, %b1 ], [ 2, %b2 ],
                  [ 3, %b3 ], [ 4, %b4 ], [ 5, %b5 ], [ 6, %b6 ],
                  [ 7, %b7 ], [ 8, %b8 ], !ask !0
  ret i64 %held
}
)");

	EXPECT_EQ (m.answer (), "-");
	EXPECT_TRUE (m.complete ());
	EXPECT_EQ (m.sources ().sources.size (), 10u);
}

} // namespace
} // namespace test
} // namespace mono

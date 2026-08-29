/*
 * Tests for operand_class () and exact_class (), the walk that says what class
 * a value is, including where the value is a merge.
 *
 * Each case gives its IR as text and names the value it asks about with an
 * `!ask` mark.
 *
 * A class the walk reads off metadata is a fake MonoClass pointer of the
 * test's own choosing. A class read off an allocation's vtable operand is
 * checked for marshal-by-ref and COM. That check reads real fields, so a case
 * with an allocation boots a runtime and marks a real class.
 */

#include "analysis/constant-values.hpp"
#include "analysis/operand-class.hpp"

#include "harness.hpp"
#include "method-symbols.hpp"
#include "passes/alloc-func.hpp"

#include <mono/metadata/appdomain.h>
#include <mono/metadata/class-internals.h>
#include <mono/metadata/class.h>

#include <llvm/ADT/STLExtras.h>
#include <llvm/AsmParser/Parser.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/IRBuilder.h>
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

/*
 * The queries take the values a function settled to, and each case here parses
 * one function and asks about it once. So the four below settle the function on
 * each call rather than keeping an answer between them.
 */

ConstantValues
settled_for (const Function &f)
{
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

	return MonoConstantValues ().run (const_cast<Function &> (f), fam);
}

std::pair<MonoClass *, bool>
operand_class_of (Value *v, const Function &f)
{
	return operand_class (v, f, settled_for (f));
}

MonoClass *
exact_class_of (Value *v, const Function &f)
{
	return exact_class (v, f, settled_for (f));
}

FieldValues
field_load_values_of (LoadInst &load)
{
	return field_load_values (load, settled_for (*load.getFunction ()));
}

/// Two classes a mark can name, written `!1` and `!2` in the IR below. The
/// walk only ever compares them, so what they point at is never read.
MonoClass *const classX = reinterpret_cast<MonoClass *> (4096);
[[maybe_unused]] MonoClass *const classY = reinterpret_cast<MonoClass *> (8192);

/// What every module below declares. `!1` and `!2` are the two class pointers
/// above, written the way `mark_exact_class ()` encodes one.
///
/// The allocation declaration carries the attributes `alloc_func_decl ()` puts
/// on it. The alloc kind is what tells the walk that an untouched field reads
/// as zero, so a case that drops it gets a different answer.
constexpr const char *preamble = R"(
declare noalias ptr @"mono.alloc.object"(ptr, i64, ptr)
    allockind("alloc,zeroed") memory(argmem: read, inaccessiblemem: readwrite)
declare void @opaque(ptr)
@vtable = external global i8
!0 = !{}
!1 = !{i64 4096}
!2 = !{i64 8192}
)";

/// A module a case asks its question about.
struct Parsed {
	LLVMContext context;
	std::unique_ptr<Module> module;
	Function *caller = nullptr;

	/// \p vtable_class marks the `@vtable` global the allocation sites name.
	explicit Parsed (const std::string &ir, MonoClass *vtable_class = nullptr)
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

		if (vtable_class != nullptr)
			mark_class_reference (*module->getNamedGlobal ("vtable"), vtable_class);

		std::string complaint;
		raw_string_ostream out (complaint);

		EXPECT_FALSE (verifyModule (*module, &out)) << complaint;
	}

	/// The instruction the module marked \p name.
	Instruction &marked (StringRef name) const
	{
		for (Instruction &at : instructions (*caller))
			if (at.getMetadata (name) != nullptr)
				return at;

		ADD_FAILURE () << "no instruction carries !" << name.str ();
		return *caller->getEntryBlock ().begin ();
	}

	/// The instruction the case asks about.
	Instruction &question () const { return marked ("ask"); }
};

TEST (OperandClassTest, PhiWithAgreeingExactArmsAnswersTheClass)
{
	Parsed m (R"(
define ptr @caller(i1 %c, ptr %p) {
entry:
  br i1 %c, label %left, label %right
left:
  %a = load ptr, ptr %p, !mono.exact.class !1
  br label %merge
right:
  %b = load ptr, ptr %p, !mono.exact.class !1
  br label %merge
merge:
  %held = phi ptr [ %a, %left ], [ %b, %right ], !ask !0
  ret ptr %held
}
)");

	auto [klass, exact] = operand_class_of (&m.question (), *m.caller);

	EXPECT_EQ (klass, classX);
	EXPECT_TRUE (exact);
	EXPECT_EQ (exact_class_of (&m.question (), *m.caller), classX);
}

TEST (OperandClassTest, PhiWithDisagreeingArmsAnswersNoClass)
{
	Parsed m (R"(
define ptr @caller(i1 %c, ptr %p) {
entry:
  br i1 %c, label %left, label %right
left:
  %a = load ptr, ptr %p, !mono.exact.class !1
  br label %merge
right:
  %b = load ptr, ptr %p, !mono.exact.class !2
  br label %merge
merge:
  %held = phi ptr [ %a, %left ], [ %b, %right ], !ask !0
  ret ptr %held
}
)");

	EXPECT_EQ (operand_class_of (&m.question (), *m.caller).first, nullptr);
	EXPECT_EQ (exact_class_of (&m.question (), *m.caller), nullptr);
}

TEST (OperandClassTest, SelectWithAgreeingArmsAnswersTheClass)
{
	Parsed m (R"(
define ptr @caller(i1 %c, ptr %p) {
entry:
  %a = load ptr, ptr %p, !mono.exact.class !1
  %b = load ptr, ptr %p, !mono.exact.class !1
  %held = select i1 %c, ptr %a, ptr %b, !ask !0
  ret ptr %held
}
)");

	auto [klass, exact] = operand_class_of (&m.question (), *m.caller);

	EXPECT_EQ (klass, classX);
	EXPECT_TRUE (exact);
}

/// `operand_class ()` never resolves a merge through a null arm: an `isinst`
/// reads a null itself, so a class guessed here is a wrong answer rather than a
/// missing optimization.
TEST (OperandClassTest, OperandClassAnswersNoClassAcrossANullArm)
{
	Parsed m (R"(
define ptr @caller(i1 %c, ptr %p) {
entry:
  br i1 %c, label %left, label %right
left:
  %a = load ptr, ptr %p, !mono.exact.class !1
  br label %merge
right:
  br label %merge
merge:
  %held = phi ptr [ %a, %left ], [ null, %right ], !ask !0
  ret ptr %held
}
)");

	EXPECT_EQ (operand_class_of (&m.question (), *m.caller).first, nullptr);
}

/// `exact_class ()` is for a caller about to dereference the value, and a null
/// arm faults before that dereference runs. So it reads past the null arm to
/// the class every other arm agrees on.
TEST (OperandClassTest, ExactClassIgnoresANullArm)
{
	Parsed m (R"(
define ptr @caller(i1 %c, ptr %p) {
entry:
  br i1 %c, label %left, label %right
left:
  %a = load ptr, ptr %p, !mono.exact.class !1
  br label %merge
right:
  br label %merge
merge:
  %held = phi ptr [ %a, %left ], [ null, %right ], !ask !0
  ret ptr %held
}
)");

	EXPECT_EQ (exact_class_of (&m.question (), *m.caller), classX);
}

/// The mutual cycle a loop-carried receiver forms must not defeat the walk.
/// Neither must the null `%carried` starts with, because the header tests for
/// null before the value can reach a dereference.
TEST (OperandClassTest, ExactClassResolvesAMutualCycleThroughNull)
{
	Parsed m (R"(
define ptr @caller(i1 %c, ptr %p) {
entry:
  br label %header
header:
  %carried = phi ptr [ null, %entry ], [ %current, %latch ], !carried !0
  %empty = icmp eq ptr %carried, null
  br i1 %empty, label %make, label %reuse
make:
  %fresh = load ptr, ptr %p, !mono.exact.class !1
  br label %merge
reuse:
  br label %merge
merge:
  %current = phi ptr [ %fresh, %make ], [ %carried, %reuse ], !ask !0
  br i1 %c, label %latch, label %exit
latch:
  br label %header
exit:
  ret ptr %current
}
)");

	EXPECT_EQ (exact_class_of (&m.question (), *m.caller), classX);
	EXPECT_EQ (exact_class_of (&m.marked ("carried"), *m.caller), classX);
}

/// `operand_class ()` does not get the same answer for the same cycle, because
/// it does not ignore the null `%carried` starts with. The null incoming
/// settles `%carried` at no class, which settles `%current` at no class too.
TEST (OperandClassTest, OperandClassDoesNotResolveTheSameCycle)
{
	Parsed m (R"(
define ptr @caller(i1 %c, ptr %p) {
entry:
  br label %header
header:
  %carried = phi ptr [ null, %entry ], [ %current, %latch ]
  %empty = icmp eq ptr %carried, null
  br i1 %empty, label %make, label %reuse
make:
  %fresh = load ptr, ptr %p, !mono.exact.class !1
  br label %merge
reuse:
  br label %merge
merge:
  %current = phi ptr [ %fresh, %make ], [ %carried, %reuse ], !ask !0
  br i1 %c, label %latch, label %exit
latch:
  br label %header
exit:
  ret ptr %current
}
)");

	EXPECT_EQ (operand_class_of (&m.question (), *m.caller).first, nullptr);
}

/// A cycle carrying two different classes must not answer either one, with or
/// without the null rule relaxed.
TEST (OperandClassTest, ExactClassRefusesACycleThatDisagrees)
{
	Parsed m (R"(
define ptr @caller(i1 %c, ptr %p) {
entry:
  %other = load ptr, ptr %p, !mono.exact.class !1
  br label %header
header:
  %carried = phi ptr [ %other, %entry ], [ %current, %latch ]
  %empty = icmp eq ptr %carried, null
  br i1 %empty, label %make, label %reuse
make:
  %fresh = load ptr, ptr %p, !mono.exact.class !2
  br label %merge
reuse:
  br label %merge
merge:
  %current = phi ptr [ %fresh, %make ], [ %carried, %reuse ], !ask !0
  br i1 %c, label %latch, label %exit
latch:
  br label %header
exit:
  ret ptr %current
}
)");

	EXPECT_EQ (exact_class_of (&m.question (), *m.caller), nullptr);
}

/// A chain of \p levels merges, each phi's two incoming edges carrying the phi
/// from the level above.
std::string
chain_of_merges (unsigned levels)
{
	std::string ir = R"(
define ptr @caller(i1 %c, ptr %p) {
entry:
  %held0 = load ptr, ptr %p, !mono.exact.class !1
  br i1 %c, label %left1, label %right1
)";

	for (unsigned level = 1; level <= levels; ++level) {
		std::string at = std::to_string (level);
		std::string above = std::to_string (level - 1);
		std::string next = std::to_string (level + 1);

		ir += "left" + at + ":\n  br label %join" + at + "\n";
		ir += "right" + at + ":\n  br label %join" + at + "\n";
		ir += "join" + at + ":\n  %held" + at + " = phi ptr [ %held" + above
		      + ", %left" + at + " ], [ %held" + above + ", %right" + at + " ]";
		ir += level == levels
		              ? ", !ask !0\n  ret ptr %held" + at + "\n"
		              : "\n  br i1 %c, label %left" + next + ", label %right" + next + "\n";
	}

	return ir + "}\n";
}

/// A walk that read the merges on demand would double its work at every level.
/// This one visits each phi a bounded number of times and answers.
TEST (OperandClassTest, ADeepChainOfMergesAnswersTheClassItCarries)
{
	Parsed m (chain_of_merges (25));

	EXPECT_EQ (exact_class_of (&m.question (), *m.caller), classX);
}

/// A function with one call to the object-allocation builtin, the vtable
/// operand a global marked with \p klass and no other class mark on the call.
///
/// `changeToInvokeAndSplitBasicBlock ()` builds an `InvokeInst` from a
/// `CallInst` the same way, keeping the same operands and copying no
/// metadata, so a plain call answers the same question the invoke it can
/// become would.
struct AllocationModule {
	std::unique_ptr<LLVMContext> context = std::make_unique<LLVMContext> ();
	std::unique_ptr<Module> module;
	Function *caller = nullptr;
	CallInst *site = nullptr;

	AllocationModule (MonoClass *klass, StringRef decl_name)
	{
		module = std::make_unique<Module> ("alloc", *context);

		Type *ptr = PointerType::get (*context, 0);
		Type *word = Type::getInt64Ty (*context);

		Function *decl = Function::Create (
			FunctionType::get (ptr, { ptr, word, ptr }, false),
			GlobalValue::ExternalLinkage, decl_name, module.get ());

		auto *vtable = new GlobalVariable (*module, Type::getInt8Ty (*context), false,
		                                   GlobalValue::ExternalLinkage, nullptr, "vtable");
		mark_class_reference (*vtable, klass);

		caller = Function::Create (FunctionType::get (ptr, {}, false),
		                           GlobalValue::ExternalLinkage, "caller", module.get ());

		IRBuilder<> b (BasicBlock::Create (*context, "entry", caller));

		site = b.CreateCall (
			decl, { vtable, ConstantInt::get (word, 64), ConstantPointerNull::get (
				                                       cast<PointerType> (ptr)) });
		b.CreateRet (site);
	}
};

TEST (OperandClassTest, AllocationSiteAnswersItsVtableClassWithNoMark)
{
	mono::test::init_runtime ();

	AllocationModule m (mono_defaults.object_class, alloc_object_name);
	auto [klass, exact] = operand_class_of (m.site, *m.caller);

	EXPECT_EQ (klass, mono_defaults.object_class);
	EXPECT_TRUE (exact);
}

/// The kept form names the same allocation as the ordinary one and answers
/// its class the same way.
TEST (OperandClassTest, KeptAllocationSiteAnswersItsVtableClassWithNoMark)
{
	mono::test::init_runtime ();

	AllocationModule m (mono_defaults.object_class, alloc_object_kept_name);

	EXPECT_EQ (operand_class_of (m.site, *m.caller).first, mono_defaults.object_class);
}

/// A marshal-by-ref class's allocator can answer with a transparent proxy
/// instead of an instance of the class it was asked to allocate, so the
/// vtable operand must not settle the class here the way it does above.
TEST (OperandClassTest, AllocationSiteRefusesAMarshalByRefClass)
{
	mono::test::init_runtime ();

	MonoClass *klass = mono_class_from_name (mono_get_corlib (), "System", "MarshalByRefObject");
	ASSERT_NE (klass, nullptr);

	AllocationModule m (klass, alloc_object_name);

	EXPECT_EQ (operand_class_of (m.site, *m.caller).first, nullptr);
}

/*
 * The cases below read a field off an allocation. The walk gathers the stores
 * to a field by walking the object's uses, and reads the dominator tree to
 * decide which of them the load is bound to see.
 */

/// The split this whole rule stands on. The store is on one arm, so a path
/// reaches the load without it and the allocation's zero is still one of the
/// values the field holds. `operand_class ()` must answer no class for that,
/// the way it would for an `isinst` reading this field. `exact_class ()` is
/// for a caller that dereferences right after, so the null path faults there
/// before it matters and that entry reads past it to the one store's class.
TEST (OperandClassTest, LoadWithOneStoreSplitsOperandClassFromExactClass)
{
	mono::test::init_runtime ();

	Parsed m (R"(
define ptr @caller(i1 %c, ptr %p) {
entry:
  %o = call ptr @"mono.alloc.object"(ptr @vtable, i64 64, ptr null)
  %f = getelementptr inbounds i8, ptr %o, i64 8
  %v = load ptr, ptr %p, !mono.exact.class !1
  br i1 %c, label %wrote, label %merge
wrote:
  store ptr %v, ptr %f, align 8
  br label %merge
merge:
  %held = load ptr, ptr %f, align 8, !ask !0
  ret ptr %held
}
)",
	          mono_defaults.object_class);

	EXPECT_EQ (operand_class_of (&m.question (), *m.caller).first, nullptr);
	EXPECT_EQ (exact_class_of (&m.question (), *m.caller), classX);
}

/// The same shape with the store in a straight line. It dominates the load,
/// so the zero is gone and `operand_class ()` answers what `exact_class ()`
/// does.
TEST (OperandClassTest, LoadWithADominatingStoreAnswersOperandClass)
{
	mono::test::init_runtime ();

	Parsed m (R"(
define ptr @caller(ptr %p) {
entry:
  %o = call ptr @"mono.alloc.object"(ptr @vtable, i64 64, ptr null)
  %f = getelementptr inbounds i8, ptr %o, i64 8
  %v = load ptr, ptr %p, !mono.exact.class !1
  store ptr %v, ptr %f, align 8
  %held = load ptr, ptr %f, align 8, !ask !0
  ret ptr %held
}
)",
	          mono_defaults.object_class);

	EXPECT_EQ (operand_class_of (&m.question (), *m.caller).first, classX);
	EXPECT_EQ (exact_class_of (&m.question (), *m.caller), classX);
}

/// The class every store agrees on settles the load even where the objects
/// those stores write are themselves different, because the rule is about
/// what class the field holds, not which object. This reads through
/// `exact_class ()`, because `operand_class ()` answers no class here too:
/// the field's zero-filled initial value is still one of the arms.
TEST (OperandClassTest, LoadAnswersWhereTwoStoresNameDifferentObjectsOfTheSameClass)
{
	mono::test::init_runtime ();

	Parsed m (R"(
define ptr @caller(ptr %p) {
entry:
  %o = call ptr @"mono.alloc.object"(ptr @vtable, i64 64, ptr null)
  %f = getelementptr inbounds i8, ptr %o, i64 8
  %first = load ptr, ptr %p, !mono.exact.class !1
  %second = load ptr, ptr %p, !mono.exact.class !1
  store ptr %first, ptr %f, align 8
  store ptr %second, ptr %f, align 8
  %held = load ptr, ptr %f, align 8, !ask !0
  ret ptr %held
}
)",
	          mono_defaults.object_class);

	EXPECT_EQ (exact_class_of (&m.question (), *m.caller), classX);
}

/// The second store kills the first, so the class the load reads is the one
/// the surviving store names rather than no class at all.
TEST (OperandClassTest, LoadAnswersTheLastOfTwoStoresThatDisagree)
{
	mono::test::init_runtime ();

	Parsed m (R"(
define ptr @caller(ptr %p) {
entry:
  %o = call ptr @"mono.alloc.object"(ptr @vtable, i64 64, ptr null)
  %f = getelementptr inbounds i8, ptr %o, i64 8
  %first = load ptr, ptr %p, !mono.exact.class !1
  %second = load ptr, ptr %p, !mono.exact.class !2
  store ptr %first, ptr %f, align 8
  store ptr %second, ptr %f, align 8
  %held = load ptr, ptr %f, align 8, !ask !0
  ret ptr %held
}
)",
	          mono_defaults.object_class);

	EXPECT_EQ (operand_class_of (&m.question (), *m.caller).first, classY);
	EXPECT_EQ (exact_class_of (&m.question (), *m.caller), classY);
}

/// The escape guard is what this rule stands on: passing the allocation to a
/// call hands its address to code this walk cannot read, so the answer must
/// go to no class even though the one store still in sight names a settled
/// class.
TEST (OperandClassTest, LoadAnswersNoClassWhereTheAllocationEscapesToACall)
{
	mono::test::init_runtime ();

	Parsed m (R"(
define ptr @caller(ptr %p) {
entry:
  %o = call ptr @"mono.alloc.object"(ptr @vtable, i64 64, ptr null)
  %f = getelementptr inbounds i8, ptr %o, i64 8
  %v = load ptr, ptr %p, !mono.exact.class !1
  store ptr %v, ptr %f, align 8
  call void @opaque(ptr %o)
  %held = load ptr, ptr %f, align 8, !ask !0
  ret ptr %held
}
)",
	          mono_defaults.object_class);

	EXPECT_EQ (operand_class_of (&m.question (), *m.caller).first, nullptr);
	EXPECT_EQ (exact_class_of (&m.question (), *m.caller), nullptr);
}

/// Writing the allocation's address out to another object does not put the
/// field beyond reach. Nothing between the store and the load can act on the
/// address that left, and a store through a pointer the function was handed
/// cannot land in memory this call made.
TEST (OperandClassTest, LoadAnswersWhereTheAllocationIsStoredIntoAnotherObject)
{
	mono::test::init_runtime ();

	Parsed m (R"(
define ptr @caller(ptr %p) {
entry:
  %o = call ptr @"mono.alloc.object"(ptr @vtable, i64 64, ptr null)
  %f = getelementptr inbounds i8, ptr %o, i64 8
  %v = load ptr, ptr %p, !mono.exact.class !1
  store ptr %v, ptr %f, align 8
  store ptr %o, ptr %p, align 8
  %held = load ptr, ptr %f, align 8, !ask !0
  ret ptr %held
}
)",
	          mono_defaults.object_class);

	EXPECT_EQ (operand_class_of (&m.question (), *m.caller).first, classX);
	EXPECT_EQ (exact_class_of (&m.question (), *m.caller), classX);
}

/// A call the write barrier's own attributes mark as safe must not stop the
/// walk from reaching the store behind it. The barrier states its effects at
/// the function level rather than on its parameter, so the declaration below
/// does too.
TEST (OperandClassTest, LoadAnswersWhereTheFieldAddressReachesACallThatCannotWriteOrCapture)
{
	mono::test::init_runtime ();

	Parsed m (R"(
declare void @sink(ptr captures(none)) memory(argmem: read)
define ptr @caller(ptr %p) {
entry:
  %o = call ptr @"mono.alloc.object"(ptr @vtable, i64 64, ptr null)
  %f = getelementptr inbounds i8, ptr %o, i64 8
  %v = load ptr, ptr %p, !mono.exact.class !1
  store ptr %v, ptr %f, align 8
  call void @sink(ptr %f)
  %held = load ptr, ptr %f, align 8, !ask !0
  ret ptr %held
}
)",
	          mono_defaults.object_class);

	EXPECT_EQ (exact_class_of (&m.question (), *m.caller), classX);
}

/// A callee whose memory effects say it can write argument memory is refused
/// even though it does not keep the pointer. Not capturing it only bounds how
/// long the callee can hold it, not whether the call itself writes through it.
TEST (OperandClassTest, LoadAnswersNoClassWhereTheCallMayWriteTheFieldAddress)
{
	mono::test::init_runtime ();

	Parsed m (R"(
declare void @sink(ptr captures(none)) memory(argmem: readwrite)
define ptr @caller(ptr %p) {
entry:
  %o = call ptr @"mono.alloc.object"(ptr @vtable, i64 64, ptr null)
  %f = getelementptr inbounds i8, ptr %o, i64 8
  %v = load ptr, ptr %p, !mono.exact.class !1
  store ptr %v, ptr %f, align 8
  call void @sink(ptr %f)
  %held = load ptr, ptr %f, align 8, !ask !0
  ret ptr %held
}
)",
	          mono_defaults.object_class);

	EXPECT_EQ (operand_class_of (&m.question (), *m.caller).first, nullptr);
	EXPECT_EQ (exact_class_of (&m.question (), *m.caller), nullptr);
}

/// A callee that keeps the field address but cannot write through it leaves
/// the store in reach. Keeping a pointer settles nothing on its own: a write
/// through the copy it kept is a later call, and that call is what the walk
/// stops at.
TEST (OperandClassTest, LoadAnswersWhereTheCallMayOnlyCaptureTheFieldAddress)
{
	mono::test::init_runtime ();

	Parsed m (R"(
declare void @sink(ptr) memory(argmem: read)
define ptr @caller(ptr %p) {
entry:
  %o = call ptr @"mono.alloc.object"(ptr @vtable, i64 64, ptr null)
  %f = getelementptr inbounds i8, ptr %o, i64 8
  %v = load ptr, ptr %p, !mono.exact.class !1
  store ptr %v, ptr %f, align 8
  call void @sink(ptr %f)
  %held = load ptr, ptr %f, align 8, !ask !0
  ret ptr %held
}
)",
	          mono_defaults.object_class);

	EXPECT_EQ (operand_class_of (&m.question (), *m.caller).first, classX);
	EXPECT_EQ (exact_class_of (&m.question (), *m.caller), classX);
}

/// A pointer compare writes nothing and yields an `i1`, so it is not a route
/// by which the object is written, whether it compares the allocation itself
/// or one of its fields - both are exactly what the translator's own null
/// test emits ahead of a dereference.
TEST (OperandClassTest, LoadAnswersWhereTheAllocationOrAFieldIsCompared)
{
	mono::test::init_runtime ();

	Parsed m (R"(
define ptr @caller(ptr %p) {
entry:
  %o = call ptr @"mono.alloc.object"(ptr @vtable, i64 64, ptr null)
  %f = getelementptr inbounds i8, ptr %o, i64 8
  %v = load ptr, ptr %p, !mono.exact.class !1
  store ptr %v, ptr %f, align 8
  %base_is_null = icmp eq ptr %o, null
  %field_is_null = icmp eq ptr %f, null
  %held = load ptr, ptr %f, align 8, !ask !0
  ret ptr %held
}
)",
	          mono_defaults.object_class);

	EXPECT_EQ (exact_class_of (&m.question (), *m.caller), classX);
}

/// `ptrtoint` writes nothing. Turning the integer back into a pointer and
/// writing through it takes an instruction or a call, and either of those is
/// what the walk stops at.
TEST (OperandClassTest, LoadAnswersWhereTheAllocationIsConvertedToAnInteger)
{
	mono::test::init_runtime ();

	Parsed m (R"(
define ptr @caller(ptr %p) {
entry:
  %o = call ptr @"mono.alloc.object"(ptr @vtable, i64 64, ptr null)
  %f = getelementptr inbounds i8, ptr %o, i64 8
  %v = load ptr, ptr %p, !mono.exact.class !1
  store ptr %v, ptr %f, align 8
  %word = ptrtoint ptr %o to i64
  %held = load ptr, ptr %f, align 8, !ask !0
  ret ptr %held
}
)",
	          mono_defaults.object_class);

	EXPECT_EQ (operand_class_of (&m.question (), *m.caller).first, classX);
	EXPECT_EQ (exact_class_of (&m.question (), *m.caller), classX);
}

/// Resolving the base through the phi to both allocations, and reading each
/// one's own field store, is what `resolve_base_candidates ()` adds over a
/// single allocation base.
TEST (OperandClassTest, LoadThroughAPhiOfAllocationsAnswersTheClass)
{
	mono::test::init_runtime ();

	Parsed m (R"(
define ptr @caller(i1 %c, ptr %p) {
entry:
  br i1 %c, label %left, label %right
left:
  %a = call ptr @"mono.alloc.object"(ptr @vtable, i64 64, ptr null)
  %av = load ptr, ptr %p, !mono.exact.class !1
  %af = getelementptr inbounds i8, ptr %a, i64 8
  store ptr %av, ptr %af, align 8
  br label %merge
right:
  %b = call ptr @"mono.alloc.object"(ptr @vtable, i64 64, ptr null)
  %bv = load ptr, ptr %p, !mono.exact.class !1
  %bf = getelementptr inbounds i8, ptr %b, i64 8
  store ptr %bv, ptr %bf, align 8
  br label %merge
merge:
  %o = phi ptr [ %a, %left ], [ %b, %right ]
  %f = getelementptr inbounds i8, ptr %o, i64 8
  %held = load ptr, ptr %f, align 8, !ask !0
  ret ptr %held
}
)",
	          mono_defaults.object_class);

	EXPECT_EQ (exact_class_of (&m.question (), *m.caller), classX);
}

/// One arm writes its allocation's address out to another object before the
/// merge. Nothing downstream of either store can reach the field, so the
/// answer is the one the arm above gives without that store.
TEST (OperandClassTest, LoadThroughAPhiOfAllocationsAnswersTheClassWhereOneEscapes)
{
	mono::test::init_runtime ();

	Parsed m (R"(
define ptr @caller(i1 %c, ptr %p) {
entry:
  br i1 %c, label %left, label %right
left:
  %a = call ptr @"mono.alloc.object"(ptr @vtable, i64 64, ptr null)
  %av = load ptr, ptr %p, !mono.exact.class !1
  %af = getelementptr inbounds i8, ptr %a, i64 8
  store ptr %av, ptr %af, align 8
  store ptr %a, ptr %p, align 8
  br label %merge
right:
  %b = call ptr @"mono.alloc.object"(ptr @vtable, i64 64, ptr null)
  %bv = load ptr, ptr %p, !mono.exact.class !1
  %bf = getelementptr inbounds i8, ptr %b, i64 8
  store ptr %bv, ptr %bf, align 8
  br label %merge
merge:
  %o = phi ptr [ %a, %left ], [ %b, %right ]
  %f = getelementptr inbounds i8, ptr %o, i64 8
  %held = load ptr, ptr %f, align 8, !ask !0
  ret ptr %held
}
)",
	          mono_defaults.object_class);

	EXPECT_EQ (exact_class_of (&m.question (), *m.caller), classX);
}

/// Agreeing on two different objects of the same class settles a merge;
/// agreeing on nothing must not.
TEST (OperandClassTest, LoadThroughAPhiOfAllocationsAnswersNoClassWhereTheyDisagree)
{
	mono::test::init_runtime ();

	Parsed m (R"(
define ptr @caller(i1 %c, ptr %p) {
entry:
  br i1 %c, label %left, label %right
left:
  %a = call ptr @"mono.alloc.object"(ptr @vtable, i64 64, ptr null)
  %av = load ptr, ptr %p, !mono.exact.class !1
  %af = getelementptr inbounds i8, ptr %a, i64 8
  store ptr %av, ptr %af, align 8
  br label %merge
right:
  %b = call ptr @"mono.alloc.object"(ptr @vtable, i64 64, ptr null)
  %bv = load ptr, ptr %p, !mono.exact.class !2
  %bf = getelementptr inbounds i8, ptr %b, i64 8
  store ptr %bv, ptr %bf, align 8
  br label %merge
merge:
  %o = phi ptr [ %a, %left ], [ %b, %right ]
  %f = getelementptr inbounds i8, ptr %o, i64 8
  %held = load ptr, ptr %f, align 8, !ask !0
  ret ptr %held
}
)",
	          mono_defaults.object_class);

	EXPECT_EQ (operand_class_of (&m.question (), *m.caller).first, nullptr);
	EXPECT_EQ (exact_class_of (&m.question (), *m.caller), nullptr);
}

/// The base of the field this reads is itself a load, of the outer
/// allocation's own field, which the inner allocation's address was stored
/// into once. Forwarding that read back to the inner allocation is what makes
/// the store into the inner object's field reachable.
TEST (OperandClassTest, LoadThroughALoadAsBaseAnswersTheClass)
{
	mono::test::init_runtime ();

	Parsed m (R"(
define ptr @caller(ptr %p) {
entry:
  %outer = call ptr @"mono.alloc.object"(ptr @vtable, i64 64, ptr null)
  %inner = call ptr @"mono.alloc.object"(ptr @vtable, i64 64, ptr null)
  %outer_field = getelementptr inbounds i8, ptr %outer, i64 8
  store ptr %inner, ptr %outer_field, align 8
  %read_back = load ptr, ptr %outer_field, align 8
  %inner_field = getelementptr inbounds i8, ptr %read_back, i64 16
  %v = load ptr, ptr %p, !mono.exact.class !1
  store ptr %v, ptr %inner_field, align 8
  %held = load ptr, ptr %inner_field, align 8, !ask !0
  ret ptr %held
}
)",
	          mono_defaults.object_class);

	EXPECT_EQ (operand_class_of (&m.question (), *m.caller).first, classX);
	EXPECT_EQ (exact_class_of (&m.question (), *m.caller), classX);
}

/// The load's base is a phi that is itself part of the mutual cycle
/// `ExactClassResolvesAMutualCycleThroughNull` above gates for a bare
/// allocation. `resolve_base_candidates ()` must terminate on that cycle,
/// the same way `walk_operand_class ()` does, rather than loop forever
/// chasing the phi's own back edge.
TEST (OperandClassTest, LoadThroughACyclicPhiBaseTerminatesAndAnswers)
{
	mono::test::init_runtime ();

	Parsed m (R"(
define ptr @caller(i1 %c, ptr %p) {
entry:
  br label %header
header:
  %carried = phi ptr [ null, %entry ], [ %current, %latch ]
  %empty = icmp eq ptr %carried, null
  br i1 %empty, label %make, label %reuse
make:
  %fresh = call ptr @"mono.alloc.object"(ptr @vtable, i64 64, ptr null)
  %v = load ptr, ptr %p, !mono.exact.class !1
  %vf = getelementptr inbounds i8, ptr %fresh, i64 8
  store ptr %v, ptr %vf, align 8
  br label %merge
reuse:
  br label %merge
merge:
  %current = phi ptr [ %fresh, %make ], [ %carried, %reuse ]
  br i1 %c, label %latch, label %exit
latch:
  br label %header
exit:
  %f = getelementptr inbounds i8, ptr %current, i64 8
  %held = load ptr, ptr %f, align 8, !ask !0
  ret ptr %held
}
)",
	          mono_defaults.object_class);

	EXPECT_EQ (exact_class_of (&m.question (), *m.caller), classX);
}

/*
 * Below is `field_load_values ()`, the same walk `operand_class ()` runs over a
 * field load, exported for a caller that wants the values a store can leave
 * rather than the class they agree on. These cases answer with the store's own
 * value, so the stored value carries no class mark.
 */

TEST (OperandClassTest, FieldLoadValuesAnswersTheOneStore)
{
	mono::test::init_runtime ();

	Parsed m (R"(
define ptr @caller(ptr %p) {
entry:
  %o = call ptr @"mono.alloc.object"(ptr @vtable, i64 64, ptr null)
  %f = getelementptr inbounds i8, ptr %o, i64 8
  %v = load ptr, ptr %p, !stored !0
  store ptr %v, ptr %f, align 8
  %held = load ptr, ptr %f, align 8, !ask !0
  ret ptr %held
}
)",
	          mono_defaults.object_class);

	FieldValues got = field_load_values_of (cast<LoadInst> (m.question ()));

	ASSERT_EQ (got.values.size (), 1u);
	EXPECT_EQ (got.values[0], &m.marked ("stored"));
	EXPECT_TRUE (got.complete);
}

/// Two stores of two different values both answer, the same way
/// `matching_field_stores ()` gathers every store to the field rather than
/// the one a particular path executes.
TEST (OperandClassTest, FieldLoadValuesAnswersEveryDistinctStore)
{
	mono::test::init_runtime ();

	Parsed m (R"(
define ptr @caller(i1 %c, ptr %p) {
entry:
  %o = call ptr @"mono.alloc.object"(ptr @vtable, i64 64, ptr null)
  %f = getelementptr inbounds i8, ptr %o, i64 8
  br i1 %c, label %left, label %right
left:
  %v1 = load ptr, ptr %p, !first !0
  store ptr %v1, ptr %f, align 8
  br label %merge
right:
  %v2 = load ptr, ptr %p, !second !0
  store ptr %v2, ptr %f, align 8
  br label %merge
merge:
  %held = load ptr, ptr %f, align 8, !ask !0
  ret ptr %held
}
)",
	          mono_defaults.object_class);

	FieldValues got = field_load_values_of (cast<LoadInst> (m.question ()));

	ASSERT_EQ (got.values.size (), 2u);
	EXPECT_TRUE (is_contained (got.values, &m.marked ("first")));
	EXPECT_TRUE (is_contained (got.values, &m.marked ("second")));
}

/// The field's own zero-filled initial value is left out of the answer: a
/// field with one store still answers with that store alone, none of it a
/// null standing for the path where the load ran first.
TEST (OperandClassTest, FieldLoadValuesLeavesOutTheZeroFilledInitialValue)
{
	mono::test::init_runtime ();

	Parsed m (R"(
define ptr @caller(ptr %p) {
entry:
  %o = call ptr @"mono.alloc.object"(ptr @vtable, i64 64, ptr null)
  %f = getelementptr inbounds i8, ptr %o, i64 8
  %v = load ptr, ptr %p, !stored !0
  store ptr %v, ptr %f, align 8
  %held = load ptr, ptr %f, align 8, !ask !0
  ret ptr %held
}
)",
	          mono_defaults.object_class);

	FieldValues got = field_load_values_of (cast<LoadInst> (m.question ()));

	for (const Value *v : got.values)
		EXPECT_FALSE (isa<ConstantPointerNull> (v));
}

/// A field no store ever reaches answers empty, the same way `operand_class
/// ()` answers no class for it.
TEST (OperandClassTest, FieldLoadValuesIsEmptyWhereNoStoreReachesTheField)
{
	mono::test::init_runtime ();

	Parsed m (R"(
define ptr @caller(ptr %p) {
entry:
  %o = call ptr @"mono.alloc.object"(ptr @vtable, i64 64, ptr null)
  %f = getelementptr inbounds i8, ptr %o, i64 8
  %held = load ptr, ptr %f, align 8, !ask !0
  ret ptr %held
}
)",
	          mono_defaults.object_class);

	EXPECT_TRUE (field_load_values_of (cast<LoadInst> (m.question ())).values.empty ());
}

/// `field_load_values ()` runs the walk under `ClassRule::guessed`, so an
/// allocation that escapes to a call still answers with the store this walk
/// found before the escape, marked incomplete rather than dropped. A caller
/// reading `complete` false knows the field can hold more than this set.
TEST (OperandClassTest, FieldLoadValuesAnswersIncompleteWhereTheAllocationEscapesToACall)
{
	mono::test::init_runtime ();

	Parsed m (R"(
define ptr @caller(ptr %p) {
entry:
  %o = call ptr @"mono.alloc.object"(ptr @vtable, i64 64, ptr null)
  %f = getelementptr inbounds i8, ptr %o, i64 8
  %v = load ptr, ptr %p, !stored !0
  store ptr %v, ptr %f, align 8
  call void @opaque(ptr %o)
  %held = load ptr, ptr %f, align 8, !ask !0
  ret ptr %held
}
)",
	          mono_defaults.object_class);

	FieldValues got = field_load_values_of (cast<LoadInst> (m.question ()));

	ASSERT_EQ (got.values.size (), 1u);
	EXPECT_EQ (got.values[0], &m.marked ("stored"));
	EXPECT_FALSE (got.complete);
}

} // namespace
} // namespace test
} // namespace mono

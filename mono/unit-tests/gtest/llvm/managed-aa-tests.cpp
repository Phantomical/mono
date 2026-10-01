/* Tests ManagedAA without LLVM's other alias analyses. */

#include "analysis/managed-aa.hpp"
#include "analysis/operand-class.hpp"

#include "harness.hpp"
#include "method-symbols.hpp"

#include <mono/metadata/class-init.h>
#include <mono/metadata/class-internals.h>
#include <mono/metadata/class.h>
#include <mono/metadata/metadata-internals.h>
#include <mono/metadata/tabledefs.h>

#include <llvm/Analysis/AliasAnalysis.h>
#include <llvm/Analysis/MemoryLocation.h>
#include <llvm/AsmParser/Parser.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/ValueSymbolTable.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/Support/SourceMgr.h>

#include <gtest/gtest.h>

#include <cstring>
#include <memory>
#include <string>

using namespace llvm;

namespace mono {
namespace test {
namespace {

constexpr const char *preamble = R"(
@statics = external global i8
!100 = !{!101, !101, i64 0}
!101 = !{!"mono managed reference", !102}
!102 = !{!"mono managed memory"}
)";

MonoClass *
corlib_class (const char *space, const char *name)
{
	MonoClass *klass = mono_class_load_from_name (mono_defaults.corlib, space, name);

	mono_class_init_internal (klass);
	return klass;
}

MonoClass *
vector_of (MonoClass *element)
{
	MonoClass *klass = mono_class_create_array (element, 1);

	mono_class_init_internal (klass);
	return klass;
}

/// The offset String.Empty has in String's statics block.
int
empty_offset ()
{
	MonoClassField *field =
		mono_class_get_field_from_name_full (mono_defaults.string_class, "Empty", nullptr);

	EXPECT_NE (field, nullptr);
	return field != nullptr ? m_field_get_offset (field) : 0;
}

/// The offset of the first instance field of \p klass declared as \p type.
int
field_offset (MonoClass *klass, MonoClass *type)
{
	gpointer iter = nullptr;

	while (MonoClassField *field = mono_class_get_fields_internal (klass, &iter))
		if ((mono_field_get_flags (field) & FIELD_ATTRIBUTE_STATIC) == 0
		    && mono_class_from_mono_type_internal (mono_field_get_type_internal (field)) == type)
			return m_field_get_offset (field);

	ADD_FAILURE () << "no such field";
	return 0;
}

MonoClass *
exception_class ()
{
	return corlib_class ("System", "Exception");
}

/// \p ir with `EMPTY` replaced by String.Empty's offset, `MESSAGE` by
/// Exception's first string field's and `INNER` by its Exception field's.
std::string
at_empty (std::string ir)
{
	std::pair<const char *, int> names[] = {
		{ "EMPTY", empty_offset () },
		{ "MESSAGE", field_offset (exception_class (), mono_defaults.string_class) },
		{ "INNER", field_offset (exception_class (), exception_class ()) },
	};

	for (auto [name, offset] : names) {
		std::string spelled = std::to_string (offset);
		size_t length = strlen (name);

		for (size_t at = ir.find (name); at != std::string::npos; at = ir.find (name, at))
			ir.replace (at, length, spelled);
	}

	return ir;
}

struct Parsed {
	LLVMContext context;
	std::unique_ptr<Module> module;
	Function *caller = nullptr;

	/// \param params  the classes `@caller`'s arguments are declared with, in
	///                order.
	explicit Parsed (const std::string &ir, std::initializer_list<MonoClass *> params = {})
	{
		SMDiagnostic problem;

		module = parseAssemblyString (at_empty (ir) + preamble, problem, context);

		if (module == nullptr) {
			std::string complaint;
			raw_string_ostream out (complaint);

			problem.print ("", out);
			ADD_FAILURE () << complaint;
			return;
		}

		caller = module->getFunction ("caller");
		set_statics_class (*module->getNamedGlobal ("statics"), mono_defaults.string_class);

		std::vector<std::pair<unsigned, MonoClass *>> classes;
		unsigned index = 0;

		for (MonoClass *klass : params)
			classes.emplace_back (index++, klass);
		mark_parameter_classes (*caller, classes);

		std::string complaint;
		raw_string_ostream out (complaint);

		EXPECT_FALSE (verifyModule (*module, &out)) << complaint;
	}

	/// Tests whether 4-byte accesses through \p a and \p b can overlap.
	AliasResult alias (StringRef a, StringRef b)
	{
		if (caller == nullptr)
			return AliasResult::MayAlias;

		FunctionAnalysisManager fam;
		ModuleAnalysisManager mam;
		LoopAnalysisManager lam;
		CGSCCAnalysisManager cgam;
		PassBuilder pb;

		fam.registerPass ([] {
			AAManager aa;

			aa.registerFunctionAnalysis<ManagedAA> ();
			return aa;
		});
		fam.registerPass ([] { return ManagedAA (); });
		pb.registerModuleAnalyses (mam);
		pb.registerCGSCCAnalyses (cgam);
		pb.registerFunctionAnalyses (fam);
		pb.registerLoopAnalyses (lam);
		pb.crossRegisterProxies (lam, fam, cgam, mam);

		ValueSymbolTable *names = caller->getValueSymbolTable ();
		Value *left = names->lookup (a);
		Value *right = names->lookup (b);

		EXPECT_NE (left, nullptr) << a.str ();
		EXPECT_NE (right, nullptr) << b.str ();

		return fam.getResult<AAManager> (*caller).alias (
			MemoryLocation (left, LocationSize::precise (4)),
			MemoryLocation (right, LocationSize::precise (4)));
	}
};

/// Starts the runtime before a case builds its arguments, which name classes.
class ManagedAATest : public ::testing::Test {
protected:
	void SetUp () override { init_runtime (); }
};

TEST_F (ManagedAATest, StaticsBlockIsNoObjectAStaticReadNames)
{
	Parsed m (R"(
define void @caller() {
  %array = load ptr, ptr getelementptr (i8, ptr @statics, i64 EMPTY), !tbaa !100
  %element = getelementptr inbounds i8, ptr %array, i64 32
  %slot = getelementptr i8, ptr @statics, i64 EMPTY
  ret void
}
)");

	EXPECT_EQ (m.alias ("element", "slot"), AliasResult::NoAlias);
}

TEST_F (ManagedAATest, PlainGepOffAnObjectReachesAnything)
{
	Parsed m (R"(
define void @caller(i64 %offset) {
  %array = load ptr, ptr getelementptr (i8, ptr @statics, i64 EMPTY), !tbaa !100
  %element = getelementptr i8, ptr %array, i64 %offset
  %slot = getelementptr i8, ptr @statics, i64 EMPTY
  ret void
}
)");

	EXPECT_EQ (m.alias ("element", "slot"), AliasResult::MayAlias);
}

TEST_F (ManagedAATest, UntaggedLoadIsNoObject)
{
	Parsed m (R"(
define void @caller(ptr %p) {
  %pointer = load ptr, ptr %p
  %element = getelementptr inbounds i8, ptr %pointer, i64 32
  %slot = getelementptr i8, ptr @statics, i64 EMPTY
  ret void
}
)");

	EXPECT_EQ (m.alias ("element", "slot"), AliasResult::MayAlias);
}

TEST_F (ManagedAATest, OffsetNoStaticFieldCoversIsNotTheBlock)
{
	Parsed m (R"(
define void @caller(ptr %p) {
  %object = load ptr, ptr %p, !tbaa !100
  %field = getelementptr inbounds i8, ptr %object, i64 16
  %past = getelementptr i8, ptr @statics, i64 1048576
  ret void
}
)");

	EXPECT_EQ (m.alias ("field", "past"), AliasResult::MayAlias);
}

TEST_F (ManagedAATest, UnrelatedParameterClassesAreTwoObjects)
{
	Parsed m (R"(
define void @caller(ptr %string, ptr %array) {
  %a = getelementptr inbounds i8, ptr %string, i64 16
  %b = getelementptr inbounds i8, ptr %array, i64 32
  ret void
}
)",
	          { mono_defaults.string_class, vector_of (mono_defaults.int32_class) });

	EXPECT_EQ (m.alias ("a", "b"), AliasResult::NoAlias);
}

TEST_F (ManagedAATest, SubclassParameterCanBeTheSameObject)
{
	Parsed m (R"(
define void @caller(ptr %object, ptr %string) {
  %a = getelementptr inbounds i8, ptr %object, i64 16
  %b = getelementptr inbounds i8, ptr %string, i64 16
  ret void
}
)",
	          { mono_defaults.object_class, mono_defaults.string_class });

	EXPECT_EQ (m.alias ("a", "b"), AliasResult::MayAlias);
}

TEST_F (ManagedAATest, StaticFieldTypeBoundsWhatItHolds)
{
	Parsed m (R"(
define void @caller(ptr %array) {
  %string = load ptr, ptr getelementptr (i8, ptr @statics, i64 EMPTY), !tbaa !100
  %a = getelementptr inbounds i8, ptr %string, i64 16
  %b = getelementptr inbounds i8, ptr %array, i64 32
  ret void
}
)",
	          { vector_of (mono_defaults.int32_class) });

	EXPECT_EQ (m.alias ("a", "b"), AliasResult::NoAlias);
}

TEST_F (ManagedAATest, InstanceFieldTypeBoundsWhatItHolds)
{
	Parsed m (R"(
define void @caller(ptr %exception, ptr %array) {
  %slot = getelementptr inbounds i8, ptr %exception, i64 MESSAGE
  %string = load ptr, ptr %slot, !tbaa !100
  %a = getelementptr inbounds i8, ptr %string, i64 16
  %b = getelementptr inbounds i8, ptr %array, i64 32
  ret void
}
)",
	          { exception_class (), vector_of (mono_defaults.int32_class) });

	EXPECT_EQ (m.alias ("a", "b"), AliasResult::NoAlias);
}

TEST_F (ManagedAATest, PlainGepToAFieldBoundsNothing)
{
	Parsed m (R"(
define void @caller(ptr %exception, ptr %array) {
  %slot = getelementptr i8, ptr %exception, i64 MESSAGE
  %string = load ptr, ptr %slot, !tbaa !100
  %a = getelementptr inbounds i8, ptr %string, i64 16
  %b = getelementptr inbounds i8, ptr %array, i64 32
  ret void
}
)",
	          { exception_class (), vector_of (mono_defaults.int32_class) });

	EXPECT_EQ (m.alias ("a", "b"), AliasResult::MayAlias);
}

TEST_F (ManagedAATest, OffsetInsideAFieldBoundsNothing)
{
	Parsed m (R"(
define void @caller(ptr %exception, ptr %array) {
  %slot = getelementptr inbounds i8, ptr %exception, i64 MESSAGE
  %inside = getelementptr inbounds i8, ptr %slot, i64 4
  %string = load ptr, ptr %inside, !tbaa !100
  %a = getelementptr inbounds i8, ptr %string, i64 16
  %b = getelementptr inbounds i8, ptr %array, i64 32
  ret void
}
)",
	          { exception_class (), vector_of (mono_defaults.int32_class) });

	EXPECT_EQ (m.alias ("a", "b"), AliasResult::MayAlias);
}

TEST_F (ManagedAATest, ElementClassBoundsWhatItHolds)
{
	Parsed m (R"(
define void @caller(ptr %strings, ptr %array, i64 %i) {
  %vector = getelementptr inbounds i8, ptr %strings, i64 32
  %at = getelementptr inbounds ptr, ptr %vector, i64 %i
  %string = load ptr, ptr %at, !tbaa !100
  %a = getelementptr inbounds i8, ptr %string, i64 16
  %b = getelementptr inbounds i8, ptr %array, i64 32
  ret void
}
)",
	          { vector_of (mono_defaults.string_class), vector_of (mono_defaults.int32_class) });

	EXPECT_EQ (m.alias ("a", "b"), AliasResult::NoAlias);
}

TEST_F (ManagedAATest, LoopPhiKeepsTheClassItsFieldHolds)
{
	Parsed m (R"(
define void @caller(ptr %first, ptr %array) {
entry:
  br label %loop
loop:
  %node = phi ptr [ %first, %entry ], [ %next, %loop ]
  %slot = getelementptr inbounds i8, ptr %node, i64 INNER
  %next = load ptr, ptr %slot, !tbaa !100
  %a = getelementptr inbounds i8, ptr %node, i64 16
  %b = getelementptr inbounds i8, ptr %array, i64 32
  %done = icmp eq ptr %next, null
  br i1 %done, label %exit, label %loop
exit:
  ret void
}
)",
	          { exception_class (), vector_of (mono_defaults.int32_class) });

	EXPECT_EQ (m.alias ("a", "b"), AliasResult::NoAlias);
}

TEST_F (ManagedAATest, PhiOfTwoClassesBoundsNothing)
{
	Parsed m (R"(
define void @caller(ptr %string, ptr %exception, ptr %array, i1 %which) {
entry:
  br i1 %which, label %left, label %join
left:
  br label %join
join:
  %either = phi ptr [ %string, %left ], [ %exception, %entry ]
  %a = getelementptr inbounds i8, ptr %either, i64 16
  %b = getelementptr inbounds i8, ptr %array, i64 32
  ret void
}
)",
	          { mono_defaults.string_class, exception_class (),
	            vector_of (mono_defaults.int32_class) });

	EXPECT_EQ (m.alias ("a", "b"), AliasResult::MayAlias);
}

TEST_F (ManagedAATest, ClassesShareNoInstance)
{
	MonoClass *string = mono_defaults.string_class;
	MonoClass *ints = vector_of (mono_defaults.int32_class);

	EXPECT_TRUE (classes_share_no_instance (string, ints));
	EXPECT_TRUE (classes_share_no_instance (ints, corlib_class ("System", "Exception")));

	EXPECT_FALSE (classes_share_no_instance (string, string));
	EXPECT_FALSE (classes_share_no_instance (mono_defaults.object_class, string));
	EXPECT_FALSE (classes_share_no_instance (corlib_class ("System", "Array"), ints));
	EXPECT_FALSE (classes_share_no_instance (corlib_class ("System", "IComparable"), ints));
	EXPECT_FALSE (classes_share_no_instance (ints, vector_of (mono_defaults.uint32_class)));
	EXPECT_FALSE (classes_share_no_instance (ints, vector_of (string)));
	EXPECT_FALSE (classes_share_no_instance (corlib_class ("System", "MarshalByRefObject"),
	                                         corlib_class ("System", "Exception")));
	EXPECT_FALSE (classes_share_no_instance (corlib_class ("System", "Action"),
	                                         corlib_class ("System", "Exception")));
}

} // namespace
} // namespace test
} // namespace mono

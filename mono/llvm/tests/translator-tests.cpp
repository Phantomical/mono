/*
 * Tests for mono::method_to_llvm ().
 *
 * Two kinds of case.  The sweep below translates every method in the corpus and
 * asserts only that it came back without a refusal - the verifier check that the
 * fixture runs on each one is what that sweep is really for, since none of the
 * hand-built basic blocks, spill slots, landing pads or invokes had ever been
 * looked at by it.  The named tests after it assert what the IR actually says,
 * for the shapes where "it verified" is not the interesting part.
 */

#include "harness.hpp"

#include <llvm/IR/CFG.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>

#include <ostream>
#include <set>
#include <string>
#include <vector>

using mono::test::Translation;
using mono::test::TranslatorTest;

namespace {

struct MethodRef {
	const char *image;
	const char *method;
};

/*
 * gtest puts the parameter's printed form in a comment after each test name, and
 * gtest_discover_tests carries that comment into the CTest name.  Without these
 * the default byte-dump of the struct lands there, which contains pointers and so
 * changes every run.
 */
void
PrintTo (const MethodRef &ref, std::ostream *os)
{
	*os << ref.image << ".dll " << ref.method;
}

/// "Arith:Add" in arith.dll becomes arith_Arith_Add, which is what ctest lists.
std::string
method_ref_name (const testing::TestParamInfo<MethodRef> &info)
{
	std::string name = std::string (info.param.image) + "_" + info.param.method;

	for (char &c : name)
		if (!isalnum (static_cast<unsigned char> (c)))
			c = '_';

	return name;
}

/// Every method in the corpus the translator is expected to accept.
const MethodRef translatable[] = {
	{"arith", "Arith:Add"},
	{"arith", "Arith:MixInt64"},
	{"arith", "Arith:MixFloat"},
	{"arith", "Arith:Div"},
	{"arith", "Arith:RemUn"},
	{"arith", "Arith:AddOvf"},
	{"arith", "Arith:MulOvfUn"},
	{"arith", "Arith:SubOvf"},
	{"arith", "Arith:Neg"},
	{"arith", "Arith:NegFloat"},
	{"arith", "Arith:Bits"},
	{"arith", "Arith:Shifts"},
	{"arith", "Arith:ConvToFloat"},
	{"arith", "Arith:ConvNarrow"},
	{"arith", "Arith:ConvOvf"},
	{"arith", "Arith:ConvRUn"},
	{"arith", "Arith:Constants"},
	{"arith", "Arith:FloatConstants"},

	{"stack", "Stack:Dup"},
	{"stack", "Stack:Pop"},
	{"stack", "Stack:Null"},
	{"stack", "Stack:DupAcrossBranch"},

	{"locals", "Locals:Deref"},
	{"locals", "Locals:RoundTrip"},
	{"locals", "Locals:NarrowLocals"},
	{"locals", "Locals:StoreArg"},
	{"locals", "Locals:ArgAddress"},
	{"locals", "Locals:LocalAddress"},
	{"locals", "Locals:ManyLocals"},

	{"flow", "Flow:Max"},
	{"flow", "Flow:SumTo"},
	{"flow", "Flow:StackAcrossBranch"},
	{"flow", "Flow:Choose"},
	{"flow", "Flow:Compare"},
	{"flow", "Flow:Compares"},
	{"flow", "Flow:Nested"},

	{"fields", "Fields:GetX"},
	{"fields", "Fields:GetY"},
	{"fields", "Fields:GetSmall"},
	{"fields", "Fields:SetX"},
	{"fields", "Fields:SetRef"},
	{"fields", "Fields:FieldAddress"},
	{"fields", "Fields:GetStatic"},
	{"fields", "Fields:SetStatic"},
	{"fields", "Fields:SetStaticRef"},
	{"fields", "Fields:StaticAddress"},
	{"fields", "Fields:TwoStatics"},

	{"arrays", "Arrays:Length"},
	{"arrays", "Arrays:GetInt"},
	{"arrays", "Arrays:GetByte"},
	{"arrays", "Arrays:GetDouble"},
	{"arrays", "Arrays:GetRef"},
	{"arrays", "Arrays:SetInt"},
	{"arrays", "Arrays:SetRef"},
	{"arrays", "Arrays:Make"},
	{"arrays", "Arrays:ElementAddress"},
	{"arrays", "Arrays:Sum"},

	{"eh", "Eh:TryCatch"},
	{"eh", "Eh:TwoCatches"},
	{"eh", "Eh:TryFinally"},
	{"eh", "Eh:TwoLeaves"},
	{"eh", "Eh:NestedFinally"},
	{"eh", "Eh:CatchInsideFinally"},
	{"eh", "Eh:Throw"},
	{"eh", "Eh:Rethrow"},
	{"eh", "Eh:Filter"},
	{"eh", "Eh:CallInTry"},

	{"boxing", "Boxing:BoxInt"},
	{"boxing", "Boxing:BoxPair"},
	{"boxing", "Boxing:BoxRefPair"},
	{"boxing", "Boxing:BoxObject"},
	{"boxing", "Boxing:UnboxInt"},
	{"boxing", "Boxing:UnboxAnyInt"},
	{"boxing", "Boxing:UnboxAnyPair"},
	{"boxing", "Boxing:RoundTrip"},
	{"boxing", "Boxing:BoxNullable"},
	{"boxing", "Boxing:UnboxNullable"},
	{"boxing", "Boxing:UnboxAnyNullable"},
	{"boxing", "Boxing:UnboxAnyNullableEnum"},

	{"misc", "Misc:SizeOfInt"},
	{"misc", "Misc:SizeOfObject"},
	{"misc", "Misc:SizeOfWide"},
	{"misc", "Misc:CheckFinite"},
	{"misc", "Misc:Breakpoint"},
	{"misc", "Misc:StackAlloc"},
	{"misc", "Misc:StackAllocZeroed"},

	{"blocks", "Blocks:Copy"},
	{"blocks", "Blocks:CopyUnaligned"},
	{"blocks", "Blocks:Fill"},
	{"blocks", "Blocks:FillVolatile"},

	{"prefixed", "Prefixed:VolatileRead"},
	{"prefixed", "Prefixed:VolatileWrite"},
	{"prefixed", "Prefixed:VolatileStatic"},
	{"prefixed", "Prefixed:UnalignedRead"},
	{"prefixed", "Prefixed:ConstrainedOnClass"},
	{"prefixed", "Prefixed:ConstrainedOnStruct"},
	{"prefixed", "Prefixed:ConstrainedBoxes"},
	{"prefixed", "Prefixed:ConstrainedBoxesWithArg"},
	{"prefixed", "Prefixed:TailCall"},

	{"fnptr", "Fnptr:TakeStatic"},
	{"fnptr", "Fnptr:TakeVirtual"},
	{"fnptr", "Fnptr:CallThroughPointer"},
	{"fnptr", "Fnptr:CallThroughArgument"},

	{"objects", "Objects:MakeCounter"},
	{"objects", "Objects:MakeCounterAt"},
	{"objects", "Objects:MakePoint"},
	{"objects", "Objects:UseThePoint"},

	{"tokens", "Tokens:Hello"},
	{"tokens", "Tokens:SameLiteralTwice"},
	{"tokens", "Tokens:TypeOf"},
	{"tokens", "Tokens:MethodToken"},
	{"tokens", "Tokens:FieldToken"},

	{"casts", "Casts:CastString"},
	{"casts", "Casts:IsString"},
	{"casts", "Casts:CastIface"},
	{"casts", "Casts:IsIface"},
	{"casts", "Casts:IsBoxedInt"},
	{"casts", "Casts:TwoCasts"},
	{"casts", "Casts:UnboxAnyString"},

	{"calls", "Calls:CallStatic"},
	{"calls", "Calls:CallStaticTwice"},
	{"calls", "Calls:CallVoid"},
	{"calls", "Calls:CallInstance"},
	{"calls", "Calls:CallVirtual"},
	{"calls", "Calls:CallSealed"},
	{"calls", "Calls:CallNonVirtual"},
	{"calls", "Calls:CallInterface"},
	{"calls", "Calls:CallGenericVirtual"},
	{"calls", "Calls:CallGenericInterface"},
};

class Translates : public TranslatorTest, public testing::WithParamInterface<MethodRef> {};

TEST_P (Translates, AndVerifies)
{
	const Translation &translation = translate (GetParam ().image, GetParam ().method);

	ASSERT_TRUE (translation.error.empty ())
		<< GetParam ().method << " was refused: " << translation.error;
	ASSERT_NE (translation.function, nullptr);
}

INSTANTIATE_TEST_SUITE_P (Corpus, Translates, testing::ValuesIn (translatable), method_ref_name);

/// Whether the function's CFG has a back edge, by depth-first search.
bool
has_cycle (const llvm::Function &function)
{
	std::set<const llvm::BasicBlock *> done;
	std::set<const llvm::BasicBlock *> on_path;
	std::vector<std::pair<const llvm::BasicBlock *, bool>> work;

	work.push_back ({&function.getEntryBlock (), false});

	while (!work.empty ()) {
		auto [block, leaving] = work.back ();

		work.pop_back ();

		if (leaving) {
			on_path.erase (block);
			done.insert (block);
			continue;
		}
		if (done.count (block))
			continue;
		if (on_path.count (block))
			return true;

		on_path.insert (block);
		work.push_back ({block, true});

		for (const llvm::BasicBlock *next : llvm::successors (block))
			if (!done.count (next))
				work.push_back ({next, false});
	}

	return false;
}

/*
 * The whole sweep above rests on the verifier check the fixture runs, so it is
 * worth knowing that check is not inert: a function with no terminator is the
 * simplest thing it must reject.
 */
TEST (Harness, TheVerifierCheckActuallyFires)
{
	llvm::LLVMContext context;
	llvm::Module module ("harness", context);
	llvm::Function *function = llvm::Function::Create (
		llvm::FunctionType::get (llvm::Type::getVoidTy (context), false),
		llvm::GlobalValue::ExternalLinkage, "broken", module);

	llvm::BasicBlock::Create (context, "entry", function);

	EXPECT_FALSE (mono::test::verify_function (*function).empty ());
}

/* ------------------------------------------------------------ arithmetic */

TEST_F (TranslatorTest, AddIsOneAddOfTheDeclaredWidth)
{
	const Translation &t = translate ("arith", "Arith:Add");

	ASSERT_NE (t.function, nullptr) << t.error;
	EXPECT_TRUE (t.function->getReturnType ()->isIntegerTy (32));
	EXPECT_EQ (t.function->arg_size (), 2u);
	EXPECT_EQ (t.count ("add i32"), 1u);
}

TEST_F (TranslatorTest, CheckedArithmeticUsesTheOverflowIntrinsics)
{
	EXPECT_EQ (translate ("arith", "Arith:AddOvf").count ("llvm.sadd.with.overflow.i32"), 1u);
	EXPECT_EQ (translate ("arith", "Arith:SubOvf").count ("llvm.ssub.with.overflow.i32"), 1u);
	EXPECT_EQ (translate ("arith", "Arith:MulOvfUn").count ("llvm.umul.with.overflow.i32"), 1u);
}

TEST_F (TranslatorTest, DivisionIsGuarded)
{
	const Translation &t = translate ("arith", "Arith:Div");

	ASSERT_NE (t.function, nullptr) << t.error;
	EXPECT_GE (t.count ("mono_llvm_throw_corlib_exception"), 1u);
	EXPECT_EQ (t.count ("sdiv i32"), 1u);
}

TEST_F (TranslatorTest, IntToFloatConversionSignExtends)
{
	EXPECT_EQ (translate ("arith", "Arith:ConvToFloat").count ("sitofp"), 1u);
	EXPECT_EQ (translate ("arith", "Arith:ConvRUn").count ("uitofp"), 1u);
}

// dup copies the stack entry, not the instruction behind it, so the argument is
// read once and both operands of the add are that one load.
TEST_F (TranslatorTest, DupReusesTheValueRatherThanReloadingIt)
{
	const Translation &t = translate ("stack", "Stack:Dup");

	ASSERT_NE (t.function, nullptr) << t.error;
	EXPECT_EQ (t.count ("load i32"), 1u) << t.text ();
	EXPECT_EQ (t.count ("add i32"), 1u);
}

/* ---------------------------------------------------------- control flow */

TEST_F (TranslatorTest, LoopBecomesABackEdge)
{
	const Translation &t = translate ("flow", "Flow:SumTo");

	ASSERT_NE (t.function, nullptr) << t.error;
	EXPECT_TRUE (has_cycle (*t.function));
}

TEST_F (TranslatorTest, SwitchBecomesASwitch)
{
	const Translation &t = translate ("flow", "Flow:Choose");

	ASSERT_NE (t.function, nullptr) << t.error;
	EXPECT_EQ (t.count ("switch i32"), 1u);
}

// A join entered holding a value has to find it in the slot every predecessor
// spilled it to, which is the only reason the spill allocas exist.
TEST_F (TranslatorTest, StackLiveAcrossABranchGoesThroughASpillSlot)
{
	const Translation &t = translate ("flow", "Flow:StackAcrossBranch");

	ASSERT_NE (t.function, nullptr) << t.error;
	EXPECT_GE (t.count ("%stack0 = alloca"), 1u);
	EXPECT_GE (t.count ("%stack1 = alloca"), 1u);
}

/* ----------------------------------------------------------------- fields */

TEST_F (TranslatorTest, InstanceFieldAccessNullChecks)
{
	const Translation &t = translate ("fields", "Fields:GetX");

	ASSERT_NE (t.function, nullptr) << t.error;
	EXPECT_GE (t.count ("make.implicit"), 1u);
}

TEST_F (TranslatorTest, StoringAReferenceGoesThroughTheWriteBarrier)
{
	EXPECT_GE (translate ("fields", "Fields:SetRef")
	                   .count ("mono_gc_wbarrier_generic_store_internal"),
	           1u);
	EXPECT_GE (translate ("fields", "Fields:SetStaticRef")
	                   .count ("mono_gc_wbarrier_generic_store_internal"),
	           1u);
}

// One relocation per class, not per field: both loads resolve through the same
// mono_statics_ block and differ only in the offset.
TEST_F (TranslatorTest, StaticsOfOneClassShareOneSymbol)
{
	const Translation &t = translate ("fields", "Fields:TwoStatics");

	ASSERT_NE (t.function, nullptr) << t.error;

	size_t blocks = 0;
	for (const llvm::GlobalVariable &global : t.module->globals ())
		if (global.getName ().starts_with ("mono_statics_"))
			++blocks;

	EXPECT_EQ (blocks, 1u);
	EXPECT_NE (t.module->getNamedGlobal ("mono_statics_Holder"), nullptr);
}

/* ----------------------------------------------------------------- arrays */

TEST_F (TranslatorTest, NewarrCallsTheAllocatorWithTheArrayVtable)
{
	const Translation &t = translate ("arrays", "Arrays:Make");

	ASSERT_NE (t.function, nullptr) << t.error;
	EXPECT_GE (t.count ("mono_array_new_specific"), 1u);
	EXPECT_GE (t.count ("mono_vtable_"), 1u);
}

TEST_F (TranslatorTest, StoringAReferenceElementChecksTheElementType)
{
	EXPECT_GE (translate ("arrays", "Arrays:SetRef").count ("mono_helper_stelem_ref_check"),
	           1u);
}

TEST_F (TranslatorTest, ElementAccessIsBoundsChecked)
{
	const Translation &t = translate ("arrays", "Arrays:GetInt");

	ASSERT_NE (t.function, nullptr) << t.error;
	EXPECT_GE (t.count ("mono_llvm_throw_corlib_exception"), 1u);
	EXPECT_GE (t.count ("icmp uge"), 1u);
}

/* ------------------------------------------------------------- exceptions */

TEST_F (TranslatorTest, ACatchGetsALandingPadAndAPersonality)
{
	const Translation &t = translate ("eh", "Eh:TryCatch");

	ASSERT_NE (t.function, nullptr) << t.error;
	EXPECT_TRUE (t.function->hasPersonalityFn ());
	EXPECT_GE (t.count ("landingpad"), 1u);
}

TEST_F (TranslatorTest, ACallInsideATryIsAnInvoke)
{
	const Translation &t = translate ("eh", "Eh:CallInTry");

	ASSERT_NE (t.function, nullptr) << t.error;
	EXPECT_GE (t.count ("invoke"), 1u);
}

TEST_F (TranslatorTest, ACallOutsideATryIsAPlainCall)
{
	const Translation &t = translate ("calls", "Calls:CallStatic");

	ASSERT_NE (t.function, nullptr) << t.error;
	EXPECT_EQ (t.count ("invoke"), 0u);
}

// The finally is entered from its own leaves and by unwinding, so its endfinally
// switches on which of those is in progress.
TEST_F (TranslatorTest, EndfinallyResumesThroughASwitch)
{
	const Translation &t = translate ("eh", "Eh:TryFinally");

	ASSERT_NE (t.function, nullptr) << t.error;
	EXPECT_GE (t.count ("switch i32"), 1u);
}

TEST_F (TranslatorTest, TwoLeavesOutOfOneTryGetTwoContinuations)
{
	const Translation &t = translate ("eh", "Eh:TwoLeaves");

	ASSERT_NE (t.function, nullptr) << t.error;
	/* One arm per leave, plus the unwind arm the switch's default carries. */
	EXPECT_GE (t.count ("i32 1, label"), 1u);
	EXPECT_GE (t.count ("i32 2, label"), 1u);
}

TEST_F (TranslatorTest, ThrowDoesNotReturn)
{
	const Translation &t = translate ("eh", "Eh:Throw");

	ASSERT_NE (t.function, nullptr) << t.error;
	EXPECT_GE (t.count ("mono_llvm_throw_exception"), 1u);
	EXPECT_GE (t.count ("unreachable"), 1u);
}

TEST_F (TranslatorTest, RethrowUsesTheRethrowHelper)
{
	EXPECT_GE (translate ("eh", "Eh:Rethrow").count ("mono_llvm_rethrow_exception"), 1u);
}

/* ------------------------------------------------------------------ calls */

TEST_F (TranslatorTest, AStaticCallNamesItsCallee)
{
	const Translation &t = translate ("calls", "Calls:CallStatic");

	ASSERT_NE (t.function, nullptr) << t.error;
	EXPECT_GE (t.count ("Helper"), 1u);
}

// Only a method that can still be overridden is looked up: a callvirt on a final
// or non-virtual one is a null check with a direct call behind it.  The callee's
// full name appears in the body exactly when the call went out directly, so that
// is what tells the two apart.
TEST_F (TranslatorTest, OnlyAnOverridableCallvirtReadsTheVtable)
{
	const Translation &overridable = translate ("calls", "Calls:CallVirtual");
	const Translation &final_method = translate ("calls", "Calls:CallSealed");
	const Translation &instance = translate ("calls", "Calls:CallNonVirtual");

	ASSERT_NE (overridable.function, nullptr) << overridable.error;
	EXPECT_EQ (overridable.count ("Base:Virt"), 0u) << overridable.text ();

	ASSERT_NE (final_method.function, nullptr) << final_method.error;
	EXPECT_GE (final_method.count ("Base:Sealed"), 1u);

	ASSERT_NE (instance.function, nullptr) << instance.error;
	EXPECT_GE (instance.count ("Base:Inst"), 1u);
}

// An interface call reaches its target through the IMT rather than the vtable,
// and carries the method it wants in the nest register for the thunk to match on.
TEST_F (TranslatorTest, AnInterfaceCallGoesThroughTheImt)
{
	const Translation &t = translate ("calls", "Calls:CallInterface");

	ASSERT_NE (t.function, nullptr) << t.error;
	EXPECT_EQ (t.count ("ptr nest @\"mono_method_"), 1u) << t.text ();
}

// A virtual generic method's slot never holds one instantiation's code - it holds a
// trampoline that reads the asked-for inflated method out of the IMT register - so
// the call carries the instantiation as a nest key whether the method lives on a
// class (vtable slot) or an interface (IMT slot).
TEST_F (TranslatorTest, AGenericVirtualCallCarriesTheInflatedMethod)
{
	const Translation &on_class = translate ("calls", "Calls:CallGenericVirtual");
	const Translation &on_iface = translate ("calls", "Calls:CallGenericInterface");

	ASSERT_NE (on_class.function, nullptr) << on_class.error;
	EXPECT_EQ (on_class.count ("ptr nest @\"mono_method_Base:Choose<int>"), 1u)
		<< on_class.text ();
	/* Dispatched, never called by name. */
	EXPECT_EQ (on_class.count ("call i32 @\"Base:Choose"), 0u);

	ASSERT_NE (on_iface.function, nullptr) << on_iface.error;
	EXPECT_EQ (on_iface.count ("ptr nest @\"mono_method_IPicker:Pick<int>"), 1u)
		<< on_iface.text ();
}

TEST_F (TranslatorTest, AVoidCallLeavesNothingOnTheStack)
{
	const Translation &t = translate ("calls", "Calls:CallVoid");

	ASSERT_NE (t.function, nullptr) << t.error;
	EXPECT_TRUE (t.function->getReturnType ()->isVoidTy ());
	EXPECT_GE (t.count ("ret void"), 1u);
}

/* ----------------------------------------------------------------- boxing */

TEST_F (TranslatorTest, BoxAllocatesWithTheVtableAndStoresTheValue)
{
	const Translation &t = translate ("boxing", "Boxing:BoxInt");

	ASSERT_NE (t.function, nullptr) << t.error;
	EXPECT_EQ (t.count ("mono_object_new_specific"), 1u);
	/* mono_type_full_name spells System.Int32 as "int". */
	EXPECT_EQ (t.count ("mono_vtable_int"), 1u);
}

// A reference type's boxed form is itself, so nothing may be allocated.
TEST_F (TranslatorTest, BoxOnAReferenceTypeAllocatesNothing)
{
	const Translation &t = translate ("boxing", "Boxing:BoxObject");

	ASSERT_NE (t.function, nullptr) << t.error;
	EXPECT_EQ (t.count ("mono_object_new_specific"), 0u);
}

// A struct with a reference inside cannot be memcpy'd into the box: the copy has to
// go through the collector so the new object's cards get marked.
TEST_F (TranslatorTest, BoxingARefStructCopiesThroughTheBarrier)
{
	const Translation &plain = translate ("boxing", "Boxing:BoxPair");
	const Translation &with_ref = translate ("boxing", "Boxing:BoxRefPair");

	ASSERT_NE (with_ref.function, nullptr) << with_ref.error;
	EXPECT_EQ (with_ref.count ("mono_gc_wbarrier_value_copy_internal"), 1u);
	ASSERT_NE (plain.function, nullptr) << plain.error;
	EXPECT_EQ (plain.count ("mono_gc_wbarrier_value_copy_internal"), 0u);
}

// unbox must reject an array (whose element class would match) by its rank byte, and
// anything whose element class is not the target's, before handing out the interior
// pointer.
TEST_F (TranslatorTest, UnboxChecksTheClassBeforeTakingTheAddress)
{
	const Translation &t = translate ("boxing", "Boxing:UnboxInt");

	ASSERT_NE (t.function, nullptr) << t.error;
	EXPECT_EQ (t.count ("mono_class_int"), 1u) << t.text ();
	EXPECT_EQ (t.count ("throw_InvalidCastException"), 4u); // 2 blocks, label + branch
}

TEST_F (TranslatorTest, UnboxAnyLeavesTheValueNotTheAddress)
{
	const Translation &t = translate ("boxing", "Boxing:UnboxAnyInt");

	ASSERT_NE (t.function, nullptr) << t.error;
	EXPECT_TRUE (t.function->getReturnType ()->isIntegerTy (32));
}

// A nullable boxes to null or to a boxed T depending on HasValue, and that branch is
// the corlib Box helper - so the call goes there and this function allocates nothing.
TEST_F (TranslatorTest, BoxingANullableCallsTheCorlibHelper)
{
	const Translation &t = translate ("boxing", "Boxing:BoxNullable");

	ASSERT_NE (t.function, nullptr) << t.error;
	EXPECT_EQ (t.count ("Nullable`1<int>:Box"), 1u) << t.text ();
	EXPECT_EQ (t.count ("mono_object_new_specific"), 0u);
}

TEST_F (TranslatorTest, UnboxAnyOnANullableCallsUnbox)
{
	const Translation &t = translate ("boxing", "Boxing:UnboxAnyNullable");

	ASSERT_NE (t.function, nullptr) << t.error;
	EXPECT_EQ (t.count ("Nullable`1<int>:Unbox"), 1u) << t.text ();
}

// T being an enum switches the helper: Unbox's cast would also accept a boxed int
// where only a boxed Color may pass, so the exact-type variant is called instead.
TEST_F (TranslatorTest, UnboxAnyOnANullableEnumCallsUnboxExact)
{
	const Translation &t = translate ("boxing", "Boxing:UnboxAnyNullableEnum");

	ASSERT_NE (t.function, nullptr) << t.error;
	EXPECT_EQ (t.count ("UnboxExact"), 1u) << t.text ();
}

// unbox on a nullable has no interior pointer to hand out, so the Nullable<int> the
// helper manufactures is spilled and the pointer pushed points at the spill.
TEST_F (TranslatorTest, UnboxOnANullableSpillsTheHelperResult)
{
	const Translation &t = translate ("boxing", "Boxing:UnboxNullable");

	ASSERT_NE (t.function, nullptr) << t.error;
	EXPECT_EQ (t.count ("Nullable`1<int>:Unbox"), 1u) << t.text ();
	EXPECT_GE (t.count ("alloca"), 1u);
}

/* ------------------------------------------------------------------- misc */

// sizeof settles at compile time, and array elements lie sizeof bytes apart - so a
// two-int64 struct answers 16, and any reference type the width of a pointer.
TEST_F (TranslatorTest, SizeofIsACompileTimeConstant)
{
	EXPECT_EQ (translate ("misc", "Misc:SizeOfInt").count ("ret i32 4"), 1u);
	EXPECT_EQ (translate ("misc", "Misc:SizeOfObject").count ("ret i32 8"), 1u);
	EXPECT_EQ (translate ("misc", "Misc:SizeOfWide").count ("ret i32 16"), 1u);
}

// NaN answers |x| >= inf by being unordered, either infinity by being equal.
TEST_F (TranslatorTest, CkfiniteRejectsNanAndInfinity)
{
	const Translation &t = translate ("misc", "Misc:CheckFinite");

	ASSERT_NE (t.function, nullptr) << t.error;
	EXPECT_EQ (t.count ("llvm.fabs"), 1u);
	EXPECT_EQ (t.count ("fcmp ueq"), 1u);
	EXPECT_GE (t.count ("throw_ArithmeticException"), 1u);
}

TEST_F (TranslatorTest, BreakBecomesADebugTrap)
{
	EXPECT_EQ (translate ("misc", "Misc:Breakpoint").count ("llvm.debugtrap"), 1u);
}

// localloc is a dynamic alloca; the localsinit flag decides whether it is zeroed.
TEST_F (TranslatorTest, LocallocIsADynamicAlloca)
{
	const Translation &raw = translate ("misc", "Misc:StackAlloc");
	const Translation &zeroed = translate ("misc", "Misc:StackAllocZeroed");

	ASSERT_NE (raw.function, nullptr) << raw.error;
	EXPECT_EQ (raw.count ("alloca i8, i64"), 1u) << raw.text ();
	EXPECT_EQ (raw.count ("llvm.memset"), 0u);

	ASSERT_NE (zeroed.function, nullptr) << zeroed.error;
	/* One more than the locals' own zeroing accounts for: the localloc block's. */
	EXPECT_GT (zeroed.count ("llvm.memset"), raw.count ("llvm.memset"));
	EXPECT_EQ (zeroed.count ("alloca i8, i64"), 1u);
}

/* ----------------------------------------------------------------- blocks */

TEST_F (TranslatorTest, BlockOpsBecomeTheIntrinsics)
{
	const Translation &copy = translate ("blocks", "Blocks:Copy");
	const Translation &unaligned = translate ("blocks", "Blocks:CopyUnaligned");
	const Translation &fill = translate ("blocks", "Blocks:Fill");
	const Translation &fenced = translate ("blocks", "Blocks:FillVolatile");

	ASSERT_NE (copy.function, nullptr) << copy.error;
	EXPECT_EQ (copy.count ("llvm.memcpy"), 1u);
	EXPECT_EQ (copy.count ("ptr align 8"), 2u) << copy.text ();

	ASSERT_NE (unaligned.function, nullptr) << unaligned.error;
	EXPECT_EQ (unaligned.count ("ptr align 1"), 2u) << unaligned.text ();

	ASSERT_NE (fill.function, nullptr) << fill.error;
	EXPECT_EQ (fill.count ("llvm.memset"), 1u);

	ASSERT_NE (fenced.function, nullptr) << fenced.error;
	EXPECT_EQ (fenced.count ("fence release"), 1u);
	EXPECT_EQ (fenced.count ("i1 true"), 1u) << fenced.text ();
}

/* --------------------------------------------------------------- prefixes */

// A volatile read has acquire semantics and a volatile write release, so the access
// is marked volatile and paired with the matching fence.
TEST_F (TranslatorTest, VolatileAccessesCarryTheirFences)
{
	const Translation &read = translate ("prefixed", "Prefixed:VolatileRead");
	const Translation &write = translate ("prefixed", "Prefixed:VolatileWrite");
	const Translation &statics = translate ("prefixed", "Prefixed:VolatileStatic");

	ASSERT_NE (read.function, nullptr) << read.error;
	EXPECT_EQ (read.count ("load volatile i32"), 1u);
	EXPECT_EQ (read.count ("fence acquire"), 1u);

	ASSERT_NE (write.function, nullptr) << write.error;
	EXPECT_EQ (write.count ("store volatile i32"), 1u);
	EXPECT_EQ (write.count ("fence release"), 1u);

	ASSERT_NE (statics.function, nullptr) << statics.error;
	EXPECT_EQ (statics.count ("load volatile i32"), 1u);
}

TEST_F (TranslatorTest, UnalignedLowersTheAccessAlignment)
{
	const Translation &t = translate ("prefixed", "Prefixed:UnalignedRead");

	ASSERT_NE (t.function, nullptr) << t.error;
	EXPECT_EQ (t.count (", align 1"), 1u) << t.text ();
}

// constrained. on a reference type dereferences the pointer and dispatches as usual;
// on a value type that implements the method it calls the implementation directly
// with the pointer as this - no boxing, and no vtable in sight.
TEST_F (TranslatorTest, ConstrainedResolvesTheReceiver)
{
	const Translation &on_class = translate ("prefixed", "Prefixed:ConstrainedOnClass");
	const Translation &on_struct = translate ("prefixed", "Prefixed:ConstrainedOnStruct");

	ASSERT_NE (on_class.function, nullptr) << on_class.error;
	/* Overridable method: dispatched through the vtable, never named. */
	EXPECT_EQ (on_class.count ("Chatty:Talk"), 0u) << on_class.text ();

	ASSERT_NE (on_struct.function, nullptr) << on_struct.error;
	EXPECT_GE (on_struct.count ("Wrapped:ToString"), 1u) << on_struct.text ();
	EXPECT_EQ (on_struct.count ("mono_object_new_specific"), 0u);
}

// A value type that does not override the method boxes its receiver: the value loads
// through the managed pointer into a fresh box, and the call dispatches on the box's
// vtable. The second case buries the receiver under an argument, so the box has to
// land in the right stack slot.
TEST_F (TranslatorTest, ConstrainedCallOnANonOverridingStructBoxes)
{
	const Translation &plain = translate ("prefixed", "Prefixed:ConstrainedBoxes");
	const Translation &buried = translate ("prefixed", "Prefixed:ConstrainedBoxesWithArg");

	ASSERT_NE (plain.function, nullptr) << plain.error;
	EXPECT_EQ (plain.count ("mono_object_new_specific"), 1u) << plain.text ();
	EXPECT_EQ (plain.count ("mono_vtable_Bare"), 1u);
	/* Dispatch stays virtual: the target comes off the box's vtable, never named. */
	EXPECT_EQ (plain.count ("call ptr @\"System.Object:ToString"), 0u);

	ASSERT_NE (buried.function, nullptr) << buried.error;
	EXPECT_EQ (buried.count ("mono_object_new_specific"), 1u) << buried.text ();
}

/* ---------------------------------------------------------- method pointers */

// ldftn's answer is the callee's own function symbol - the engine resolves it to the
// entry point. When calli's pointer is that constant, the builder folds the whole
// thing back into a direct call; only a pointer nothing knows stays indirect.
TEST_F (TranslatorTest, LdftnPushesTheCalleeAndCalliCallsThroughIt)
{
	const Translation &take = translate ("fnptr", "Fnptr:TakeStatic");
	const Translation &folded = translate ("fnptr", "Fnptr:CallThroughPointer");
	const Translation &indirect = translate ("fnptr", "Fnptr:CallThroughArgument");

	ASSERT_NE (take.function, nullptr) << take.error;
	EXPECT_GE (take.count ("Fnptr:AddOne"), 1u);

	ASSERT_NE (folded.function, nullptr) << folded.error;
	EXPECT_EQ (folded.count ("call i32 @\"Fnptr:AddOne"), 1u) << folded.text ();

	ASSERT_NE (indirect.function, nullptr) << indirect.error;
	EXPECT_EQ (indirect.count ("call i32 %"), 1u) << indirect.text ();
}

TEST_F (TranslatorTest, LdvirtftnAsksTheRuntime)
{
	const Translation &t = translate ("fnptr", "Fnptr:TakeVirtual");

	ASSERT_NE (t.function, nullptr) << t.error;
	EXPECT_EQ (t.count ("mono_ldvirtfn"), 1u);
	EXPECT_EQ (t.count ("@\"mono_method_"), 1u);
}

/* ----------------------------------------------------------------- newobj */

TEST_F (TranslatorTest, NewobjAllocatesThenCallsTheConstructor)
{
	const Translation &t = translate ("objects", "Objects:MakeCounterAt");

	ASSERT_NE (t.function, nullptr) << t.error;
	EXPECT_EQ (t.count ("mono_object_new_specific"), 1u);
	EXPECT_EQ (t.count ("mono_vtable_Counter"), 1u);
	EXPECT_GE (t.count ("Counter:.ctor"), 1u);
}

// A value type constructs in place: no heap allocation, a zeroed slot handed to the
// constructor as this, and the value loaded back out.
TEST_F (TranslatorTest, ValueTypeNewobjConstructsInATemp)
{
	const Translation &t = translate ("objects", "Objects:MakePoint");

	ASSERT_NE (t.function, nullptr) << t.error;
	EXPECT_EQ (t.count ("mono_object_new_specific"), 0u);
	EXPECT_EQ (t.count ("llvm.memset"), 1u);
	EXPECT_GE (t.count ("Point:.ctor"), 1u);
}

/* ----------------------------------------------------------------- tokens */

// The interned string is a runtime object, so it rides on a symbol the engine
// resolves - and the same literal twice is the same symbol, because interning
// already promises one object.
TEST_F (TranslatorTest, LdstrBecomesOneSymbolPerLiteral)
{
	const Translation &once = translate ("tokens", "Tokens:Hello");
	const Translation &twice = translate ("tokens", "Tokens:SameLiteralTwice");

	ASSERT_NE (once.function, nullptr) << once.error;
	EXPECT_EQ (once.count ("@mono_ldstr_tokens_"), 1u) << once.text ();

	ASSERT_NE (twice.function, nullptr) << twice.error;
	EXPECT_EQ (twice.count ("@mono_ldstr_tokens_"), 2u);

	size_t distinct = 0;

	for (const llvm::GlobalVariable &global : twice.module->globals ())
		if (global.getName ().starts_with ("mono_ldstr_tokens_"))
			++distinct;
	EXPECT_EQ (distinct, 1u);
}

// A type's handle is its MonoType, which lives inside the MonoClass - so ldtoken on
// a type is the class symbol plus an offset, wrapped into a RuntimeTypeHandle.
TEST_F (TranslatorTest, LdtokenWrapsTheMatchingSymbolFamily)
{
	const Translation &type = translate ("tokens", "Tokens:TypeOf");
	const Translation &method = translate ("tokens", "Tokens:MethodToken");
	const Translation &field = translate ("tokens", "Tokens:FieldToken");

	ASSERT_NE (type.function, nullptr) << type.error;
	EXPECT_EQ (type.count ("@mono_class_Holder"), 1u) << type.text ();

	ASSERT_NE (method.function, nullptr) << method.error;
	EXPECT_EQ (method.count ("@\"mono_method_"), 1u) << method.text ();

	ASSERT_NE (field.function, nullptr) << field.error;
	EXPECT_EQ (field.count ("@\"mono_field_"), 1u) << field.text ();
}

/* ------------------------------------------------------------------ casts */

TEST_F (TranslatorTest, CastclassAndIsinstUseTheirOwnHelpers)
{
	const Translation &cast = translate ("casts", "Casts:CastString");
	const Translation &test = translate ("casts", "Casts:IsString");

	ASSERT_NE (cast.function, nullptr) << cast.error;
	EXPECT_EQ (cast.count ("mono_object_castclass_with_cache"), 1u);
	ASSERT_NE (test.function, nullptr) << test.error;
	EXPECT_EQ (test.count ("mono_object_isinst_with_cache"), 1u);
}

// The helpers memoize the last vtable that answered, so every cast site needs a
// cache slot of its very own - two sites sharing one would fight over it.
TEST_F (TranslatorTest, EachCastSiteGetsItsOwnCacheSlot)
{
	const Translation &t = translate ("casts", "Casts:TwoCasts");

	ASSERT_NE (t.function, nullptr) << t.error;
	EXPECT_EQ (t.count ("@cast_cache"), 2u) << t.text ();

	const llvm::GlobalVariable *first = t.module->getGlobalVariable ("cast_cache", true);

	ASSERT_NE (first, nullptr);
	EXPECT_TRUE (first->hasInternalLinkage ());
}

TEST_F (TranslatorTest, UnboxAnyOnAReferenceTypeIsACast)
{
	const Translation &t = translate ("casts", "Casts:UnboxAnyString");

	ASSERT_NE (t.function, nullptr) << t.error;
	EXPECT_EQ (t.count ("mono_object_castclass_with_cache"), 1u);
	EXPECT_EQ (t.count ("mono_object_new_specific"), 0u);
}

/* --------------------------------------------------------------- refusals */

struct RefusalRef {
	const char *method;
	const char *expected;
};

void
PrintTo (const RefusalRef &ref, std::ostream *os)
{
	*os << ref.method << " -> '" << ref.expected << "'";
}

class Refuses : public TranslatorTest, public testing::WithParamInterface<RefusalRef> {};

TEST_P (Refuses, WithAnErrorRatherThanACrash)
{
	const Translation &t = translate ("refused", GetParam ().method);

	EXPECT_EQ (t.function, nullptr) << "unexpectedly translated:\n" << t.text ();
	EXPECT_NE (t.error.find (GetParam ().expected), std::string::npos)
		<< "expected an error mentioning '" << GetParam ().expected
		<< "', got: " << t.error;
}

const RefusalRef refusals[] = {
	{"Refused:UsesAStringCtor", "string constructor"},
	{"Refused:StackUnderflow", "stack"},
	{"Refused:BadLocalIndex", "local"},
	{"Refused:FallsOffTheEnd", "return"},
	{"Refused:UsesJmp", "jmp"},
	{"Refused:UsesArglist", "arglist"},
	{"Refused:UsesMkrefany", "mkrefany"},
};

INSTANTIATE_TEST_SUITE_P (Corpus, Refuses, testing::ValuesIn (refusals),
                          [] (const testing::TestParamInfo<RefusalRef> &info) {
				  std::string name = info.param.method;

				  for (char &c : name)
					  if (!isalnum (static_cast<unsigned char> (c)))
						  c = '_';
				  return name;
			  });

} // namespace

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

// For buffer_copy_for (), which one case asks directly rather than through the
// IR, because the sibling it has to refuse has no reachable call site to
// translate.
#include "analysis/operand-class.hpp"
#include "method-to-llvm.hpp"
#include "passes/gc-barrier.hpp"

#include "config.h"
#include <glib.h>

#include "mono/metadata/class-init.h"
#include "mono/metadata/class-internals.h"
#include "mono/metadata/debug-helpers.h"
#include "mono/metadata/gc-internals.h"

#include <llvm/IR/CFG.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/IR/InstrTypes.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>

#include <cstdlib>
#include <cstring>
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
	{"flow", "Flow:MergeIntWithNativeInt"},
	{"flow", "Flow:MergeFloatWidths"},
	{"flow", "Flow:MergePointerWithNativeInt"},
	{"flow", "Flow:DeadBlockBeforeJoin"},

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
	{"fields", "Fields:GetPerThread"},
	{"fields", "Fields:SetPerThread"},
	{"fields", "Fields:GetBaked"},
	{"fields", "Fields:SetMixed"},
	{"fields", "Fields:SetStaticMixed"},
	{"fields", "Fields:SetFlat"},

	{"arrays", "Arrays:Length"},
	{"arrays", "Arrays:GetInt"},
	{"arrays", "Arrays:GetByte"},
	{"arrays", "Arrays:GetDouble"},
	{"arrays", "Arrays:GetRef"},
	{"arrays", "Arrays:SetInt"},
	{"arrays", "Arrays:SetRef"},
	{"arrays", "Arrays:SetMixedElem"},
	{"arrays", "Arrays:SetFlatElem"},
	{"arrays", "Arrays:Make"},
	{"arrays", "Arrays:ElementAddress"},
	{"arrays", "Arrays:Sum"},
	{"arrays", "Arrays:Make2D"},
	{"arrays", "Arrays:MakeBounded"},
	{"arrays", "Arrays:Make5D"},

	{"eh", "Eh:TryCatch"},
	{"eh", "Eh:TwoCatches"},
	{"eh", "Eh:TryFinally"},
	{"eh", "Eh:TryFault"},
	{"eh", "Eh:TwoLeaves"},
	{"eh", "Eh:NestedFinally"},
	{"eh", "Eh:CatchInsideFinally"},
	{"eh", "Eh:Throw"},
	{"eh", "Eh:Rethrow"},
	{"eh", "Eh:CallInTry"},
	{"eh", "Eh:CallUnderCatchAndFinally"},

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
	{"prefixed", "Prefixed:VolatileWriteRef"},
	{"prefixed", "Prefixed:VolatileReadStruct"},
	{"prefixed", "Prefixed:UnalignedVolatileRead"},
	{"prefixed", "Prefixed:ConstrainedOnClass"},
	{"prefixed", "Prefixed:ConstrainedOnStruct"},
	{"prefixed", "Prefixed:ConstrainedBoxes"},
	{"prefixed", "Prefixed:ConstrainedBoxesWithArg"},
	{"prefixed", "Prefixed:TailCall"},

	{"fnptr", "Fnptr:TakeStatic"},
	{"fnptr", "Fnptr:TakeVirtual"},
	{"fnptr", "Fnptr:CallThroughPointer"},
	{"fnptr", "Fnptr:CallThroughArgument"},
	{"fnptr", "Fnptr:CallNative"},
	{"fnptr", "Fnptr:TailThroughPointer"},

	{"typedref", "TypedRef:MakeRef"},
	{"typedref", "TypedRef:ReadBack"},
	{"typedref", "TypedRef:TypeOfRef"},
	{"typedref", "TypedRef:ValueOfRef"},
	{"typedref", "TypedRef:CountVarargs"},
	{"typedref", "TypedRef:CallsAVararg"},

	{"objects", "Objects:MakeCounter"},
	{"objects", "Objects:MakeCounterAt"},
	{"objects", "Objects:MakePoint"},
	{"objects", "Objects:UseThePoint"},
	{"objects", "Objects:MakeString"},

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
	{"calls", "Calls:TailStatic"},
	{"calls", "Calls:TailNarrow"},
	{"calls", "Calls:TailVoid"},
	{"calls", "Calls:TailVirtual"},
	{"calls", "Calls:TailMismatch"},
	{"calls", "Calls:TailMerged"},
	{"calls", "Calls:TailTwoWays"},
	{"calls", "Calls:TailByref"},
	{"calls", "Calls:JumpsToHelper"},
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

// A plain fptosi is poison out of range, and LLVM folds poison to zero wherever it can
// see the operand. The constrained intrinsic is what keeps every conversion on the
// target's own instruction instead.
TEST_F (TranslatorTest, FloatConversionUsesTheConstrainedIntrinsic)
{
	const Translation &s = translate ("arith", "Arith:ConvLongFromDouble");

	ASSERT_NE (s.function, nullptr) << s.error;
	EXPECT_EQ (s.count ("llvm.experimental.constrained.fptosi.i64.f64"), 1u) << s.text ();
	EXPECT_EQ (s.count ("fptosi double"), 0u) << s.text ();

}

// amd64 converts to an unsigned int64 by halves, and mono_fconv_u8 () settles which half
// a NaN goes down. The signed intrinsic under a test on "below 2^63" is that shape.
TEST_F (TranslatorTest, UnsignedLongFromFloatConvertsByHalves)
{
	const Translation &t = translate ("arith", "Arith:ConvULongFromDouble");

	ASSERT_NE (t.function, nullptr) << t.error;
	EXPECT_EQ (t.count ("llvm.experimental.constrained.fptosi.i64.f64"), 1u) << t.text ();
	EXPECT_EQ (t.count ("fcmp olt double"), 1u) << t.text ();
	EXPECT_EQ (t.count ("fsub double"), 1u) << t.text ();
}

// The width matters, not just the intrinsic: anything wider than the target legalizes
// to i128, and codegen lowers a 128-bit conversion to a compiler-rt call that nothing
// in the JIT's link order defines.
TEST_F (TranslatorTest, CheckedFloatConversionConvertsAtTheTargetWidth)
{
	const Translation &s = translate ("arith", "Arith:ConvOvfLongFromDouble");

	ASSERT_NE (s.function, nullptr) << s.error;
	EXPECT_EQ (s.count ("llvm.experimental.constrained.fptosi.i64.f64"), 1u) << s.text ();

	const Translation &u = translate ("arith", "Arith:ConvOvfULongFromDouble");

	ASSERT_NE (u.function, nullptr) << u.error;
	EXPECT_EQ (u.count ("llvm.experimental.constrained.fptoui.i64.f64"), 1u) << u.text ();
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

// The paths into a join can disagree on a slot's representation. The slot keeps the
// type the first path gave it and the later edge converts on the way in, which is
// the direction mini merges too.
TEST_F (TranslatorTest, DivergentJoinsConvertTowardTheFirstPath)
{
	const Translation &ints = translate ("flow", "Flow:MergeIntWithNativeInt");
	const Translation &floats = translate ("flow", "Flow:MergeFloatWidths");
	const Translation &pointers = translate ("flow", "Flow:MergePointerWithNativeInt");

	ASSERT_NE (ints.function, nullptr) << ints.error;
	/* The int32 edge registered the slot first, so the native int truncates in. */
	EXPECT_GE (ints.count ("trunc"), 1u) << ints.text ();

	ASSERT_NE (floats.function, nullptr) << floats.error;
	/* float64 registered first, so the float32 edge extends. */
	EXPECT_EQ (floats.count ("fpext"), 1u) << floats.text ();

	ASSERT_NE (pointers.function, nullptr) << pointers.error;
	EXPECT_GE (pointers.count ("ptrtoint"), 1u) << pointers.text ();
}

// Unreachable IL is legal and compilers emit it. Translating it means inventing an
// entry stack for it, which mistypes its own body and lets it settle the entry stack
// of the live block it falls into, so the block is left empty instead.
TEST_F (TranslatorTest, UnreachableBlocksAreNotTranslated)
{
	const Translation &t = translate ("flow", "Flow:DeadBlockBeforeJoin");

	ASSERT_NE (t.function, nullptr) << t.error;
	EXPECT_EQ (t.count ("unreachable"), 1u) << t.text ();
}

/* ----------------------------------------------------------------- fields */

TEST_F (TranslatorTest, InstanceFieldAccessNullChecks)
{
	const Translation &t = translate ("fields", "Fields:GetX");

	ASSERT_NE (t.function, nullptr) << t.error;
	EXPECT_GE (t.count ("make.implicit"), 1u);
}

// The store the method asked for is an instruction of its own and the barrier
// stands beside it, so what the field holds is visible until the lowering. The
// card itself is that lowering's, and gc-barrier-tests.cpp reads it there.
TEST_F (TranslatorTest, AReferenceStoreStandsBesideItsBarrier)
{
	const Translation &field = translate ("fields", "Fields:SetRef");
	const Translation &statics = translate ("fields", "Fields:SetStaticRef");

	ASSERT_NE (field.function, nullptr) << field.error;
	EXPECT_EQ (field.count ("call void @mono.gc.wbarrier"), 1u) << field.text ();
	EXPECT_EQ (field.count ("wb_mark"), 0u) << field.text ();
	EXPECT_EQ (field.count ("@mono_gc_card_table"), 0u) << field.text ();

	ASSERT_NE (statics.function, nullptr) << statics.error;
	EXPECT_EQ (statics.count ("call void @mono.gc.wbarrier"), 1u) << statics.text ();

	llvm::CallBase *site = nullptr;

	for (llvm::Instruction &in : llvm::instructions (*field.function))
		if (auto *call = llvm::dyn_cast<llvm::CallBase> (&in))
			if (call->getCalledFunction () != nullptr
			    && call->getCalledFunction ()->getName () == mono::gc_barrier_name)
				site = call;

	ASSERT_NE (site, nullptr);

	auto *store = llvm::dyn_cast_or_null<llvm::StoreInst> (site->getPrevNode ());

	ASSERT_NE (store, nullptr) << field.text ();
	EXPECT_EQ (store->getPointerOperand (), site->getArgOperand (0));
	EXPECT_EQ (store->getValueOperand (), site->getArgOperand (1));
}

// The collector runs for the life of the process, so the shape of its card path
// is one fact about the module rather than one about each site. A concurrent
// major collector is the arm that reads a flag as well as the value, and the
// address of that flag is what says so.
TEST_F (TranslatorTest, AConcurrentCollectorsFlagRidesOnTheDeclaration)
{
	if (mono_gc_card_table_nursery_check ()
	    || mono_gc_get_concurrent_collection_flag () == nullptr)
		GTEST_SKIP () << "this collector collects nothing concurrently";

	const Translation &field = translate ("fields", "Fields:SetRef");

	ASSERT_NE (field.function, nullptr) << field.error;

	llvm::Function *decl = field.module->getFunction (mono::gc_barrier_name);

	ASSERT_NE (decl, nullptr) << field.text ();
	EXPECT_TRUE (decl->hasFnAttribute ("mono-gc-card-table"));
	EXPECT_TRUE (decl->hasFnAttribute ("mono-gc-concurrent-flag"));
	EXPECT_FALSE (decl->hasFnAttribute ("mono-gc-value-decides"));
}

// A value type that holds references moves as one call, which is the copy and
// the cards together. A fold behind the translator is what takes that call apart
// where the IR says an open copy is safe. A value type with no references owes
// no cards and copies alone.
TEST_F (TranslatorTest, StoringARefStructCopiesThroughTheCollector)
{
	const std::string copy = std::string (mono::gc_value_copy_name);

	EXPECT_EQ (translate ("fields", "Fields:SetMixed").count (copy), 1u);
	EXPECT_EQ (translate ("fields", "Fields:SetStaticMixed").count (copy), 1u);
	EXPECT_EQ (translate ("arrays", "Arrays:SetMixedElem").count (copy), 1u);

	// The translator leaves the copy inside that call. A memmove is what the
	// fold behind it writes for a site whose two addresses can overlap, and no
	// other site here asks for one.
	EXPECT_EQ (translate ("fields", "Fields:SetMixed").count ("llvm.memmove"), 0u);

	EXPECT_EQ (translate ("fields", "Fields:SetFlat").count (copy), 0u);
	EXPECT_EQ (translate ("arrays", "Arrays:SetFlatElem").count (copy), 0u);
}

// The IL says nothing about the two addresses a stobj names, so the site keeps
// the fold on a memmove. A box is the site that says otherwise, below.
TEST_F (TranslatorTest, AStoredRefStructCanOverlap)
{
	const Translation &field = translate ("fields", "Fields:SetMixed");
	const llvm::CallInst *copy = nullptr;

	ASSERT_NE (field.function, nullptr) << field.error;

	for (const llvm::Instruction &in : llvm::instructions (*field.function)) {
		const auto *call = llvm::dyn_cast<llvm::CallInst> (&in);

		if (call != nullptr && call->getCalledFunction () != nullptr
		    && call->getCalledFunction ()->getName () == mono::gc_value_copy_name)
			copy = call;
	}

	ASSERT_NE (copy, nullptr) << field.text ();
	EXPECT_FALSE (copy->hasFnAttr (mono::gc_no_overlap_attr)) << field.text ();

	// The class's own alignment rides on the site, because the fold has no
	// other way to read it back.
	EXPECT_TRUE (copy->getParamAlign (0).has_value ()) << field.text ();
	EXPECT_TRUE (copy->getParamAlign (1).has_value ()) << field.text ();
}

// One relocation per class, not per field: both loads resolve through the same
// mono_statics_ block and differ only in the offset.
TEST_F (TranslatorTest, StaticsOfOneClassShareOneSymbol)
{
	const Translation &t = translate ("fields", "Fields:TwoStatics");

	ASSERT_NE (t.function, nullptr) << t.error;

	size_t blocks = 0;
	size_t holder = 0;

	/* Names carry the class's pointer, so match on the prefix rather than whole. */
	for (const llvm::GlobalVariable &global : t.module->globals ())
		if (global.getName ().starts_with ("mono_statics_")) {
			++blocks;
			if (global.getName ().starts_with ("mono_statics_Holder"))
				++holder;
		}

	EXPECT_EQ (blocks, 1u) << t.text ();
	EXPECT_EQ (holder, 1u) << t.text ();
}

// A thread-static's recorded offset is a per-thread lookup cookie, not a place in
// the statics block, so its address comes from the runtime on every access - and
// never through mono_statics_.
TEST_F (TranslatorTest, ThreadStaticsAskTheRuntimeForTheirAddress)
{
	const Translation &load = translate ("fields", "Fields:GetPerThread");
	const Translation &store = translate ("fields", "Fields:SetPerThread");

	ASSERT_NE (load.function, nullptr) << load.error;
	EXPECT_EQ (load.count ("mono_class_static_field_address"), 1u) << load.text ();
	EXPECT_EQ (load.count ("mono_domain_get"), 1u);
	EXPECT_EQ (load.count ("mono_field_Holder:PerThread"), 1u);
	EXPECT_EQ (load.count ("mono_statics_"), 0u);

	ASSERT_NE (store.function, nullptr) << store.error;
	EXPECT_EQ (store.count ("mono_class_static_field_address"), 1u) << store.text ();
}

// An RVA field needs nothing special once the vtable exists: creating it copies the
// image data into the statics block, so the ordinary block-plus-offset address is
// the right one.
TEST_F (TranslatorTest, RvaStaticsLiveInTheOrdinaryStaticsBlock)
{
	const Translation &t = translate ("fields", "Fields:GetBaked");

	ASSERT_NE (t.function, nullptr) << t.error;
	EXPECT_EQ (t.count ("mono_statics_Holder"), 1u) << t.text ();
	EXPECT_EQ (t.count ("mono_class_static_field_address"), 0u);
}

/* ----------------------------------------------------------------- arrays */

/*
 * What a newarr allocates through is the collector's answer: emit_vector_alloc ()
 * calls the collector's array allocator where there is one, and the runtime's
 * array-new icall where there is none. `mono/tests/newarr-refusal.cs` covers what
 * the two arms do, on both collectors.
 *
 * The case below covers what behavior cannot see. A fall back to the icall costs
 * a call and a TLAB bump on every allocation and breaks nothing, so a lost fast
 * path is silent. The case asserts the absence of the icall rather than the
 * presence of the allocator: the property is that this collector's fast path is
 * taken, not how it is written.
 */
TEST_F (TranslatorTest, NewarrDoesNotFallBackWhereTheCollectorAllocates)
{
	MonoClass *array = mono_class_create_array (mono_defaults.int32_class, 1);

	if (mono_gc_get_managed_array_allocator (array) == nullptr)
		GTEST_SKIP () << "this collector has no array allocator";

	const Translation &t = translate ("arrays", "Arrays:Make");

	ASSERT_NE (t.function, nullptr) << t.error;
	EXPECT_EQ (t.count ("ves_icall_array_new_specific"), 0u) << t.text ();
	EXPECT_GE (t.count ("mono_vtable_"), 1u);
}

// Multi-dimensional arrays construct through newobj on a bodyless metadata ctor; the
// runtime icalls implement it, keyed by that ctor's method. Two int32 lengths take
// the direct mono_array_new_2; bounds pairs and rank five deinterleave into a buffer
// for mono_array_new_n_icall - bounds first, lengths after.
TEST_F (TranslatorTest, ArrayNewobjCallsTheRuntimeArrayIcalls)
{
	const Translation &two = translate ("arrays", "Arrays:Make2D");
	const Translation &bounded = translate ("arrays", "Arrays:MakeBounded");
	const Translation &five = translate ("arrays", "Arrays:Make5D");

	ASSERT_NE (two.function, nullptr) << two.error;
	EXPECT_EQ (two.count ("mono_array_new_2"), 1u) << two.text ();
	EXPECT_GE (two.count ("mono_method_"), 1u);

	/* The allocation's whole claim is NoAlias: an allockind would let a dead
	 * allocation - and its catchable OutOfMemoryException - be deleted. The
	 * call goes through the icall wrapper, whose name decorates the icall's. */
	llvm::Function *allocator = nullptr;
	for (llvm::Function &f : *two.module)
		if (f.getName ().contains ("mono_array_new_2"))
			allocator = &f;
	ASSERT_NE (allocator, nullptr);
	EXPECT_TRUE (allocator->hasRetAttribute (llvm::Attribute::NoAlias));
	EXPECT_FALSE (allocator->hasFnAttribute (llvm::Attribute::AllocKind));

	ASSERT_NE (bounded.function, nullptr) << bounded.error;
	EXPECT_EQ (bounded.count ("mono_array_new_n_icall"), 1u) << bounded.text ();
	/* Two sign-extended bounds and two zero-extended lengths land in the buffer. */
	EXPECT_EQ (bounded.count ("sext"), 2u);
	EXPECT_EQ (bounded.count ("zext"), 2u);

	ASSERT_NE (five.function, nullptr) << five.error;
	EXPECT_EQ (five.count ("mono_array_new_n_icall"), 1u) << five.text ();
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

// The runtime resumes at the innermost pad with the chosen clause's index in the
// selector register, so the pad has to carry the clause's marker and route on the
// selector rather than branching straight to one handler.
TEST_F (TranslatorTest, APadNamesItsClausesAndRoutesBySelector)
{
	const Translation &t = translate ("eh", "Eh:TryCatch");

	ASSERT_NE (t.function, nullptr) << t.error;
	EXPECT_GE (t.count ("catch ptr @\"mono_eh_clause_0@"), 1u);
	EXPECT_GE (t.count ("switch i32"), 1u);
}

// A catch nested inside another protected region can only be reached through the
// inner pad, so the pad's chain has to name the enclosing clause as well.
TEST_F (TranslatorTest, ANestedPadCarriesTheEnclosingClause)
{
	const Translation &t = translate ("eh", "Eh:CallUnderCatchAndFinally");

	ASSERT_NE (t.function, nullptr) << t.error;
	EXPECT_GE (t.count ("catch ptr @\"mono_eh_clause_0@"), 1u);
	EXPECT_GE (t.count ("catch ptr @\"mono_eh_clause_1@"), 1u);
}

// A finally entered by unwinding ends by handing control back to the unwinder,
// which saved where it was; falling off the end would be resuming nowhere.
TEST_F (TranslatorTest, AnUnwoundFinallyResumesThroughTheRuntime)
{
	const Translation &t = translate ("eh", "Eh:TryFinally");

	ASSERT_NE (t.function, nullptr) << t.error;
	EXPECT_GE (t.count ("mono_llvm_resume_unwind"), 1u);
}

// A thread aborted inside a finally has to finish it first. The request only sets a
// byte in the frame, so the handler's exit is what has to notice and deliver, and the
// markers bracketing the body are what tell the runtime the frame is in there at all.
TEST_F (TranslatorTest, AFinallyHoldsAnAbortInUntilItIsDone)
{
	const Translation &t = translate ("eh", "Eh:TryFinally");

	ASSERT_NE (t.function, nullptr) << t.error;
	EXPECT_EQ (t.count ("%abort_guard0 = alloca i8"), 1u) << t.text ();
	/* One marker opening the body, one closing it at the endfinally. */
	EXPECT_EQ (t.count ("llvm.experimental.stackmap"), 2u) << t.text ();
	/* Written from another thread, so the exit has to go back to memory for it. */
	EXPECT_EQ (t.count ("load volatile i8, ptr %abort_guard0"), 1u) << t.text ();
	EXPECT_GE (t.count ("ves_icall_thread_finish_async_abort"), 1u) << t.text ();
}

// A fault runs only while an exception is already on its way out, where the runtime
// carries the abort behind it - there is nothing to defer and no byte to defer it in.
TEST_F (TranslatorTest, AFaultCarriesNoAbortGuard)
{
	const Translation &t = translate ("eh", "Eh:TryFault");

	ASSERT_NE (t.function, nullptr) << t.error;
	EXPECT_EQ (t.count ("abort_guard"), 0u) << t.text ();
	EXPECT_EQ (t.count ("llvm.experimental.stackmap"), 0u) << t.text ();
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

// Every endfinally in a handler gets the same set of continuations. Filling in
// only one of them leaves the others sending an ordinary leave to the unwinder,
// which has no unwind in progress to resume.
TEST_F (TranslatorTest, EveryEndfinallyCarriesTheLeaveOn)
{
	const Translation &t = translate ("eh", "Eh:TwoEndfinallys");

	ASSERT_NE (t.function, nullptr) << t.error;
	EXPECT_EQ (t.count ("switch i32"), 2u);
	EXPECT_EQ (t.count ("i32 1, label"), 2u);
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

	// The lookup is a mono.vtable.func call, which keeps the vtable and the slot
	// as operands for fold_dispatch_sites () to read.
	EXPECT_EQ (overridable.count ("@mono.vtable.func"), 1u) << overridable.text ();

	ASSERT_NE (final_method.function, nullptr) << final_method.error;
	EXPECT_GE (final_method.count ("Base:Sealed"), 1u);
	EXPECT_EQ (final_method.count ("@mono.vtable.func"), 0u) << final_method.text ();

	ASSERT_NE (instance.function, nullptr) << instance.error;
	EXPECT_GE (instance.count ("Base:Inst"), 1u);
	EXPECT_EQ (instance.count ("@mono.vtable.func"), 0u) << instance.text ();
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

// A tail. call whose prototype matches the caller's is honored as a musttail
// call - a guaranteed jump, which is the only form that fails loudly rather than
// quietly, and so the only one worth using where III.2.4 makes the jump
// mandatory. A dispatched or indirect target is a legacy-boundary call whose
// prototype MonoAbiPass may yet change, so the guarantee cannot be demanded of
// one - and musttail across the two conventions is not even well-formed IR.
TEST_F (TranslatorTest, AMatchingTailCallIsHonoredAsMustTail)
{
	EXPECT_EQ (translate ("calls", "Calls:TailStatic").count ("musttail call"), 1u);
	EXPECT_EQ (translate ("calls", "Calls:TailVoid").count ("musttail call"), 1u);
	EXPECT_EQ (translate ("calls", "Calls:TailVirtual").count ("musttail call"), 0u);
	EXPECT_EQ (translate ("fnptr", "Fnptr:TailThroughPointer").count ("musttail call"),
	           0u);
}

// A dispatched site still hands its frame away: the jump reaches a stub as readily
// as a call does, and the IMT key rides a register of its own that the jump leaves
// alone. Left as an ordinary call instead, a tail. callvirt recursing in constant
// space overflows the stack. The key has to survive onto the marked site - a jump
// that lost it dispatches on nothing.
//
// And a dispatched site is not a boundary call any more. What a vtable slot holds
// is the method's stub, entered in this backend's own convention like anything
// else, so nothing is left for MonoAbiPass to lower.
TEST_F (TranslatorTest, ADispatchedTailCallIsMarkedAndKeepsItsKey)
{
	/* The marker is an attribute group in the printed text, so it is read here. */
	auto legacy_sites = [] (const Translation &t) {
		unsigned n = 0;

		for (const llvm::BasicBlock &block : *t.function)
			for (const llvm::Instruction &instruction : block) {
				const auto *call =
					llvm::dyn_cast<llvm::CallBase> (&instruction);

				if (call != nullptr
				    && call->getFnAttr ("monocc").isValid ())
					n++;
			}
		return n;
	};

	const Translation &vtable = translate ("calls", "Calls:TailVirtual");
	const Translation &imt = translate ("calls", "Calls:TailInterface");

	ASSERT_NE (vtable.function, nullptr) << vtable.error;
	EXPECT_EQ (vtable.count ("tail call"), 1u) << vtable.text ();
	EXPECT_EQ (vtable.count ("notail"), 0u) << vtable.text ();
	EXPECT_EQ (legacy_sites (vtable), 0u) << vtable.text ();

	ASSERT_NE (imt.function, nullptr) << imt.error;
	EXPECT_EQ (imt.count ("tail call"), 1u) << imt.text ();
	EXPECT_EQ (imt.count ("ptr nest @\"mono_method_"), 1u) << imt.text ();
	EXPECT_EQ (legacy_sites (imt), 0u) << imt.text ();
}

// Where the guarantee cannot be made the prefix is still worth asking for. A
// prototype that differs from the caller's is a jump the backend forms or does
// not, and a plain tail call is how that question gets put to it - demanding it
// would abort the process over a jump the prefix only ever permitted.
TEST_F (TranslatorTest, AnUnmatchedTailCallIsStillMarked)
{
	const Translation &t = translate ("calls", "Calls:TailMismatch");

	ASSERT_NE (t.function, nullptr) << t.error;
	EXPECT_EQ (t.count ("musttail"), 0u) << t.text ();
	EXPECT_EQ (t.count ("notail"), 0u) << t.text ();
	EXPECT_EQ (t.count ("tail call"), 1u) << t.text ();
}

// The site carries the callee's return attributes itself. Tail-call eligibility
// compares the caller's against the site's own list and does not fall back to the
// called function, so a musttail call that leaves the extension off reads as a
// mismatched ABI - and LLVM drops the tail call silently, which puts the frame back
// and turns a constant-space recursion into a stack overflow.
TEST_F (TranslatorTest, AMustTailCallCarriesTheReturnExtension)
{
	const Translation &t = translate ("calls", "Calls:TailNarrow");

	ASSERT_NE (t.function, nullptr) << t.error;
	EXPECT_EQ (t.count ("musttail call zeroext i8"), 1u) << t.text ();
}

// Declining a tail. prefix is always legal, and it is how the unsafe cases are
// handled. Both markers promise the callee touches nothing of this frame, so an
// argument that could point into it rules out either one - the site becomes an
// ordinary call, which the notail policy then covers like any other.
TEST_F (TranslatorTest, AnUnsafeTailCallFallsBackToAPlainCall)
{
	const Translation &t = translate ("calls", "Calls:TailByref");

	ASSERT_NE (t.function, nullptr) << t.error;
	EXPECT_EQ (t.count ("musttail"), 0u) << t.text ();
	EXPECT_EQ (t.count ("notail call"), 1u) << t.text ();
}

// Frames have to survive the optimizer: without notail, tailcallelim rewrites a
// self-recursive call into a loop and every recursive frame vanishes from the trace.
TEST_F (TranslatorTest, ACallIsMarkedNotail)
{
	const Translation &t = translate ("calls", "Calls:CallVoid");

	ASSERT_NE (t.function, nullptr) << t.error;
	EXPECT_GE (t.count ("notail call"), 1u) << t.text ();
}

// jmp transfers the current arguments to a matching method: they reload from
// their slots (so anything starg wrote goes along) into a musttail call whose
// result is returned directly.
TEST_F (TranslatorTest, JmpReloadsTheArgumentsIntoAMustTailCall)
{
	const Translation &t = translate ("calls", "Calls:JumpsToHelper");

	ASSERT_NE (t.function, nullptr) << t.error;
	EXPECT_EQ (t.count ("musttail call"), 1u) << t.text ();
	EXPECT_GE (t.count ("Calls:Helper"), 1u);
}

/* ----------------------------------------------------------------- boxing */

// This harness links SGen, which hands out a managed allocator for a class this
// small, so an allocation shows up as a call to that wrapper rather than to the
// runtime entry point ves_icall_object_new_specific.
TEST_F (TranslatorTest, BoxAllocatesWithTheVtableAndStoresTheValue)
{
	const Translation &t = translate ("boxing", "Boxing:BoxInt");

	ASSERT_NE (t.function, nullptr) << t.error;
	EXPECT_EQ (t.count ("object:AllocSmall"), 1u) << t.text ();
	/*
	 * mono_type_full_name spells System.Int32 as "int". The allocator takes the
	 * vtable and the box writes it back over the header, so the symbol is named
	 * at both.
	 */
	EXPECT_EQ (t.count ("mono_vtable_int"), 2u) << t.text ();
	EXPECT_EQ (t.count ("store ptr @\"mono_vtable_int"), 1u) << t.text ();
}

// A reference type's boxed form is itself, so nothing may be allocated.
TEST_F (TranslatorTest, BoxOnAReferenceTypeAllocatesNothing)
{
	const Translation &t = translate ("boxing", "Boxing:BoxObject");

	ASSERT_NE (t.function, nullptr) << t.error;
	EXPECT_EQ (t.count ("object:AllocSmall"), 0u);
	EXPECT_EQ (t.count ("ves_icall_object_new_specific"), 0u);
}

// A struct with a reference inside cannot be memcpy'd into the box: the copy
// owes the new object's cards. The site says the two addresses cannot overlap,
// because the destination is the object the box allocated above.
TEST_F (TranslatorTest, BoxingARefStructCopiesThroughTheCollector)
{
	const std::string copy = std::string (mono::gc_value_copy_name);
	const Translation &plain = translate ("boxing", "Boxing:BoxPair");
	const Translation &with_ref = translate ("boxing", "Boxing:BoxRefPair");
	const llvm::CallInst *site = nullptr;

	ASSERT_NE (with_ref.function, nullptr) << with_ref.error;
	EXPECT_EQ (with_ref.count (copy), 1u);
	ASSERT_NE (plain.function, nullptr) << plain.error;
	EXPECT_EQ (plain.count (copy), 0u);

	for (const llvm::Instruction &in : llvm::instructions (*with_ref.function)) {
		const auto *call = llvm::dyn_cast<llvm::CallInst> (&in);

		if (call != nullptr && call->getCalledFunction () != nullptr
		    && call->getCalledFunction ()->getName () == mono::gc_value_copy_name)
			site = call;
	}

	ASSERT_NE (site, nullptr) << with_ref.text ();
	EXPECT_TRUE (site->hasFnAttr (mono::gc_no_overlap_attr)) << with_ref.text ();
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
	EXPECT_EQ (t.count ("ves_icall_object_new_specific"), 0u);
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

// What a break means is the runtime's decision, so it calls the runtime rather
// than trapping: with a debugger client attached it raises a user-break event.
TEST_F (TranslatorTest, BreakCallsTheDebuggerAgent)
{
	const Translation &t = translate ("misc", "Misc:Breakpoint");

	ASSERT_NE (t.function, nullptr) << t.error;
	EXPECT_EQ (t.count ("debugger_agent_user_break"), 1u) << t.text ();
	EXPECT_EQ (t.count ("llvm.debugtrap"), 0u);
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

// A volatile read has acquire semantics and a volatile write release (I.12.6.7).
// Where the machine can make the access atomically the ordering rides the
// instruction itself, which is what scopes it to that one access; volatile stays
// on it because atomic alone does not promise the access survives.
TEST_F (TranslatorTest, VolatileScalarAccessesCarryTheirOrdering)
{
	const Translation &read = translate ("prefixed", "Prefixed:VolatileRead");
	const Translation &write = translate ("prefixed", "Prefixed:VolatileWrite");
	const Translation &statics = translate ("prefixed", "Prefixed:VolatileStatic");

	ASSERT_NE (read.function, nullptr) << read.error;
	EXPECT_EQ (read.count ("load atomic volatile i32"), 1u) << read.text ();
	EXPECT_EQ (read.count ("acquire"), 1u);
	EXPECT_EQ (read.count ("fence"), 0u);

	ASSERT_NE (write.function, nullptr) << write.error;
	EXPECT_EQ (write.count ("store atomic volatile i32"), 1u) << write.text ();
	EXPECT_EQ (write.count ("release"), 1u);
	EXPECT_EQ (write.count ("fence"), 0u);

	ASSERT_NE (statics.function, nullptr) << statics.error;
	EXPECT_EQ (statics.count ("load atomic volatile i32"), 1u) << statics.text ();
	EXPECT_EQ (statics.count ("fence"), 0u);
}

// An access no single atomic instruction covers keeps the older shape: a plain
// volatile access with a fence beside it. A reference carries a barrier, a value
// class is a copy, and an access the unaligned. prefix gave up the alignment of
// cannot be atomic at all.
TEST_F (TranslatorTest, VolatileFallsBackToAFence)
{
	const Translation &ref = translate ("prefixed", "Prefixed:VolatileWriteRef");
	const Translation &vtype = translate ("prefixed", "Prefixed:VolatileReadStruct");
	const Translation &loose = translate ("prefixed", "Prefixed:UnalignedVolatileRead");

	ASSERT_NE (ref.function, nullptr) << ref.error;
	EXPECT_EQ (ref.count ("fence release"), 1u) << ref.text ();
	EXPECT_EQ (ref.count ("call void @mono.gc.wbarrier"), 1u);

	ASSERT_NE (vtype.function, nullptr) << vtype.error;
	EXPECT_EQ (vtype.count ("fence acquire"), 1u) << vtype.text ();
	EXPECT_GE (vtype.count ("llvm.memcpy"), 1u);
	EXPECT_EQ (vtype.count ("atomic"), 0u);

	ASSERT_NE (loose.function, nullptr) << loose.error;
	EXPECT_EQ (loose.count ("load volatile i32"), 1u) << loose.text ();
	EXPECT_EQ (loose.count ("fence acquire"), 1u);
	EXPECT_EQ (loose.count ("atomic"), 0u);
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
	EXPECT_EQ (on_struct.count ("ves_icall_object_new_specific"), 0u);
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
	EXPECT_EQ (plain.count ("object:AllocSmall"), 1u) << plain.text ();
	/* The allocator takes the vtable, and the box writes it back over the header. */
	EXPECT_EQ (plain.count ("mono_vtable_Bare"), 2u) << plain.text ();
	EXPECT_EQ (plain.count ("store ptr @\"mono_vtable_Bare"), 1u) << plain.text ();
	/* Dispatch stays virtual: the target comes off the box's vtable, never named. */
	EXPECT_EQ (plain.count ("call ptr @\"System.Object:ToString"), 0u);

	ASSERT_NE (buried.function, nullptr) << buried.error;
	EXPECT_EQ (buried.count ("object:AllocSmall"), 1u) << buried.text ();
}

/* ---------------------------------------------------------- method pointers */

// calli through an unmanaged signature is not a raw call: the runtime's indirect
// native-func wrapper takes the pointer as its leading argument and owns the GC
// transition, so what this method emits is an ordinary call to the wrapper.
TEST_F (TranslatorTest, UnmanagedCalliGoesThroughTheTransitionWrapper)
{
	const Translation &t = translate ("fnptr", "Fnptr:CallNative");

	ASSERT_NE (t.function, nullptr) << t.error;
	EXPECT_EQ (t.count ("wrapper_native_indirect"), 1u) << t.text ();
}

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

/* ---------------------------------------------------------------- typedref */

// The descriptor mkrefany builds names the class twice: the klass field holds the
// class itself, and the type field points at the MonoType living inside it - the
// same symbol at an offset.
TEST_F (TranslatorTest, MkrefanyDescribesTheClassAndTheAddress)
{
	const Translation &t = translate ("typedref", "TypedRef:MakeRef");

	ASSERT_NE (t.function, nullptr) << t.error;
	EXPECT_GE (t.count ("mono_class_int"), 2u) << t.text ();
}

// refanyval only hands the address back when the descriptor's class is exactly the
// asked-for one; anything else is an InvalidCastException.
TEST_F (TranslatorTest, RefanyvalChecksTheClassBeforeTakingTheAddress)
{
	const Translation &t = translate ("typedref", "TypedRef:ValueOfRef");

	ASSERT_NE (t.function, nullptr) << t.error;
	EXPECT_EQ (t.count ("mono_class_int"), 1u) << t.text ();
	EXPECT_EQ (t.count ("throw_InvalidCastException"), 2u); // 1 block, label + branch
}

// A vararg method's variable arguments never become parameters: they travel in a
// buffer whose address is the one trailing pointer the declaration carries, so a
// method declaring one fixed int32 takes two arguments.
TEST_F (TranslatorTest, AVarargMethodTakesACookieBufferAsItsLastArgument)
{
	const Translation &t = translate ("typedref", "TypedRef:CountVarargs");

	ASSERT_NE (t.function, nullptr) << t.error;
	ASSERT_EQ (t.function->arg_size (), 2u) << t.text ();
	EXPECT_TRUE (t.function->getArg (1)->getType ()->isPointerTy ()) << t.text ();
}

// The call packs the variable part into that buffer behind the call-site
// signature: 8 bytes of cookie, an int32 in a whole slot, then a float in four.
TEST_F (TranslatorTest, AVarargCallPacksTheVariableArgumentsBehindTheSignature)
{
	const Translation &t = translate ("typedref", "TypedRef:CallsAVararg");

	ASSERT_NE (t.function, nullptr) << t.error;
	EXPECT_EQ (t.count ("mono_sig_"), 1u) << t.text ();
	/* The buffer runs to 8 + 8 + 4 + 8. */
	EXPECT_EQ (t.count ("alloca [28 x i8]"), 1u) << t.text ();
	EXPECT_EQ (t.count ("i64 8"), 1u) << t.text ();
	EXPECT_EQ (t.count ("i64 16"), 1u) << t.text ();
	EXPECT_EQ (t.count ("i64 20"), 1u) << t.text ();
}

/* ----------------------------------------------------------------- newobj */

TEST_F (TranslatorTest, NewobjAllocatesThenCallsTheConstructor)
{
	const Translation &t = translate ("objects", "Objects:MakeCounterAt");

	ASSERT_NE (t.function, nullptr) << t.error;
	EXPECT_EQ (t.count ("object:AllocSmall"), 1u) << t.text ();
	/* The allocator takes the vtable, and newobj writes it back over the header. */
	EXPECT_EQ (t.count ("mono_vtable_Counter"), 2u) << t.text ();
	EXPECT_EQ (t.count ("store ptr @\"mono_vtable_Counter"), 1u) << t.text ();
	EXPECT_GE (t.count ("Counter:.ctor"), 1u);
}

// Not every class gets a managed allocator - the collector declines a finalizable
// one, among others - and those sites go on allocating through the runtime.
TEST_F (TranslatorTest, NewobjOnAFinalizableClassAllocatesThroughTheRuntime)
{
	const Translation &t = translate ("objects", "Objects:MakeFinalized");

	ASSERT_NE (t.function, nullptr) << t.error;
	EXPECT_EQ (t.count ("object:AllocSmall"), 0u) << t.text ();
	EXPECT_EQ (t.count ("ves_icall_object_new_specific"), 1u) << t.text ();
}

// A value type constructs in place: no heap allocation, a zeroed slot handed to the
// constructor as this, and the value loaded back out.
TEST_F (TranslatorTest, ValueTypeNewobjConstructsInATemp)
{
	const Translation &t = translate ("objects", "Objects:MakePoint");

	ASSERT_NE (t.function, nullptr) << t.error;
	EXPECT_EQ (t.count ("object:AllocSmall"), 0u);
	EXPECT_EQ (t.count ("ves_icall_object_new_specific"), 0u);
	EXPECT_EQ (t.count ("llvm.memset"), 1u);
	EXPECT_GE (t.count ("Point:.ctor"), 1u);
}

// A string cannot be allocated before its length is known, so nothing is allocated
// here: newobj asks the builtin for the object the constructor built. The method it
// stands for is the wrapper the runtime publishes for a string constructor, which this
// backend compiles. How that wrapper is reached - the null this
// it never reads - is settled in lower_runtime_builtins (), and nothing about it is
// spelled here.
TEST_F (TranslatorTest, StringNewobjAsksTheBuiltinForTheObject)
{
	const Translation &t = translate ("objects", "Objects:MakeString");

	ASSERT_NE (t.function, nullptr) << t.error;
	EXPECT_EQ (t.count ("ves_icall_object_new_specific"), 0u);
	EXPECT_EQ (t.count ("call ptr @\"mono.builtin.string_constructor."
	                    "(wrapper managed-to-managed) string:.ctor"),
	           1u)
		<< t.text ();
	EXPECT_EQ (t.count ("(ptr null"), 0u);

	/* The result is fresh, and the declaration says so. */
	for (const llvm::Function &decl : t.module->functions ())
		if (decl.getName ().contains ("string:.ctor")
		    && !decl.getName ().starts_with ("mono.builtin.")) {
			EXPECT_TRUE (decl.hasRetAttribute (llvm::Attribute::NoAlias));
		}
}

// A plain call to a string constructor is the runtime-invoke wrapper's shape, and it
// leaves the string behind for the instruction after it: the placeholder this the
// caller pushed goes no further, and the object is what lands in the local.
TEST_F (TranslatorTest, StringCtorCalledDirectlyLeavesTheString)
{
	const Translation &t = translate ("objects", "Objects:CallStringCtor");

	ASSERT_NE (t.function, nullptr) << t.error;
	EXPECT_EQ (t.count ("call ptr @\"mono.builtin.string_constructor."
	                    "(wrapper managed-to-managed) string:.ctor"),
	           1u)
		<< t.text ();
	EXPECT_EQ (t.count ("ret ptr"), 1u) << t.text ();
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
	EXPECT_EQ (once.count ("@\"mono_ldstr_tokens_"), 1u) << once.text ();

	ASSERT_NE (twice.function, nullptr) << twice.error;
	EXPECT_EQ (twice.count ("@\"mono_ldstr_tokens_"), 2u);

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
	const Translation &type = translate ("tokens", "Tokens:TypeToken");
	const Translation &method = translate ("tokens", "Tokens:MethodToken");
	const Translation &field = translate ("tokens", "Tokens:FieldToken");

	ASSERT_NE (type.function, nullptr) << type.error;
	EXPECT_EQ (type.count ("@\"mono_class_Holder"), 1u) << type.text ();

	ASSERT_NE (method.function, nullptr) << method.error;
	EXPECT_EQ (method.count ("@\"mono_method_"), 1u) << method.text ();

	ASSERT_NE (field.function, nullptr) << field.error;
	EXPECT_EQ (field.count ("@\"mono_field_"), 1u) << field.text ();
}

// typeof (T) is an ldtoken and the call behind it, and the pair has one answer.
// The System.Type rides on a symbol of its own, and neither the class symbol nor
// the call is left.
TEST_F (TranslatorTest, TypeOfBecomesTheTypeObject)
{
	const Translation &t = translate ("tokens", "Tokens:TypeOf");

	ASSERT_NE (t.function, nullptr) << t.error;
	EXPECT_EQ (t.count ("@\"mono_typeof_Holder"), 1u) << t.text ();
	EXPECT_EQ (t.count ("GetTypeFromHandle"), 0u) << t.text ();
	EXPECT_EQ (t.count ("@\"mono_class_Holder"), 0u) << t.text ();
}

// The fold needs the call on every path the ldtoken is on. A call that starts a
// block is entered from elsewhere as well, so the handle and the call both stand.
TEST_F (TranslatorTest, TypeOfDoesNotFoldAcrossABranch)
{
	const Translation &t = translate ("tokens", "Tokens:TypeOfAcrossABranch");

	ASSERT_NE (t.function, nullptr) << t.error;
	EXPECT_EQ (t.count ("@\"mono_typeof_Holder"), 0u) << t.text ();
	EXPECT_EQ (t.count ("@\"mono_class_Holder"), 1u) << t.text ();
	EXPECT_EQ (t.count ("GetTypeFromHandle"), 1u) << t.text ();
}

// object.GetType () is the System.Type on the receiver's vtable, so the site is a
// null check and two reads rather than a call into an icall wrapper.
TEST_F (TranslatorTest, GetTypeReadsTheVtable)
{
	const Translation &t = translate ("tokens", "Tokens:TypeOfObject");

	ASSERT_NE (t.function, nullptr) << t.error;
	EXPECT_EQ (t.count ("object:GetType"), 0u) << t.text ();
	EXPECT_EQ (t.count ("%obj_type = call ptr @mono.vtable.type"), 1u) << t.text ();

	// The read off the object is a load, because an allocation's own store
	// writes that word. `!invariant.group` is `mark_object_vtable_read ()`'s
	// tag for that load.
	EXPECT_EQ (t.count ("!invariant.group"), 1u) << t.text ();
	EXPECT_GT (t.count ("NullReferenceException"), 0u) << t.text ();
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

// The cache memoizes the last vtable that answered, so every cast site needs a
// cache slot of its very own - two sites sharing one would fight over it.
TEST_F (TranslatorTest, EachCastSiteGetsItsOwnCacheSlot)
{
	const Translation &t = translate ("casts", "Casts:TwoCasts");

	ASSERT_NE (t.function, nullptr) << t.error;

	// Count the slots rather than the mentions of one. Each site reads its
	// slot and then hands the same slot to the helper, so the number of
	// mentions says nothing about how many slots there are.
	unsigned slots = 0;

	for (const llvm::GlobalVariable &global : t.module->globals ()) {
		if (!global.getName ().starts_with ("cast_cache"))
			continue;

		EXPECT_TRUE (global.hasInternalLinkage ()) << global.getName ().str ();
		++slots;
	}

	EXPECT_EQ (slots, 2u) << t.text ();
}

/*
 * The translator writes one call and no test at all. What decides the answer -
 * the class, the site's cache and the wrapper behind it - are its operands, and
 * lower_type_tests () writes the probe once nothing has folded the site.
 * cast-func-tests.cpp covers what it writes.
 */
TEST_F (TranslatorTest, ACastIsOneCallCarryingWhatDecidesIt)
{
	const Translation &t = translate ("casts", "Casts:CastString");

	ASSERT_NE (t.function, nullptr) << t.error;

	EXPECT_EQ (t.count ("call ptr @mono.cast.castclass"), 1u) << t.text ();
	EXPECT_EQ (t.count ("@\"mono_class_string"), 1u) << t.text ();
	EXPECT_EQ (t.count ("ptr @cast_cache"), 1u) << t.text ();
	EXPECT_EQ (t.count ("mono_object_castclass_with_cache"), 1u) << t.text ();

	// None of the probe. A site the translator wrote a test in front of is a
	// site no pass can answer.
	EXPECT_EQ (t.count ("obj_idepth"), 0u) << t.text ();
	EXPECT_EQ (t.count ("cached_vtable"), 0u) << t.text ();
	EXPECT_EQ (t.count ("icmp eq ptr"), 0u) << t.text ();
}

// The two forms answer a failed test differently, so each has a declaration of
// its own and the lowering reads which one it is off the name.
TEST_F (TranslatorTest, IsinstAndCastclassAreDifferentDeclarations)
{
	const Translation &cast = translate ("casts", "Casts:CastString");
	const Translation &test = translate ("casts", "Casts:IsString");

	ASSERT_NE (cast.function, nullptr) << cast.error;
	ASSERT_NE (test.function, nullptr) << test.error;

	EXPECT_EQ (cast.count ("call ptr @mono.cast.castclass"), 1u) << cast.text ();
	EXPECT_EQ (cast.count ("mono.cast.isinst"), 0u) << cast.text ();
	EXPECT_EQ (test.count ("call ptr @mono.cast.isinst"), 1u) << test.text ();
	EXPECT_EQ (test.count ("mono.cast.castclass"), 0u) << test.text ();
}

/*
 * An interface target reaches the same declaration. Which test stands in front
 * of the wrapper is the lowering's to pick, and the site says nothing about it.
 */
TEST_F (TranslatorTest, ACastToAnInterfaceKeepsTheHelper)
{
	const Translation &t = translate ("casts", "Casts:CastIface");

	ASSERT_NE (t.function, nullptr) << t.error;
	EXPECT_EQ (t.count ("obj_idepth"), 0u) << t.text ();
	EXPECT_EQ (t.count ("mono_object_castclass_with_cache"), 1u) << t.text ();
}

/*
 * A reference parameter's declared class is recorded on the function, which is
 * what lets a fold answer a test on an argument once inlining has brought the
 * two together. `mark_parameter_classes ()` has the format.
 */
TEST_F (TranslatorTest, AMethodRecordsItsReferenceParameterClasses)
{
	const Translation &t = translate ("casts", "Casts:CastString");

	ASSERT_NE (t.function, nullptr) << t.error;

	const llvm::MDNode *listed = t.function->getMetadata (mono::param_classes_md);

	ASSERT_NE (listed, nullptr) << t.text ();
	// The one argument, which is an object reference.
	EXPECT_EQ (listed->getNumOperands (), 1u) << t.text ();
}

TEST_F (TranslatorTest, UnboxAnyOnAReferenceTypeIsACast)
{
	const Translation &t = translate ("casts", "Casts:UnboxAnyString");

	ASSERT_NE (t.function, nullptr) << t.error;
	EXPECT_EQ (t.count ("mono_object_castclass_with_cache"), 1u);
	EXPECT_EQ (t.count ("ves_icall_object_new_specific"), 0u);
}

/* ---------------------------------------------------------- buffer copies */

/*
 * A behavioural test cannot see this rewrite, because the icall it replaces
 * copies the same bytes. So the cases below read the IR.
 *
 * They translate the class library's own methods rather than an il/ fixture,
 * because the two copies are internal to corlib and the translator refuses a
 * call a fixture's assembly cannot make. That ties each case to the body corlib
 * has today, which is the body the rewrite is written against.
 */

// System.Buffer refuses a null and a count below one, then hands the rest to
// the icall. What is left of each body is the intrinsic and those guards.
TEST_F (TranslatorTest, ARawCopyBecomesAMemoryIntrinsic)
{
	// The nuint overloads, which are the ones that reach the icall. The int and
	// uint forwarders beside them only widen the count.
	const Translation &copy =
		translate ("mscorlib", "System.Buffer:Memcpy(byte*,byte*,ulong)");
	const Translation &move =
		translate ("mscorlib", "System.Buffer:Memmove(byte*,byte*,ulong)");

	ASSERT_NE (copy.function, nullptr) << copy.error;
	EXPECT_EQ (copy.count ("llvm.memcpy"), 1u) << copy.text ();
	EXPECT_EQ (copy.count ("llvm.memmove"), 0u) << copy.text ();
	EXPECT_EQ (copy.count ("RuntimeImports:Memcpy"), 0u) << copy.text ();
	EXPECT_EQ (copy.count ("ptr align 1"), 2u) << copy.text ();

	ASSERT_NE (move.function, nullptr) << move.error;
	EXPECT_EQ (move.count ("llvm.memmove"), 1u) << move.text ();
	EXPECT_EQ (move.count ("llvm.memcpy"), 0u) << move.text ();
	EXPECT_EQ (move.count ("RuntimeImports:Memmove"), 0u) << move.text ();
}

/*
 * A caller with a literal count does not reach the intrinsic here. Marshal
 * reads a value the caller did not align through a copy of its own width, and
 * the two System.Buffer forwarders stand between that site and the icall. The
 * shape test refuses the first of them, which branches on its count.
 *
 * This is the translator's output, before any pipeline, which is what the
 * fixture hands back. Tier 2 does fold both forwarders: it takes the whole
 * chain down to one unaligned `load i32`, with the null test the copy carries
 * left as an implicit check. So read this as where the intrinsic has not
 * arrived yet rather than as a limit on the compiled code.
 */
TEST_F (TranslatorTest, AConstantCountStillReachesTheForwarder)
{
	const Translation &t =
		translate ("mscorlib", "System.Runtime.InteropServices.Marshal:ReadInt32(intptr)");

	ASSERT_NE (t.function, nullptr) << t.error;
	EXPECT_EQ (t.count ("llvm.memcpy"), 0u) << t.text ();
	EXPECT_GE (t.count ("Buffer:Memcpy"), 1u) << t.text ();
}

/*
 * Memmove_wbarrier sits beside the two this matches, under the same class and
 * with a name the first eight characters of which agree. It counts elements
 * rather than bytes and marks cards, so an intrinsic in its place would drop
 * the barrier and leave the collector a reference it never scans.
 */
TEST_F (TranslatorTest, TheWriteBarrierCopyIsNotMatched)
{
	MONO_SKIP_WITHOUT_CORPUS ();

	auto lookup = [] (const char *name) -> MonoMethod * {
		MonoMethodDesc *desc = mono_method_desc_new (name, TRUE);
		MonoMethod *method =
			mono_method_desc_search_in_image (desc, mono_get_corlib ());

		mono_method_desc_free (desc);
		return method;
	};

	MonoMethod *barrier =
		lookup ("System.Runtime.RuntimeImports:Memmove_wbarrier");
	MonoMethod *plain = lookup ("System.Runtime.RuntimeImports:Memmove");

	ASSERT_NE (barrier, nullptr);
	ASSERT_NE (plain, nullptr);

	EXPECT_FALSE (mono::buffer_copy_for (
			      barrier, mono_method_signature_internal (barrier))
			      .has_value ());

	std::optional<mono::BufferCopy> copy = mono::buffer_copy_for (
		plain, mono_method_signature_internal (plain));

	ASSERT_TRUE (copy.has_value ());
	EXPECT_TRUE (copy->may_overlap);
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
	{"Refused:StackUnderflow", "stack"},
	{"Refused:BadLocalIndex", "local"},
	{"Refused:FallsOffTheEnd", "return"},
	{"Refused:ConstrainedPlainCall", "plain call"},
	{"Refused:UsesJmpBadly", "signature"},
	{"Refused:ArglistOutsideAVararg", "vararg"},
	{"Refused:JumpsToAVararg", "vararg"},
	{"Refused:MergesAStructWithAnInt", "different type"},
};

/* A filter clause translates: the body becomes a function of its own beside
 * the method, reaching the frame's locals through llvm.localrecover. */
TEST_F (TranslatorTest, AFilterClauseBecomesItsOwnFunction)
{
	const Translation &t = translate ("refused", "Refused:HasAFilterClause");

	ASSERT_NE (t.function, nullptr) << t.error;

	llvm::Function *body = nullptr;
	for (llvm::Function &f : *t.module)
		if (f.getName ().contains ("$filter"))
			body = &f;
	ASSERT_NE (body, nullptr) << t.text ();
	EXPECT_FALSE (body->isDeclaration ());
}

INSTANTIATE_TEST_SUITE_P (Corpus, Refuses, testing::ValuesIn (refusals),
                          [] (const testing::TestParamInfo<RefusalRef> &info) {
				  std::string name = info.param.method;

				  for (char &c : name)
					  if (!isalnum (static_cast<unsigned char> (c)))
						  c = '_';
				  return name;
			  });

} // namespace

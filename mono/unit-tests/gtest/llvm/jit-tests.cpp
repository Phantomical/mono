/*
 * Tests for MonoJit, the ORCv2 execution engine.
 *
 * Two layers: pure-LLVM tests that hand-build IR and never translate a method,
 * and end-to-end tests that translate methods from the il/ corpus with
 * method_to_llvm (), compile them through the jit and execute the result. The
 * end-to-end ones are the point: translator output actually runs. Both boot a
 * runtime, since code memory comes out of mono's code manager.
 */

#include "harness.hpp"

#include "jit.hpp"
#include "passes/builtins.hpp"
#include "passes/lower-builtins.hpp"
#include "passes/tier-counter.hpp"

#include <llvm/ADT/SmallPtrSet.h>
#include <llvm/Analysis/LoopInfo.h>
#include <llvm/IR/Dominators.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/PassManager.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/Support/CommandLine.h>

#include <sys/mman.h>

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <utility>

using namespace llvm;
using namespace llvm::orc;

namespace mono {
namespace test {
namespace {

/// A context/module pair ready to hand to MonoJit::compile.
struct OwnedModule {
	std::unique_ptr<LLVMContext> context;
	std::unique_ptr<Module> module;

	ThreadSafeModule take ()
	{
		return ThreadSafeModule (std::move (module),
		                         ThreadSafeContext (std::move (context)));
	}
};

/// i32 add(i32 a, i32 b) { return a + b; }
static OwnedModule
build_add_module ()
{
	OwnedModule m;
	m.context = std::make_unique<LLVMContext> ();
	m.module = std::make_unique<Module> ("jit.add", *m.context);

	Type *i32 = Type::getInt32Ty (*m.context);
	Function *fn = Function::Create (FunctionType::get (i32, { i32, i32 }, false),
	                                 Function::ExternalLinkage, "add", m.module.get ());
	IRBuilder<> b (BasicBlock::Create (*m.context, "entry", fn));
	b.CreateRet (b.CreateAdd (fn->getArg (0), fn->getArg (1)));

	EXPECT_FALSE (verifyFunction (*fn, &errs ()));
	return m;
}

/// i64 entry(i64 x) { return helper(x) + 1; } with helper external.
static OwnedModule
build_helper_call_module (const char *helper_name)
{
	OwnedModule m;
	m.context = std::make_unique<LLVMContext> ();
	m.module = std::make_unique<Module> ("jit.helper", *m.context);

	Type *i64 = Type::getInt64Ty (*m.context);
	FunctionType *fty = FunctionType::get (i64, { i64 }, false);
	FunctionCallee helper = m.module->getOrInsertFunction (helper_name, fty);
	Function *fn = Function::Create (fty, Function::ExternalLinkage, "entry",
	                                 m.module.get ());
	IRBuilder<> b (BasicBlock::Create (*m.context, "entry", fn));
	Value *call = b.CreateCall (helper, { fn->getArg (0) });
	b.CreateRet (b.CreateAdd (call, ConstantInt::get (i64, 1)));

	EXPECT_FALSE (verifyFunction (*fn, &errs ()));
	return m;
}

extern "C" int64_t
mono_jit_test_double_it (int64_t x)
{
	return x * 2;
}

/*
 * These hand-build their IR and never translate a method, but code memory comes
 * out of mono's code manager, so they still need a runtime under them.
 */
class Jit : public ::testing::Test {
public:
	static void SetUpTestSuite ()
	{
		MONO_SKIP_WITHOUT_CORPUS ();
		init_runtime ();
	}
};

TEST_F (Jit, CompilesAndRunsHandBuiltIr)
{
	auto jit = test::make_jit ();
	ASSERT_TRUE (bool (jit)) << toString (jit.takeError ());

	auto entry = (*jit)->compile (build_add_module ().take (), "add");
	ASSERT_TRUE (bool (entry)) << toString (entry.takeError ());

	auto add = reinterpret_cast<int32_t (*) (int32_t, int32_t)> (entry->entry);
	EXPECT_EQ (add (2, 40), 42);
	EXPECT_EQ (add (-7, 3), -4);
}

TEST_F (Jit, ResolvesRegisteredHelpers)
{
	auto jit = test::make_jit ();
	ASSERT_TRUE (bool (jit)) << toString (jit.takeError ());

	ASSERT_FALSE (bool ((*jit)->register_symbol (
		"mono_jit_test_double_it", (void *) &mono_jit_test_double_it)));
	/* Registering the same name again is a no-op, not an error. */
	ASSERT_FALSE (bool ((*jit)->register_symbol (
		"mono_jit_test_double_it", (void *) &mono_jit_test_double_it)));

	auto entry = (*jit)->compile (
		build_helper_call_module ("mono_jit_test_double_it").take (), "entry");
	ASSERT_TRUE (bool (entry)) << toString (entry.takeError ());

	auto fn = reinterpret_cast<int64_t (*) (int64_t)> (entry->entry);
	EXPECT_EQ (fn (20), 41);
}

TEST_F (Jit, UnregisteredHelperFailsTheCompile)
{
	auto jit = test::make_jit ();
	ASSERT_TRUE (bool (jit)) << toString (jit.takeError ());

	/*
	 * The missing symbol's name is reported through the session's error
	 * reporter ("Symbols not found: ..." on stderr); the Error handed back
	 * from the lookup is the generic materialization failure. What this test
	 * pins down is that the compile FAILS - nothing silently binds.
	 */
	auto entry = (*jit)->compile (
		build_helper_call_module ("mono_jit_test_never_registered").take (),
		"entry");
	ASSERT_FALSE (bool (entry));
	consumeError (entry.takeError ());
}

/*
 * Removal takes a freed method's names and side tables out. It does not take the
 * memory back: a code manager frees only whole, so a retired body keeps its bytes
 * until the arena goes. What has to hold is that the removal succeeds, that the
 * JIT is still usable after one, and that a later compile is given somewhere
 * else - the same address would mean the bytes were handed out while something
 * could still be reading them.
 */
TEST_F (Jit, RemovalLeavesTheJitUsable)
{
	auto jit = test::make_jit ();
	ASSERT_TRUE (bool (jit)) << toString (jit.takeError ());

	auto first = (*jit)->compile (build_add_module ().take (), "add");
	ASSERT_TRUE (bool (first)) << toString (first.takeError ());
	ASSERT_NE (first->dylib, nullptr);

	const uint8_t *was = first->code;
	JITDylib *dylib = first->dylib;

	ASSERT_FALSE (bool ((*jit)->remove_dylibs ({ dylib })));

	auto second = (*jit)->compile (build_add_module ().take (), "add");
	ASSERT_TRUE (bool (second)) << toString (second.takeError ());
	EXPECT_NE (second->code, was);

	auto add = reinterpret_cast<int32_t (*) (int32_t, int32_t)> (second->entry);
	EXPECT_EQ (add (2, 40), 42);
}

/*
 * The end-to-end fixture: runtime booted, methods translated from the corpus,
 * compiled through one shared jit, executed.
 */
class JitExecution : public ::testing::Test {
public:
	static void SetUpTestSuite ()
	{
		MONO_SKIP_WITHOUT_CORPUS ();
		init_runtime ();
		jit_holder () = cantFail (test::make_jit ());
	}

	static void TearDownTestSuite () { jit_holder ().reset (); }

protected:
	static std::unique_ptr<MonoJit> &jit_holder ()
	{
		static std::unique_ptr<MonoJit> jit;
		return jit;
	}

	/// Translate IMAGE's METHOD and compile it, failing the test on a refusal,
	/// invalid IR, or a compile error.
	void *compile (const std::string &image, const std::string &method)
	{
		std::unique_ptr<Translation> t = translate_method (image, method);

		EXPECT_NE (t->function, nullptr) << t->error;
		if (t->function == nullptr)
			return nullptr;
		EXPECT_EQ (t->verifier_error, "") << t->text ();

		std::string entry = t->function->getName ().str ();
		auto addr = jit_holder ()->compile (
			ThreadSafeModule (std::move (t->module),
		                      ThreadSafeContext (std::move (t->context))),
			entry);
		EXPECT_TRUE (bool (addr)) << toString (addr.takeError ());
		return addr ? addr->entry : nullptr;
	}
};

TEST_F (JitExecution, TranslatedIntArithmeticRuns)
{
	auto add = reinterpret_cast<int32_t (*) (int32_t, int32_t)> (
		compile ("arith", "Arith:Add"));
	ASSERT_NE (add, nullptr);
	EXPECT_EQ (add (2, 40), 42);
	EXPECT_EQ (add (INT32_MAX, 1), INT32_MIN);

	auto mix = reinterpret_cast<int64_t (*) (int64_t, int64_t)> (
		compile ("arith", "Arith:MixInt64"));
	ASSERT_NE (mix, nullptr);
	/* a * b - a */
	EXPECT_EQ (mix (7, 6), 35);
	EXPECT_EQ (mix (-3, 5), -12);
}

TEST_F (JitExecution, TranslatedFloatArithmeticRuns)
{
	auto mix = reinterpret_cast<double (*) (double, double)> (
		compile ("arith", "Arith:MixFloat"));
	ASSERT_NE (mix, nullptr);
	/* a / b + b */
	EXPECT_DOUBLE_EQ (mix (1.0, 2.0), 2.5);
}

TEST_F (JitExecution, Tier0PipelinePromotesAllocasToSsa)
{
	std::unique_ptr<Translation> t = translate_method ("arith", "Arith:Add");
	ASSERT_NE (t->function, nullptr) << t->error;

	/*
	 * The translator routes arguments through allocas and counts on mem2reg;
	 * the tier-0 pipeline is what provides it. Before: allocas present.
	 * After: none, and the add still there.
	 */
	EXPECT_GT (t->count ("alloca"), 0u);

	MonoJit::run_tier1_pipeline (*t->module);

	EXPECT_EQ (t->count ("alloca"), 0u);
	EXPECT_GT (t->count ("add"), 0u);
}

/*
 * LLVM's own print options reach the IR pipelines through the
 * StandardInstrumentations each tier registers, and a lost registration is
 * invisible from outside: the option parses, the pipeline runs and nothing
 * prints, which a reader takes for an answer.
 *
 * A pipeline is built once for each thread, and it registers what the options
 * said at the moment it was built. So the run goes on a thread of its own
 * rather than on the one gtest ran the earlier tests on, and that thread builds
 * its pipeline after the option is set.
 */
TEST_F (JitExecution, APrintOptionReachesTheTier1Pipeline)
{
	std::unique_ptr<Translation> t = translate_method ("arith", "Arith:Add");
	ASSERT_NE (t->function, nullptr) << t->error;

	cl::Option *option = cl::getRegisteredOptions ().lookup ("print-after-all");
	ASSERT_NE (option, nullptr);

	auto *print_after_all = static_cast<cl::opt<bool> *> (option);
	bool was_set = print_after_all->getValue ();

	print_after_all->setValue (true);
	testing::internal::CaptureStderr ();

	std::thread ([&t] { MonoJit::run_tier1_pipeline (*t->module); }).join ();

	std::string printed = testing::internal::GetCapturedStderr ();
	print_after_all->setValue (was_set);

	EXPECT_NE (printed.find ("IR Dump After"), std::string::npos);
	EXPECT_NE (printed.find ("InstCombinePass"), std::string::npos);
}

// The call shape belongs to the pass. What the translator emitted names no this at
// all. What survives the lowering is the constructor's wrapper - the method the
// runtime publishes for a string constructor - called with the null this it never
// reads, and nothing of the builtin left over.
TEST_F (JitExecution, LowerBuiltinsGivesTheStringConstructorItsNullThis)
{
	std::unique_ptr<Translation> t = translate_method ("objects", "Objects:MakeString");
	ASSERT_NE (t->function, nullptr) << t->error;
	ASSERT_EQ (t->count ("mono.builtin.string_constructor."), 1u) << t->text ();

	PassBuilder pb;
	ModuleAnalysisManager mam;
	ModulePassManager mpm;

	pb.registerModuleAnalyses (mam);
	mpm.addPass (MonoBuiltinLower (LowerStage::pre_simplification));
	mpm.run (*t->module, mam);

	EXPECT_EQ (t->count ("mono.builtin."), 0u) << t->text ();
	EXPECT_EQ (t->count ("call ptr @\"(wrapper managed-to-managed) string:.ctor"), 1u)
		<< t->text ();
	EXPECT_EQ (t->count ("(ptr null"), 1u) << t->text ();
	EXPECT_EQ (verify_function (*t->function), "") << t->text ();
}

// A call only becomes a jump if a ret follows it in the same block, and the
// pipeline's own SimplifyCFG takes that away: a method with a base case has two
// returns, which get merged into one block reached by a branch. musttail is
// protected from that by a verifier rule and a plain tail call is not, so
// without the repair the marker survives the pipeline meaning nothing - no
// diagnostic, and a recursion that should run in constant space overflows.
TEST_F (JitExecution, TheTier0PipelineLeavesATailCallInTailPosition)
{
	std::unique_ptr<Translation> t = translate_method ("calls", "Calls:TailMerged");
	ASSERT_NE (t->function, nullptr) << t->error;
	ASSERT_EQ (t->count ("tail call"), 1u) << t->text ();

	MonoJit::run_tier1_pipeline (*t->module);

	const CallInst *jump = nullptr;

	for (const BasicBlock &block : *t->function) {
		for (const Instruction &instruction : block) {
			auto *call = dyn_cast<CallInst> (&instruction);

			if (call != nullptr && call->getTailCallKind () == CallInst::TCK_Tail)
				jump = call;
		}
	}

	ASSERT_NE (jump, nullptr) << t->text ();
	EXPECT_TRUE (isa<ReturnInst> (jump->getNextNode ())) << t->text ();
	EXPECT_EQ (verify_function (*t->function), "") << t->text ();
}

// Every tail call gets its ret back, not just the first. Restoring one withdraws
// a predecessor from the merged block, which collapses the phi standing in for
// the returned value - so the ones that follow arrive at a block that no longer
// looks the way the merge left it.
//
// How many are left to check is the optimizer's business rather than this
// test's. Two sites calling one method are sunk into a single call fed by a
// select, which is still a jump and still runs in constant space.
TEST_F (JitExecution, EveryMergedTailCallGetsItsReturnBack)
{
	std::unique_ptr<Translation> t = translate_method ("calls", "Calls:TailTwoWays");
	ASSERT_NE (t->function, nullptr) << t->error;
	ASSERT_EQ (t->count ("tail call"), 2u) << t->text ();

	MonoJit::run_tier1_pipeline (*t->module);

	unsigned jumps = 0;

	for (const BasicBlock &block : *t->function) {
		for (const Instruction &instruction : block) {
			auto *call = dyn_cast<CallInst> (&instruction);

			if (call == nullptr || call->getTailCallKind () != CallInst::TCK_Tail)
				continue;

			++jumps;
			EXPECT_TRUE (isa<ReturnInst> (call->getNextNode ())) << t->text ();
		}
	}

	EXPECT_GT (jumps, 0u) << t->text ();
	EXPECT_EQ (verify_function (*t->function), "") << t->text ();
}

/*
 * Slabs are placed wherever mmap puts them, so a helper can end up further from
 * the code calling it than a direct call reaches. What saves that is the
 * combination of code model Small with Reloc::PIC_ and every callee being
 * external: the callee is not dso_local, so lowering emits R_X86_64_PLT32, and
 * JITLink turns a BranchPCRel32 whose target is undefined in the graph into a
 * jump stub - collapsing it back to a direct branch only when the displacement
 * actually fits.
 *
 * Nothing in the runtime would fail loudly if that stopped holding; the call
 * would simply be built with a truncated displacement and land somewhere else.
 * So put a helper far enough away that the direct form cannot encode, and call
 * it.
 */
TEST_F (Jit, CallsAHelperFurtherAwayThanRel32Reaches)
{
	/* mov rax, rdi; add rax, rax; ret - mono_jit_test_double_it by hand,
	 * because what has to move is the callee's address. */
	static const uint8_t body[] = { 0x48, 0x89, 0xf8, 0x48, 0x01, 0xc0, 0xc3 };

	size_t page = 4096;
	void *far = nullptr;

	/* Somewhere no ordinary mapping lands, and > 4GB from both the test binary
	 * and any slab, so a rel32 from either cannot encode it. */
	for (uintptr_t at = 0x200000000000ULL; at < 0x400000000000ULL; at += 0x10000000000ULL) {
		void *got = mmap (reinterpret_cast<void *> (at), page,
		                  PROT_READ | PROT_WRITE | PROT_EXEC,
		                  MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		if (got != MAP_FAILED) {
			far = got;
			break;
		}
	}
	ASSERT_NE (far, nullptr) << "could not place a far helper";
	memcpy (far, body, sizeof (body));

	auto jit = test::make_jit ();
	ASSERT_TRUE (bool (jit)) << toString (jit.takeError ());

	ASSERT_FALSE (bool ((*jit)->register_symbol ("mono_jit_test_far_helper", far)));

	auto entry = (*jit)->compile (
		build_helper_call_module ("mono_jit_test_far_helper").take (), "entry");
	ASSERT_TRUE (bool (entry)) << toString (entry.takeError ());

	int64_t distance = std::abs (static_cast<int64_t> (
		reinterpret_cast<intptr_t> (far)
		- reinterpret_cast<intptr_t> (entry->entry)));
	EXPECT_GT (distance, int64_t (1) << 32)
		<< "the helper is close enough for a direct call, so this proves nothing";

	auto fn = reinterpret_cast<int64_t (*) (int64_t)> (entry->entry);
	EXPECT_EQ (fn (20), 41);

	munmap (far, page);
}

/*
 * The counter cases, which read a body that must stay at tier 1 while they do.
 * jit.cpp reads the threshold once and keeps the answer, so a case cannot pin it
 * for itself. The build registers this suite as a run of its own under a count
 * nothing reaches, and the check below is what keeps a run without it honest.
 */
class JitProfile : public ::testing::Test {
public:
	static void SetUpTestSuite ()
	{
		MONO_SKIP_WITHOUT_CORPUS ();

		if (::getenv ("MONO_LLVM_JIT_TIER2_THRESHOLD") == nullptr)
			GTEST_SKIP () << "MONO_LLVM_JIT_TIER2_THRESHOLD is unset, so a "
			                 "body under test can promote out from under it";

		const char *tier2 = ::getenv ("MONO_LLVM_JIT_TIER2");

		if (tier2 != nullptr && (*tier2 == '\0' || StringRef (tier2) == "0"
		                         || StringRef (tier2).equals_insensitive ("false")))
			GTEST_SKIP () << "MONO_LLVM_JIT_TIER2 turns tier 2 off, so the "
			                 "pipeline instruments nothing";

		init_runtime ();
	}
};

/// A branchy i32 -> i32 function, marked as one the instrumentation should
/// count. \p arms decides how many branches it has, and so how many counters.
static Function *
build_counted_function (Module &m, LLVMContext &ctx, StringRef name, unsigned arms)
{
	Type *i32 = Type::getInt32Ty (ctx);
	Function *fn = Function::Create (FunctionType::get (i32, { i32 }, false),
	                                 Function::ExternalLinkage, name, &m);

	// ProfileSelectPass instruments whatever carries this. TierCounterPass wants
	// a handle as well and leaves the body alone without one, which keeps this
	// module free of symbols only the runtime could resolve.
	fn->addFnAttr (tier_counter_attribute, "10");

	BasicBlock *entry = BasicBlock::Create (ctx, "entry", fn);
	BasicBlock *exit = BasicBlock::Create (ctx, "exit", fn);
	IRBuilder<> b (entry);
	Value *acc = fn->getArg (0);

	/*
	 * Instrumentation runs behind the simplification, so an arm has to be one
	 * simplification keeps. A volatile store is the cheapest thing SimplifyCFG
	 * will neither speculate into the block above nor fold into a select, so
	 * the arm stays a block and the block gets a counter of its own.
	 */
	AllocaInst *slot = b.CreateAlloca (i32, nullptr, "slot");

	b.CreateStore (b.getInt32 (0), slot);

	for (unsigned i = 0; i < arms; i++) {
		BasicBlock *taken = BasicBlock::Create (ctx, "taken" + Twine (i), fn);
		BasicBlock *next = BasicBlock::Create (ctx, "next" + Twine (i), fn);

		b.CreateCondBr (b.CreateICmpSGT (acc, b.getInt32 (int32_t (i))), taken,
		                next);
		b.SetInsertPoint (taken);
		b.CreateStore (b.getInt32 (int32_t (i)), slot, /*isVolatile=*/true);
		b.CreateBr (next);
		b.SetInsertPoint (next);
	}

	b.CreateBr (exit);
	b.SetInsertPoint (exit);
	b.CreateRet (acc);

	EXPECT_FALSE (verifyFunction (*fn, &errs ()));
	return fn;
}

/*
 * Two instrumented methods in one batched compile. Each has to come back with
 * its own counter array: they share one `__llvm_prf_cnts`, so anything that
 * reads the section as a single method's array either returns the other method's
 * numbers or fails.
 */
TEST_F (JitProfile, EachInstrumentedFunctionGetsItsOwnCounters)
{
	auto jit = test::make_jit ();
	ASSERT_TRUE (bool (jit)) << toString (jit.takeError ());

	OwnedModule m;
	m.context = std::make_unique<LLVMContext> ();
	m.module = std::make_unique<Module> ("jit.two-counted", *m.context);

	build_counted_function (*m.module, *m.context, "counted_a", 1);
	build_counted_function (*m.module, *m.context, "counted_b", 3);

	std::vector<ProfileCounters> layout =
		MonoJit::optimize (*m.module, JitTier::tier1);
	ASSERT_EQ (layout.size (), 2u);

	StringRef names[] = { "counted_a", "counted_b" };
	auto compiled = (*jit)->compile_batch (m.take (), names, {}, layout);
	ASSERT_TRUE (bool (compiled)) << toString (compiled.takeError ());
	ASSERT_EQ (compiled->size (), 2u);

	const CompiledMethod &first = compiled->front ();
	const CompiledMethod &second = compiled->back ();

	// Each result is its own method's, so what the other one counts is not in it.
	ASSERT_TRUE (first.profile.has_value ());
	ASSERT_TRUE (second.profile.has_value ());
	EXPECT_EQ (first.profile->function, "counted_a");
	EXPECT_EQ (second.profile->function, "counted_b");
	EXPECT_TRUE (first.other_profiles.empty ());
	EXPECT_TRUE (second.other_profiles.empty ());
	ASSERT_EQ (first.functions.size (), 1u);
	EXPECT_EQ (first.functions.front ().first, "counted_a");
	ASSERT_EQ (second.functions.size (), 1u);
	EXPECT_EQ (second.functions.front ().first, "counted_b");

	const ProfileCounters &a = *first.profile;
	const ProfileCounters &b = *second.profile;

	ASSERT_NE (a.counters, nullptr);
	ASSERT_NE (b.counters, nullptr);
	ASSERT_GT (a.count, 0u);
	ASSERT_GT (b.count, 0u);

	// Different CFGs, so the arrays cannot be the same size, and they must not
	// overlap - one array covering both is exactly the bug this guards.
	EXPECT_NE (a.count, b.count);
	EXPECT_TRUE (a.counters + a.count <= b.counters
	             || b.counters + b.count <= a.counters);

	// The counts themselves: run each a different number of times and read the
	// entry block's counter back out of live code memory.
	auto fn_a = reinterpret_cast<int32_t (*) (int32_t)> (first.entry);
	auto fn_b = reinterpret_cast<int32_t (*) (int32_t)> (second.entry);

	for (int i = 0; i < 7; i++)
		fn_a (i);
	for (int i = 0; i < 3; i++)
		fn_b (i);

	EXPECT_EQ (a.counters[0], 7u);
	EXPECT_EQ (b.counters[0], 3u);
}

/*
 * Where TierCounterPass puts the counter in a body that has a loop. The pass adds
 * the turns up in a register and takes that total off at each exit, so two things
 * about the emission matter as much as the arithmetic: the accumulator reaches
 * the counter through registers rather than a stack slot, and one add covers a
 * loop rather than one add covering a block. Tier-1 codegen is FastISel with the
 * fast register allocator, and no pass behind this one tidies up either mistake.
 *
 * The blocks no loop holds are charged at the entry instead, in this body and in
 * a body with no loop at all. That charge is one load and one branch, and every
 * managed test in the tree exercises it.
 *
 * These cases set the entry weight to zero, so a count below is the work alone.
 */

/// The declaration a test function throws through.
FunctionCallee
thrower_decl (Module &m)
{
	LLVMContext &ctx = m.getContext ();
	FunctionCallee callee = m.getOrInsertFunction (
		"mono_llvm_throw_exception", Type::getVoidTy (ctx));

	if (auto *fn = dyn_cast<Function> (callee.getCallee ()))
		fn->setDoesNotReturn ();

	return callee;
}

/// Gives fn the attributes that ask for a counter, and defines the handle.
///
/// TierCounterPass looks the handle up by name and leaves the body alone when it
/// finds nothing, so the module has to hold one.
void
ask_for_a_counter (Module &m, Function *fn, StringRef threshold)
{
	std::string handle = (fn->getName () + ".handle").str ();

	// An entry weight of zero, because these cases are about where the write-backs
	// land and what they charge. A weight would add itself to each of them, which
	// says nothing about placement and moves every count below.
	fn->addFnAttr (tier_counter_attribute, threshold);
	fn->addFnAttr (tier_entry_weight_attribute, "0");
	fn->addFnAttr (tier_handle_attribute, handle);
	new GlobalVariable (m, Type::getInt32Ty (m.getContext ()), /*isConstant=*/true,
	                    GlobalValue::PrivateLinkage,
	                    ConstantInt::get (Type::getInt32Ty (m.getContext ()), 0), handle);
}

/// The write-backs in f, which are the atomic subtractions from its cost counter.
SmallVector<AtomicRMWInst *, 4>
write_backs (Function &f)
{
	GlobalVariable *counter = f.getParent ()->getNamedGlobal ("mono_tier_cost");
	SmallVector<AtomicRMWInst *, 4> found;

	if (counter == nullptr)
		return found;

	for (Instruction &i : instructions (f))
		if (auto *rmw = dyn_cast<AtomicRMWInst> (&i))
			if (rmw->getPointerOperand () == counter)
				found.push_back (rmw);

	return found;
}

/// What a walk back from a write-back's cost finds.
struct CostChain {
	/// The accumulation points, and the one add that folds in the constant for
	/// the blocks outside every loop.
	unsigned adds = 0;
	/// True when the accumulator still reaches the write-back from memory,
	/// which means PromoteMemToReg never ran over the stack slot.
	bool from_memory = false;
};

/// Walks back from a cost value through the arithmetic that built it.
void
walk_cost (Value *v, SmallPtrSetImpl<Value *> &seen, CostChain &chain)
{
	if (v == nullptr || isa<Constant> (v) || !seen.insert (v).second)
		return;

	if (auto *phi = dyn_cast<PHINode> (v)) {
		for (Value *in : phi->incoming_values ())
			walk_cost (in, seen, chain);

		return;
	}

	if (auto *op = dyn_cast<BinaryOperator> (v);
	    op != nullptr && op->getOpcode () == Instruction::Add) {
		chain.adds++;
		walk_cost (op->getOperand (0), seen, chain);
		walk_cost (op->getOperand (1), seen, chain);
		return;
	}

	if (isa<LoadInst> (v))
		chain.from_memory = true;
}

CostChain
cost_chain (AtomicRMWInst *write_back)
{
	SmallPtrSet<Value *, 16> seen;
	CostChain chain;

	walk_cost (write_back->getValOperand (), seen, chain);
	return chain;
}

/// The loops in f, counted the way TierCounterPass counts them.
unsigned
loop_count (Function &f)
{
	DominatorTree dt (f);
	LoopInfo li (dt);

	return li.getLoopsInPreorder ().size ();
}

/// i64 f(i32 n), one loop whose body has two arms, then a ret.
///
/// Each arm holds a volatile store of its own, to an address of its own.
/// SimplifyCFG will neither speculate such a store into the block above nor sink
/// two of them into one block, so the loop keeps four blocks. A placement that
/// went per block would then be plain in the count below.
Function *
build_looping_function (Module &m, LLVMContext &ctx, StringRef name, StringRef threshold)
{
	Type *i32 = Type::getInt32Ty (ctx);
	Type *i64 = Type::getInt64Ty (ctx);
	Function *fn = Function::Create (FunctionType::get (i64, { i32 }, false),
	                                 Function::ExternalLinkage, name, &m);

	ask_for_a_counter (m, fn, threshold);

	BasicBlock *entry = BasicBlock::Create (ctx, "entry", fn);
	BasicBlock *head = BasicBlock::Create (ctx, "head", fn);
	BasicBlock *odd = BasicBlock::Create (ctx, "odd", fn);
	BasicBlock *even = BasicBlock::Create (ctx, "even", fn);
	BasicBlock *latch = BasicBlock::Create (ctx, "latch", fn);
	BasicBlock *exit = BasicBlock::Create (ctx, "exit", fn);

	Value *n = fn->getArg (0);
	IRBuilder<> b (entry);
	AllocaInst *result = b.CreateAlloca (i64, nullptr, "result");
	AllocaInst *mark_odd = b.CreateAlloca (i64, nullptr, "mark_odd");
	AllocaInst *mark_even = b.CreateAlloca (i64, nullptr, "mark_even");

	b.CreateStore (ConstantInt::get (i64, 0), result);
	b.CreateCondBr (b.CreateICmpSGT (n, ConstantInt::get (i32, 0)), head, exit);

	b.SetInsertPoint (head);

	PHINode *i = b.CreatePHI (i32, 2, "i");
	Value *bit = b.CreateAnd (i, ConstantInt::get (i32, 1));

	b.CreateCondBr (b.CreateICmpNE (bit, ConstantInt::get (i32, 0)), odd, even);

	b.SetInsertPoint (odd);
	b.CreateStore (ConstantInt::get (i64, 1), mark_odd, /*isVolatile=*/true);
	b.CreateBr (latch);

	b.SetInsertPoint (even);
	b.CreateStore (ConstantInt::get (i64, 2), mark_even, /*isVolatile=*/true);
	b.CreateBr (latch);

	b.SetInsertPoint (latch);

	Value *next = b.CreateAdd (i, ConstantInt::get (i32, 1), "next");

	b.CreateStore (b.CreateSExt (next, i64), result, /*isVolatile=*/true);
	b.CreateCondBr (b.CreateICmpSLT (next, n), head, exit);

	i->addIncoming (ConstantInt::get (i32, 0), entry);
	i->addIncoming (next, latch);

	b.SetInsertPoint (exit);
	b.CreateRet (b.CreateLoad (i64, result, /*isVolatile=*/true));

	EXPECT_FALSE (verifyFunction (*fn, &errs ()));
	return fn;
}

TEST_F (JitProfile, TheCostCounterStaysInRegistersAndCountsPerLoop)
{
	OwnedModule m;
	m.context = std::make_unique<LLVMContext> ();
	m.module = std::make_unique<Module> ("jit.tier-cost", *m.context);

	Function *fn = build_looping_function (*m.module, *m.context, "looping", "1000");

	MonoJit::optimize (*m.module, JitTier::tier1);

	ASSERT_FALSE (verifyFunction (*fn, &errs ()));

	/*
	 * Two write-backs, and what each of them charges tells them apart. The entry
	 * charges the blocks no loop holds, which this pass knows as a number. The
	 * one ret charges what the loops added, which only the run knows.
	 */
	SmallVector<AtomicRMWInst *, 4> found = write_backs (*fn);
	ASSERT_EQ (found.size (), 2u);

	AtomicRMWInst *at_entry = nullptr;
	AtomicRMWInst *at_exit = nullptr;

	for (AtomicRMWInst *rmw : found) {
		if (isa<Constant> (rmw->getValOperand ()))
			at_entry = rmw;
		else
			at_exit = rmw;
	}

	// The entry is the one point a call always reaches. A callee's exception that
	// unwinds through this frame reaches no ret, so a body without this charge
	// spends nothing however often it is called.
	ASSERT_NE (at_entry, nullptr) << "no write-back charges a constant, so the "
	                                 "blocks outside every loop are charged at an "
	                                 "exit the exception can take away";
	ASSERT_NE (at_exit, nullptr);
	EXPECT_TRUE (fn->getEntryBlock ().getTerminator ()->getSuccessor (0)
	             == at_entry->getParent ())
		<< "the constant is charged somewhere the entry block does not branch to";

	CostChain chain = cost_chain (at_exit);

	EXPECT_FALSE (chain.from_memory)
		<< "the accumulator reaches the counter from a stack slot, so the "
		   "promotion to registers did not happen";

	// One add for each loop, and no more. More than that means the placement went
	// per block, which is what FastISel and the fast register allocator make
	// expensive.
	unsigned loops = loop_count (*fn);

	ASSERT_GT (loops, 0u) << "the simplification pipeline removed the loop, so "
	                         "this case no longer measures the placement";
	EXPECT_GE (chain.adds, 1u);
	EXPECT_LE (chain.adds, loops)
		<< "found " << chain.adds << " accumulation adds for " << loops
		<< " loops, so the pass is no longer placing one for each loop";
}

TEST_F (JitProfile, AnUnprotectedThrowWritesTheCountBack)
{
	OwnedModule m;
	m.context = std::make_unique<LLVMContext> ();
	m.module = std::make_unique<Module> ("jit.tier-throw", *m.context);

	LLVMContext &ctx = *m.context;
	Function *fn = Function::Create (
		FunctionType::get (Type::getVoidTy (ctx), {}, false),
		Function::ExternalLinkage, "only_throws", m.module.get ());

	ask_for_a_counter (*m.module, fn, "1000");

	IRBuilder<> b (BasicBlock::Create (ctx, "entry", fn));

	b.CreateCall (thrower_decl (*m.module));
	b.CreateUnreachable ();

	ASSERT_FALSE (verifyFunction (*fn, &errs ()));

	MonoJit::optimize (*m.module, JitTier::tier1);

	ASSERT_FALSE (verifyFunction (*fn, &errs ()));

	// Nothing in this body protects the site, so the exception leaves the frame
	// for good and the work it did has to reach the counter here.
	EXPECT_EQ (write_backs (*fn).size (), 1u);
}

TEST_F (JitProfile, AThrowThisBodyCatchesGetsNoWriteBackOfItsOwn)
{
	OwnedModule m;
	m.context = std::make_unique<LLVMContext> ();
	m.module = std::make_unique<Module> ("jit.tier-invoke", *m.context);

	LLVMContext &ctx = *m.context;
	Type *i32 = Type::getInt32Ty (ctx);
	Function *fn = Function::Create (FunctionType::get (i32, { i32 }, false),
	                                 Function::ExternalLinkage, "catches_itself",
	                                 m.module.get ());

	ask_for_a_counter (*m.module, fn, "1000");
	fn->setPersonalityFn (cast<Constant> (
		m.module
			->getOrInsertFunction ("mono_personality",
	                                       FunctionType::get (i32, {}, true))
			.getCallee ()));

	BasicBlock *entry = BasicBlock::Create (ctx, "entry", fn);
	BasicBlock *gone = BasicBlock::Create (ctx, "gone", fn);
	BasicBlock *pad = BasicBlock::Create (ctx, "pad", fn);
	BasicBlock *exit = BasicBlock::Create (ctx, "exit", fn);

	IRBuilder<> b (entry);
	AllocaInst *slot = b.CreateAlloca (i32, nullptr, "slot");

	b.CreateStore (ConstantInt::get (i32, 0), slot, /*isVolatile=*/true);
	b.CreateInvoke (thrower_decl (*m.module), gone, pad);

	b.SetInsertPoint (gone);
	b.CreateUnreachable ();

	b.SetInsertPoint (pad);

	LandingPadInst *caught =
		b.CreateLandingPad (StructType::get (PointerType::get (ctx, 0), i32), 0);

	caught->setCleanup (true);
	b.CreateStore (ConstantInt::get (i32, 1), slot, /*isVolatile=*/true);
	b.CreateBr (exit);

	b.SetInsertPoint (exit);
	b.CreateRet (b.CreateLoad (i32, slot, /*isVolatile=*/true));

	ASSERT_FALSE (verifyFunction (*fn, &errs ()));

	MonoJit::optimize (*m.module, JitTier::tier1);

	ASSERT_FALSE (verifyFunction (*fn, &errs ()));

	/*
	 * One write-back, at the ret. The invoke has not left the frame, because
	 * this body holds the pad it lands on, so a write-back in front of it would
	 * take the same work off the counter again when the ret is reached.
	 */
	bool invokes = false;

	for (Instruction &i : instructions (*fn))
		invokes |= isa<InvokeInst> (&i);

	ASSERT_TRUE (invokes) << "the simplification pipeline turned the invoke into "
	                         "a call, so this case no longer has the arm it is "
	                         "about";

	SmallVector<AtomicRMWInst *, 4> found = write_backs (*fn);

	ASSERT_EQ (found.size (), 1u);

	Instruction *terminator = found.front ()->getParent ()->getTerminator ();

	EXPECT_FALSE (isa<InvokeInst> (terminator));
}

/// A callee that comes back and can unwind. It carries neither nounwind nor
/// noreturn, so TierCounterPass sends it to the pad.
FunctionCallee
unwinding_callee_decl (Module &m)
{
	return m.getOrInsertFunction ("mono_test_may_unwind",
	                              Type::getVoidTy (m.getContext ()));
}

TEST_F (JitProfile, ALoopChargesItsTurnsWhenACalleeUnwindsThroughTheFrame)
{
	OwnedModule m;
	m.context = std::make_unique<LLVMContext> ();
	m.module = std::make_unique<Module> ("jit.tier-unwind", *m.context);

	LLVMContext &ctx = *m.context;
	Function *fn = build_looping_function (*m.module, ctx, "looping_and_calling", "1000");

	// Inside the loop, so an exception that leaves the frame from here takes the
	// turns made before it with it.
	BasicBlock *head = nullptr;

	for (BasicBlock &block : *fn)
		if (block.getName () == "head")
			head = &block;

	ASSERT_NE (head, nullptr);
	IRBuilder<> (head, head->getFirstInsertionPt ())
		.CreateCall (unwinding_callee_decl (*m.module));

	ASSERT_FALSE (verifyFunction (*fn, &errs ()));

	MonoJit::optimize (*m.module, JitTier::tier1);

	ASSERT_FALSE (verifyFunction (*fn, &errs ()));

	/*
	 * Three write-backs: the entry's constant, the ret, and the pad. The pad is
	 * the only one an exception that unwinds out of the frame reaches, and the
	 * turns are what it charges.
	 */
	SmallVector<AtomicRMWInst *, 4> found = write_backs (*fn);

	ASSERT_EQ (found.size (), 3u);

	AtomicRMWInst *at_pad = nullptr;

	for (AtomicRMWInst *rmw : found) {
		BasicBlock *landing = rmw->getParent ();

		// The check splits the block the pad started as, so the landing pad
		// instruction sits in a block the write-back's block is reached from.
		for (BasicBlock *pred : predecessors (landing))
			if (pred->isLandingPad ())
				landing = pred;
		if (landing->isLandingPad ())
			at_pad = rmw;
	}

	ASSERT_NE (at_pad, nullptr) << "no write-back is reached from a landing pad, "
	                               "so an exception that unwinds through this "
	                               "frame charges none of the turns";
	EXPECT_FALSE (isa<Constant> (at_pad->getValOperand ()))
		<< "the pad charges a number this pass knew, so it is charging the "
		   "constant rather than the turns the loop made";

	bool invokes = false;

	for (Instruction &i : instructions (*fn))
		invokes |= isa<InvokeInst> (&i);

	EXPECT_TRUE (invokes) << "the call never became an invoke, so nothing routes "
	                         "an unwind to the pad";
}
} // namespace
} // namespace test
} // namespace mono

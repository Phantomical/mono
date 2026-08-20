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
#include "passes/lower-builtins.hpp"
#include "passes/tier-counter.hpp"

#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/PassManager.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Passes/PassBuilder.h>

#include <sys/mman.h>

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
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

	MonoJit::run_tier0_pipeline (*t->module);

	EXPECT_EQ (t->count ("alloca"), 0u);
	EXPECT_GT (t->count ("add"), 0u);
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
	mpm.addPass (LowerBuiltinsPass ());
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

	MonoJit::run_tier0_pipeline (*t->module);

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
TEST_F (JitExecution, EveryMergedTailCallGetsItsReturnBack)
{
	std::unique_ptr<Translation> t = translate_method ("calls", "Calls:TailTwoWays");
	ASSERT_NE (t->function, nullptr) << t->error;
	ASSERT_EQ (t->count ("tail call"), 2u) << t->text ();

	MonoJit::run_tier0_pipeline (*t->module);

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

	EXPECT_EQ (jumps, 2u) << t->text ();
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
 * The counter cases, which need the instrumentation the tier-0 pipeline only
 * adds when the runtime's tier-2 threshold is set. jit.cpp reads that once and
 * keeps the answer, and the pipeline is built once per thread, so a case cannot
 * turn it on for itself. The build registers this suite as a run of its own with
 * the variable set, and the check below is what keeps a run without it honest.
 */
class JitProfile : public ::testing::Test {
public:
	static void SetUpTestSuite ()
	{
		MONO_SKIP_WITHOUT_CORPUS ();

		if (::getenv ("MONO_LLVM_JIT_TIER2_THRESHOLD") == nullptr)
			GTEST_SKIP () << "MONO_LLVM_JIT_TIER2_THRESHOLD is unset, so "
			                 "the pipeline instruments nothing";

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

	for (unsigned i = 0; i < arms; i++) {
		BasicBlock *taken = BasicBlock::Create (ctx, "taken" + Twine (i), fn);
		BasicBlock *next = BasicBlock::Create (ctx, "next" + Twine (i), fn);

		b.CreateCondBr (b.CreateICmpSGT (acc, b.getInt32 (int32_t (i))), taken,
		                next);
		b.SetInsertPoint (taken);
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
 * reads the section as a single method's array either answers with the other
 * method's numbers or refuses to answer.
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
	// entry counter back out of live code memory.
	auto fn_a = reinterpret_cast<int32_t (*) (int32_t)> (first.entry);
	auto fn_b = reinterpret_cast<int32_t (*) (int32_t)> (second.entry);

	for (int i = 0; i < 7; i++)
		fn_a (i);
	for (int i = 0; i < 3; i++)
		fn_b (i);

	EXPECT_EQ (a.counters[0], 7u);
	EXPECT_EQ (b.counters[0], 3u);
}
} // namespace
} // namespace test
} // namespace mono

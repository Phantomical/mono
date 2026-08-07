/*
 * Tests for MonoJit, the ORCv2 execution engine.
 *
 * Two layers: pure-LLVM tests that hand-build IR and never touch the runtime,
 * and end-to-end tests that boot the runtime, translate methods from the il/
 * corpus with method_to_llvm (), compile them through the jit and execute the
 * result. The end-to-end ones are the point: translator output actually runs.
 */

#include "harness.hpp"

#include "jit.hpp"
#include "passes/lower-builtins.hpp"

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

TEST (Jit, CompilesAndRunsHandBuiltIr)
{
	auto jit = MonoJit::create ();
	ASSERT_TRUE (bool (jit)) << toString (jit.takeError ());

	auto entry = (*jit)->compile (build_add_module ().take (), "add");
	ASSERT_TRUE (bool (entry)) << toString (entry.takeError ());

	auto add = reinterpret_cast<int32_t (*) (int32_t, int32_t)> (entry->entry);
	EXPECT_EQ (add (2, 40), 42);
	EXPECT_EQ (add (-7, 3), -4);
}

TEST (Jit, ResolvesRegisteredHelpers)
{
	auto jit = MonoJit::create ();
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

TEST (Jit, UnregisteredHelperFailsTheCompile)
{
	auto jit = MonoJit::create ();
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
 * Removal is how a freed method's code is given back. What has to hold is that
 * the memory returns to the pool the linker allocates out of - otherwise a
 * process that churns dynamic methods grows without bound - and that the JIT is
 * still usable afterwards.
 */
TEST (Jit, RemovedCodeIsReusedByLaterCompiles)
{
	auto jit = MonoJit::create ();
	ASSERT_TRUE (bool (jit)) << toString (jit.takeError ());

	auto first = (*jit)->compile (build_add_module ().take (), "add");
	ASSERT_TRUE (bool (first)) << toString (first.takeError ());
	ASSERT_NE (first->dylib, nullptr);

	const uint8_t *was = first->code;
	JITDylib *dylib = first->dylib;

	ASSERT_FALSE (bool ((*jit)->remove_dylibs ({ dylib })));

	/*
	 * Same module, so the same size request: the allocator has nothing else to
	 * satisfy it from, which is what makes the address a real check that the
	 * first one's memory came back rather than a coincidence.
	 */
	auto second = (*jit)->compile (build_add_module ().take (), "add");
	ASSERT_TRUE (bool (second)) << toString (second.takeError ());
	EXPECT_EQ (second->code, was);

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
		jit_holder () = cantFail (MonoJit::create ());
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

// The creator's call shape belongs to the pass. What the translator emitted names no
// this at all; what survives the lowering is the constructor's wrapper - the method
// the runtime publishes for a string constructor - called in fastcc with the null this
// it never reads, and nothing of the builtin left over.
TEST_F (JitExecution, LowerBuiltinsGivesTheCreatorItsNullThis)
{
	std::unique_ptr<Translation> t = translate_method ("objects", "Objects:MakeString");
	ASSERT_NE (t->function, nullptr) << t->error;
	ASSERT_EQ (t->count ("mono.builtin.creator."), 1u) << t->text ();

	PassBuilder pb;
	ModuleAnalysisManager mam;
	ModulePassManager mpm;

	pb.registerModuleAnalyses (mam);
	mpm.addPass (LowerBuiltinsPass ());
	mpm.run (*t->module, mam);

	EXPECT_EQ (t->count ("mono.builtin."), 0u) << t->text ();
	EXPECT_EQ (t->count ("call fastcc ptr @\"(wrapper managed-to-managed) string:.ctor"),
	           1u)
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
TEST (Jit, CallsAHelperFurtherAwayThanRel32Reaches)
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

	auto jit = MonoJit::create ();
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

} // namespace
} // namespace test
} // namespace mono

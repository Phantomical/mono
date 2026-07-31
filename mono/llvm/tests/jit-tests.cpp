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

#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>

#include <cstdint>
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

	auto add = reinterpret_cast<int32_t (*) (int32_t, int32_t)> (*entry);
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

	auto fn = reinterpret_cast<int64_t (*) (int64_t)> (*entry);
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
 * The end-to-end fixture: runtime booted, methods translated from the corpus,
 * compiled through one shared jit, executed.
 */
class JitExecution : public ::testing::Test {
public:
	static void SetUpTestSuite ()
	{
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
		return addr ? *addr : nullptr;
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

} // namespace
} // namespace test
} // namespace mono

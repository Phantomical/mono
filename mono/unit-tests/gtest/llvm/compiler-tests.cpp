/*
 * Tests for mono/llvm/compiler.cpp, specifically the assembly MONO_LLVM_JIT_ASM
 * prints out of the codegen pipeline.
 *
 * What makes this worth a test rather than an eyeball is the side tables: they
 * are written by a pass that only exists inside this backend's own pipeline, so
 * they are the half of the output no offline llc run over the same IR can
 * reproduce.
 */

#include "harness.hpp"

#include "jit.hpp"

#include <llvm/IR/Module.h>

#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <string>

using namespace llvm;
using namespace llvm::orc;

namespace mono {
namespace test {
namespace {

/*
 * What the dumps in this file are selected by. compiler.cpp reads
 * MONO_LLVM_JIT_ASM once, on the first compile of the process, and booting the
 * runtime already compiles a dozen methods - so the variable has to be in place
 * before any test runs, whether this binary was started for one case or for all
 * of them. Nothing but a module a test renamed contains this, so no other
 * compile is dumped.
 */
constexpr const char *asm_filter = "mono.asm.dump.fixture";

class SelectAsmDumps : public ::testing::Environment {
public:
	void SetUp () override
	{
		::setenv ("MONO_LLVM_JIT_ASM", asm_filter, 1);
		// The tier gate is read once as well, so the tier a case wants is
		// fixed here: tier 1 is kept and tier 2 is refused.
		::setenv ("MONO_LLVM_JIT_ASM_TIER", "1", 1);
	}
};

const ::testing::Environment *asm_dumps_selected =
	::testing::AddGlobalTestEnvironment (new SelectAsmDumps);

/// Everything written to fd 2 - where llvm::errs () goes - while this is alive.
class CapturedStderr {
public:
	CapturedStderr ()
	{
		sink_ = ::tmpfile ();
		saved_ = ::dup (STDERR_FILENO);
		::fflush (stderr);
		::dup2 (::fileno (sink_), STDERR_FILENO);
	}

	~CapturedStderr ()
	{
		restore ();
		::fclose (sink_);
	}

	/// What has been written so far, with stderr handed back to the process.
	std::string text ()
	{
		restore ();

		std::string out;
		char buffer[4096];

		::rewind (sink_);
		for (size_t got = ::fread (buffer, 1, sizeof (buffer), sink_); got != 0;
		     got = ::fread (buffer, 1, sizeof (buffer), sink_))
			out.append (buffer, got);

		return out;
	}

private:
	void restore ()
	{
		if (saved_ < 0)
			return;

		::fflush (stderr);
		::dup2 (saved_, STDERR_FILENO);
		::close (saved_);
		saved_ = -1;
	}

	FILE *sink_ = nullptr;
	int saved_ = -1;
};

extern "C" void
test_personality (void)
{
	abort ();
}

class AsmDump : public ::testing::Test {
public:
	static void SetUpTestSuite ()
	{
		MONO_SKIP_WITHOUT_CORPUS ();
		init_runtime ();
	}

protected:
	/// Translate and compile IMAGE's METHOD at the given tier, having renamed
	/// its module so the filter does or does not name it. Hands back the
	/// compiled function's symbol, which is what the assembly labels it.
	///
	/// folded names a second method of the same image to translate in beside it
	/// and mark always-inline, the way the engine's pre-pass folds a callee.
	std::string compile (const std::string &image, const std::string &method,
	                     bool dumped, JitTier tier = JitTier::tier1,
	                     const std::string &folded = std::string ())
	{
		std::unique_ptr<Translation> t = translate_method (image, method);

		EXPECT_NE (t->function, nullptr) << t->error;
		if (t->function == nullptr)
			return std::string ();

		if (!folded.empty ())
			EXPECT_NE (fold_method_into (*t, image, folded), nullptr);

		t->module->setModuleIdentifier (dumped ? asm_filter : image);

		std::string entry = t->function->getName ().str ();
		auto jit = test::make_jit ();

		EXPECT_TRUE (bool (jit)) << toString (jit.takeError ());
		if (!jit)
			return std::string ();

		/*
		 * The landing pads name a personality routine. It is never called - the
		 * runtime's unwinder re-enters the frame itself - but the object still
		 * has to be able to bind it.
		 */
		EXPECT_FALSE (bool ((*jit)->register_symbol (
			"mono_personality", (void *) &test_personality)));

		// As the runtime compiles: translator output still names the symbolic
		// calls the mono passes rewrite, so it cannot be linked as it stands.
		MonoJit::optimize (*t->module, tier);

		auto addr = (*jit)->compile (
			ThreadSafeModule (std::move (t->module),
		                      ThreadSafeContext (std::move (t->context))),
			entry);

		EXPECT_TRUE (bool (addr)) << toString (addr.takeError ());
		return entry;
	}
};

TEST_F (AsmDump, PrintsTheCodeAndTheClauseTableOfASelectedMethod)
{
	CapturedStderr captured;
	std::string entry = compile ("eh", "Eh:TryCatch", /*dumped=*/true);
	std::string dump = captured.text ();

	ASSERT_FALSE (entry.empty ());

	EXPECT_NE (dump.find (asm_filter), std::string::npos) << dump;
	/* The label the code starts at, quoted because the name has spaces in it. */
	EXPECT_NE (dump.find ("\"" + entry + "\":"), std::string::npos) << dump;
	/*
	 * Intel syntax, which the dump asks for through the `x86-asm-syntax` option
	 * and therefore gets from the MCAsmInfo. The return below is the same claim
	 * in the instructions: AT&T spells it `retq`.
	 */
	EXPECT_NE (dump.find ("\t.intel_syntax noprefix"), std::string::npos) << dump;
	EXPECT_NE (dump.find ("\tret\n"), std::string::npos) << dump;

	/*
	 * The point of dumping from inside the pipeline: the clause table, written
	 * by a pass no stock codegen run has.
	 */
	EXPECT_NE (dump.find (".section\t.mono_lsda"), std::string::npos) << dump;
	/* The frame description, which textual assembly carries as directives. */
	EXPECT_NE (dump.find (".cfi_startproc"), std::string::npos) << dump;
}

/*
 * The chain of bodies folded into a method, which rides beside the line table
 * and is what lets a stack walk report a frame for each of them.
 */
TEST_F (AsmDump, WritesTheInlineTableOfAFoldedBody)
{
	CapturedStderr captured;
	std::string entry = compile ("calls", "Calls:CallStatic", /*dumped=*/true,
	                             JitTier::tier1, "Calls:Helper");
	std::string dump = captured.text ();

	ASSERT_FALSE (entry.empty ());

	EXPECT_NE (dump.find (".section\t.mono_inlines"), std::string::npos) << dump;
	/* 'MINL', the section's magic, which the header opens with. */
	EXPECT_NE (dump.find ("\t.long\t1296649804"), std::string::npos) << dump;
	/* Each record names the function it describes an offset into. */
	EXPECT_NE (dump.find ("\t.quad\t\"" + entry + "\""), std::string::npos) << dump;
}

/*
 * A method with nothing folded into it gets a line table and no inline table.
 * The two sections are written from one set of rows, so this is what says the
 * case above is about the fold rather than about compiling anything at all.
 */
TEST_F (AsmDump, WritesNoInlineTableWithoutAFold)
{
	CapturedStderr captured;
	std::string entry = compile ("arith", "Arith:Add", /*dumped=*/true);
	std::string dump = captured.text ();

	ASSERT_FALSE (entry.empty ());
	EXPECT_NE (dump.find (".section\t.mono_lines"), std::string::npos) << dump;
	EXPECT_EQ (dump.find (".section\t.mono_inlines"), std::string::npos) << dump;
}

TEST_F (AsmDump, LeavesAMethodTheFilterDoesNotNameAlone)
{
	CapturedStderr captured;
	std::string entry = compile ("arith", "Arith:Add", /*dumped=*/false);
	std::string dump = captured.text ();

	ASSERT_FALSE (entry.empty ());
	EXPECT_EQ (dump.find ("*** assembly for"), std::string::npos) << dump;
}

/*
 * The same method the case above dumps, compiled at the other tier: the filter
 * still names it, so what refuses the dump is the tier and nothing else.
 */
TEST_F (AsmDump, LeavesAMethodOfAnotherTierAlone)
{
	CapturedStderr captured;
	std::string entry
		= compile ("eh", "Eh:TryCatch", /*dumped=*/true, JitTier::tier2);
	std::string dump = captured.text ();

	ASSERT_FALSE (entry.empty ());
	EXPECT_EQ (dump.find ("*** assembly for"), std::string::npos) << dump;
}

} // namespace
} // namespace test
} // namespace mono

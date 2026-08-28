/*
 * Tests for mono/llvm/compiler.cpp, specifically the assembly the `tier1-asm`
 * dump point prints out of the codegen pipeline.
 *
 * What makes this worth a test rather than an eyeball is the side tables: they
 * are written by a pass that only exists inside this backend's own pipeline, so
 * they are the half of the output no offline llc run over the same IR can
 * reproduce.
 */

#include "harness.hpp"

#include "dump.hpp"
#include "jit.hpp"
#include "mini.h"

#include <llvm/IR/Module.h>


#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

using namespace llvm;
using namespace llvm::orc;

namespace mono {
namespace test {
namespace {

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
	/// Translate and compile IMAGE's METHOD at the given tier, having given the
	/// body a dump name the filter does or does not name. Hands back the
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

		/* As the engine does, so the dump is filed and filtered by method. */
		set_dump_name (*t->function, dumped ? dump_filter : image);

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
		EXPECT_FALSE (bool ((*jit)->register_code_symbol (
			"mono_personality", (void *) &test_personality)));

		/*
		 * What the engine's own builtins registration gives a compile
		 * (runtime/builtins.cpp), for the two a body here reaches: the thrower
		 * behind a null or bounds check, and the copy codegen calls when the
		 * count is not a constant. Neither is called - nothing here runs the
		 * code - but the object still has to bind them.
		 */
		EXPECT_FALSE (bool ((*jit)->register_code_symbol (
			"mono_llvm_throw_corlib_exception",
			(void *) &mono_llvm_throw_corlib_exception)));
		EXPECT_FALSE (bool ((*jit)->register_code_symbol (
			"memcpy", (void *) static_cast<void *(*) (void *, const void *, size_t)> (
					  &std::memcpy))));

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
	CapturedStdout captured;
	std::string entry = compile ("eh", "Eh:TryCatch", /*dumped=*/true);
	std::string dump = captured.text ();

	ASSERT_FALSE (entry.empty ());

	EXPECT_NE (dump.find (dump_filter), std::string::npos) << dump;
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
	CapturedStdout captured;
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
	CapturedStdout captured;
	std::string entry = compile ("arith", "Arith:Add", /*dumped=*/true);
	std::string dump = captured.text ();

	ASSERT_FALSE (entry.empty ());
	EXPECT_NE (dump.find (".section\t.mono_lines"), std::string::npos) << dump;
	EXPECT_EQ (dump.find (".section\t.mono_inlines"), std::string::npos) << dump;
}

TEST_F (AsmDump, LeavesAMethodTheFilterDoesNotNameAlone)
{
	CapturedStdout captured;
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
	CapturedStdout captured;
	std::string entry
		= compile ("eh", "Eh:TryCatch", /*dumped=*/true, JitTier::tier2);
	std::string dump = captured.text ();

	ASSERT_FALSE (entry.empty ());
	EXPECT_EQ (dump.find ("*** assembly for"), std::string::npos) << dump;
}

/*
 * A copy whose count is a runtime value has two lowerings and no third: a call
 * to memcpy, or an inline loop. PreISelIntrinsicLowering picks the loop when it
 * finds no libcall for the target, and it asks the RuntimeLibraryInfoWrapper
 * that build_object_pipeline () adds. Leave that pass out and the legacy manager
 * builds one from an empty Triple, which has no libcalls at all, so every copy
 * in every method becomes a byte-at-a-time loop. That measured 2 GB/s against
 * glibc's 44 on a 64KB copy.
 *
 * Blocks:Copy is a cpblk over a count the caller passes, so it is one such copy
 * and nothing else. The call below is what says the pipeline still describes the
 * target to codegen.
 */
TEST_F (AsmDump, ACopyOfARuntimeLengthCallsTheLibrary)
{
	CapturedStdout captured;
	std::string entry = compile ("blocks", "Blocks:Copy", /*dumped=*/true);
	std::string dump = captured.text ();

	ASSERT_FALSE (entry.empty ());
	EXPECT_NE (dump.find ("memcpy"), std::string::npos) << dump;
}

} // namespace
} // namespace test
} // namespace mono

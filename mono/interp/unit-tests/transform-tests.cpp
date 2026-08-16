/*
 * What the transform's optimizations do to one method's bytecode.
 *
 * A case asserts on the whole opcode sequence rather than on the one instruction
 * it is about: an optimization that leaves an extra instruction standing is as
 * wrong as one that rewrites the wrong opcode, and only the whole list catches
 * both.
 */

#include "harness.hpp"

#include "mono/interp/mintops.hpp"

using namespace mono::test;

namespace {

class Cprop : public ::testing::Test {
protected:
	static void SetUpTestSuite ()
	{
		MONO_SKIP_WITHOUT_CORPUS ();
		init_runtime ();
	}

	void SetUp () override { MONO_SKIP_WITHOUT_CORPUS (); }
};

TEST_F (Cprop, FoldsAddOfTwoConstants)
{
	Transform transform ("snippets", "Snippets:test_cprop_add_consts");
	transform.cprop ();

	Code code (transform);

	ASSERT_EQ (code.opcodes (), (std::vector<std::string> {"ldc.i4", "ret"}));
	EXPECT_EQ (READ32 (&code.at (0)->data [0]), 0x1122u + 0x3344u);
}

TEST_F (Cprop, ForwardsALocalLoadedTwice)
{
	Transform transform ("snippets", "Snippets:test_cprop_ldloc_stloc");
	transform.cprop ();

	Code code (transform);

	ASSERT_EQ (code.opcodes (), (std::vector<std::string> {
		"initlocals", "call", "mov.4", "add.i4", "ret"}));

	/*
	 * Both loads are gone: the add reads the local the call's result was stored
	 * to, twice, rather than through a temporary of its own.
	 *
	 * The store itself stays. Its source is the call's return slot, which sits in
	 * the call-args region, and the rewrite that would let the call write the
	 * local directly refuses INTERP_LOCAL_FLAG_CALL_ARGS sources.
	 */
	InterpInst *store = code.at (2);
	InterpInst *add = code.at (3);

	EXPECT_EQ (add->sregs [0], store->dreg);
	EXPECT_EQ (add->sregs [1], store->dreg);
}

/*
 * The transform's own tracing, which MONO_VERBOSE_METHOD turns on for a run.  It
 * prints every instruction it rewrites, and the printer decodes each opcode's
 * operands from a table it keeps separately from the handlers.  A table that
 * disagrees with an opcode reads past the instruction, so the printer is worth
 * running even where nothing reads what it wrote.
 */
TEST_F (Cprop, PrintsWhatItRewrites)
{
	Transform transform ("snippets", "Snippets:test_cprop_ldloc_stloc", 4);

	testing::internal::CaptureStdout ();
	mono_interp_print_td_code (transform.get ());
	transform.cprop ();
	mono_interp_print_td_code (transform.get ());
	std::string printed = testing::internal::GetCapturedStdout ();

	EXPECT_NE (printed.find ("test_cprop_ldloc_stloc"), std::string::npos);
	EXPECT_NE (printed.find ("add.i4"), std::string::npos);
}

} // namespace

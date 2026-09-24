/*
 * test-eh-ranges.cpp: the native ranges a compiled body publishes its clauses
 * over.
 */

#include "config.h"

#include "metadata/class-internals.h"
#include "mini/domain-method.hpp"

#include "llvm/runtime.h"

#include <mono/metadata/appdomain.h>
#include <mono/metadata/assembly.h>
#include <mono/metadata/assembly-internals.h>
#include <mono/metadata/metadata.h>
#include <mono/utils/mono-error-internals.h>

#include <gtest/gtest.h>

#include <cstdint>

#include "harness.hpp"

namespace {

#define TESTPROG "eh-ranges.exe"

MonoImage *g_image;

class EHRanges : public ::testing::Test {
public:
	static void SetUpTestSuite ()
	{
		MONO_SKIP_WITHOUT_CLASS_LIBRARY ();
		mono::test::init_runtime ();

		if (g_image != nullptr)
			return;

		MonoAssemblyOpenRequest req;
		mono_assembly_request_prepare_open (
			&req, MONO_ASMCTX_DEFAULT,
			mono_domain_default_alc (mono_domain_get ()));

		MonoImageOpenStatus status;
		MonoAssembly *assembly = mono_assembly_request_open (TESTPROG, &req, &status);

		ASSERT_NE (nullptr, assembly) << "failed loading " TESTPROG;
		g_image = mono_assembly_get_image_internal (assembly);
	}

	void SetUp () override
	{
		MONO_SKIP_WITHOUT_CLASS_LIBRARY ();

		if (!mono_llvm_jit_tier2_enabled ())
			GTEST_SKIP () << "tier 2 is off in this configuration";
	}

protected:
	static MonoMethod *method_named (const char *name, int argc)
	{
		ERROR_DECL (error);
		MonoClass *klass = mono_class_from_name_checked (g_image, "", "EHRanges", error);

		mono_error_assert_ok (error);
		if (klass == nullptr)
			return nullptr;

		MonoMethod *found =
			mono_class_get_method_from_name_checked (klass, name, argc, 0, error);

		mono_error_assert_ok (error);
		return found;
	}
};

/// Whether the instruction that ends at \p end is a call.
bool
ends_after_call (const std::uint8_t *end)
{
	// call rel32
	if (end[-5] == 0xe8)
		return true;
	// call qword ptr [rip + disp32]
	if (end[-6] == 0xff && end[-5] == 0x15)
		return true;
	// call reg, with or without a REX prefix
	return end[-2] == 0xff && (end[-1] & 0xf8) == 0xd0;
}

} // namespace

/*
 * Inner's catch, inlined into Outer's try at tier 2, covers the call to Foo
 * and ends there. Outer's own clause widens over the code around it, and that
 * code lies outside Inner's try.
 */
TEST_F (EHRanges, AnInlinedClauseCoversOnlyItsCall)
{
	ERROR_DECL (error);
	MonoDomain *domain = mono_domain_get ();
	MonoMethod *method = method_named ("Outer", 1);

	ASSERT_NE (nullptr, method);
	ASSERT_TRUE (mono_llvm_jit_promote_now (method, domain, (uint8_t) mono::MonoTier::tier2))
		<< "Outer would not compile at tier 2";

	void *body = mono_llvm_jit_find_body (domain, method);
	ASSERT_NE (nullptr, body);

	MonoJitInfo *jinfo = mono_jit_info_table_find (domain, body);
	ASSERT_NE (nullptr, jinfo);

	MonoClass *argument = mono_class_from_name_checked (
		mono_defaults.corlib, "System", "ArgumentException", error);
	mono_error_assert_ok (error);

	int inner = 0;

	for (guint32 i = 0; i < jinfo->num_clauses; ++i) {
		const MonoJitExceptionInfo &ei = jinfo->clauses[i];

		if (ei.flags != MONO_EXCEPTION_CLAUSE_NONE || ei.data.catch_class != argument)
			continue;

		++inner;
		EXPECT_TRUE (ends_after_call ((const std::uint8_t *) ei.try_end))
			<< "Inner's catch runs past its call, over code outside Inner's try";
	}

	ASSERT_GT (inner, 0) << "Inner was not inlined with its clause, so this checks nothing";
}

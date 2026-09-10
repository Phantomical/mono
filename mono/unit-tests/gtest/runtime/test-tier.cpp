/*
 * test-tier.cpp: Unit test for MonoJitInfo::tier.
 *
 * A method keeps up to three live bodies at once, whichever of tier 0,
 * tier 1 and tier 2 published before the next one superseded it. A profiler
 * has to read a body's tier off its own MonoJitInfo, because the record
 * only answers for the current body.
 */

#include "config.h"

#include "metadata/class-internals.h"
#include "metadata/domain-internals.h"
#include "metadata/object-internals.h"
#include "mini/domain-method.h"
#include "mini/domain-method.hpp"

#include "llvm/runtime.h"

#include <mono/metadata/appdomain.h>
#include <mono/metadata/assembly.h>
#include <mono/metadata/assembly-internals.h>
#include <mono/metadata/metadata.h>
#include <mono/metadata/object.h>
#include <mono/utils/mono-error-internals.h>

#include <gtest/gtest.h>

#include <vector>

#include "harness.hpp"

namespace {

#define TESTPROG "tier.exe"

MonoImage *g_image;

class MethodTier : public ::testing::Test {
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

		// The first case needs a real classic tier-0 body, which carries a
		// jit info. An interpreted one does not.
		if (mono_llvm_jit_interp_tier0_enabled ())
			GTEST_SKIP () << "the interpreter is tier 0 here, so this checks nothing";
		if (!mono_llvm_jit_tier0_enabled ())
			GTEST_SKIP () << "tier 0 is off in this configuration";
		if (!mono_llvm_jit_tier2_enabled ())
			GTEST_SKIP () << "tier 2 is off in this configuration";
	}

protected:
	static MonoMethod *method_named (const char *name, int argc)
	{
		ERROR_DECL (error);
		MonoClass *klass = mono_class_from_name_checked (g_image, "", "Tier", error);

		mono_error_assert_ok (error);
		if (klass == nullptr)
			return nullptr;

		MonoMethod *found =
			mono_class_get_method_from_name_checked (klass, name, argc, 0, error);

		mono_error_assert_ok (error);
		return found;
	}
};

} // namespace

/*
 * Every body a promotion leaves behind keeps the tier that compiled it, on
 * its own MonoJitInfo - the superseded tier-0 and tier-1 bodies included.
 * The published jit-info table reads the same tier back.
 */
TEST_F (MethodTier, EachSupersededBodyKeepsItsOwnTier)
{
	ERROR_DECL (error);
	MonoDomain *domain = mono_domain_get ();
	MonoMethod *method = method_named ("Probe", 1);

	ASSERT_NE (nullptr, method);
	ASSERT_GT (mono_llvm_jit_tier0_budget (method), 0)
		<< "this method no longer starts at tier 0, so it checks nothing";

	mono_compile_method_checked (method, error);
	mono_error_assert_ok (error);

	ASSERT_TRUE (mono_llvm_jit_promote_now (method, domain, (uint8_t) mono::MonoTier::tier1))
		<< "Probe would not compile at tier 1";
	ASSERT_TRUE (mono_llvm_jit_promote_now (method, domain, (uint8_t) mono::MonoTier::tier2))
		<< "Probe would not compile at tier 2";

	mono::MonoDomainMethod *dm = mono::domain_method_find (domain, method);
	ASSERT_NE (nullptr, dm);

	std::vector<mono::MonoMethodBody> bodies;
	dm->foreach_body ([&] (const mono::MonoMethodBody &body) { bodies.push_back (body); });

	ASSERT_EQ (3u, bodies.size ())
		<< "the record kept a different number of bodies than the three promotions asked for";

	EXPECT_EQ (mono::MonoTier::tier0, bodies[0].tier);
	EXPECT_EQ (mono::MonoTier::tier1, bodies[1].tier);
	EXPECT_EQ (mono::MonoTier::tier2, bodies[2].tier);

	for (const mono::MonoMethodBody &body : bodies) {
		ASSERT_NE (nullptr, body.jinfo)
			<< "a compiled body with no jit info is nothing a profiler can read the tier off";
		EXPECT_EQ (body.tier, (mono::MonoTier) body.jinfo->tier);

		MonoJitInfo *published = mono_jit_info_table_find (domain, body.code);

		ASSERT_NE (nullptr, published)
			<< "a superseded body dropped out of the jit info table";
		EXPECT_EQ (body.tier, (mono::MonoTier) published->tier);
	}
}

/*
 * A filter clause compiles to a body of its own, registered under the same
 * method but never attached to the record. The record's promotions never
 * touch its jit info, so it stays at MonoTier::none.
 */
TEST_F (MethodTier, AFilterBodyCarriesNoTier)
{
	MonoDomain *domain = mono_domain_get ();
	MonoMethod *method = method_named ("WithFilter", 1);

	ASSERT_NE (nullptr, method);

	// Straight to tier 1, so the LLVM backend - not the classic compiler -
	// lowers the filter clause into a body of its own.
	ASSERT_TRUE (mono_llvm_jit_promote_now (method, domain, (uint8_t) mono::MonoTier::tier1))
		<< "WithFilter would not compile at tier 1";

	void *body = mono_llvm_jit_find_body (domain, method);
	ASSERT_NE (nullptr, body);

	MonoJitInfo *jinfo = mono_jit_info_table_find (domain, body);
	ASSERT_NE (nullptr, jinfo);

	void *filter = nullptr;

	for (guint32 i = 0; i < jinfo->num_clauses; ++i) {
		if (jinfo->clauses[i].flags == MONO_EXCEPTION_CLAUSE_FILTER) {
			filter = jinfo->clauses[i].data.filter;
			break;
		}
	}

	ASSERT_NE (nullptr, filter) << "the compile carried no filter clause to check";

	MonoJitInfo *filter_jinfo = mono_jit_info_table_find (domain, filter);

	ASSERT_NE (nullptr, filter_jinfo) << "the filter body dropped out of the jit info table";
	ASSERT_NE (jinfo, filter_jinfo)
		<< "the lookup answered with the method's own jit info, not the filter's";
	EXPECT_TRUE (filter_jinfo->llvm_side_body)
		<< "a filter body's jit info is always registered with a null header";
	EXPECT_EQ (mono::MonoTier::none, (mono::MonoTier) filter_jinfo->tier);
}

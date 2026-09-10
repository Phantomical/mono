/*
 * test-etw-profiler.cpp: Unit tests for the two payload computations
 * mono/mini/etw-profiler.cpp's HOST_WIN32 half calls: etw_method_flags () and
 * etw_body_il_map (). Both take no ETW session, so these run on every host.
 */

#include "config.h"

#include "metadata/class-internals.h"
#include "metadata/domain-internals.h"
#include "metadata/metadata-internals.h"
#include "metadata/object-internals.h"
#include "mini/domain-method.h"
#include "mini/domain-method.hpp"
#include "mini/etw-profiler.hpp"

#include "llvm/runtime.h"

#include <mono/metadata/appdomain.h>
#include <mono/metadata/assembly.h>
#include <mono/metadata/assembly-internals.h>
#include <mono/metadata/debug-helpers.h>
#include <mono/metadata/loader.h>
#include <mono/metadata/marshal.h>
#include <mono/metadata/object.h>
#include <mono/utils/mono-error-internals.h>

#include <gtest/gtest.h>

#include <cstring>
#include <vector>

#include "harness.hpp"

namespace {

#define TESTPROG "etw.exe"

MonoImage *g_image;

class EtwProfilerBase : public ::testing::Test {
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

protected:
	static MonoClass *class_named (const char *name)
	{
		ERROR_DECL (error);
		MonoClass *klass = mono_class_from_name_checked (g_image, "", name, error);

		mono_error_assert_ok (error);
		return klass;
	}

	static MonoMethod *method_of (MonoClass *klass, const char *name, int argc)
	{
		ERROR_DECL (error);
		MonoMethod *found =
			mono_class_get_method_from_name_checked (klass, name, argc, 0, error);

		mono_error_assert_ok (error);
		return found;
	}

	static MonoMethod *method_named (const char *name, int argc)
	{
		MonoClass *klass = class_named ("Etw");
		return klass != nullptr ? method_of (klass, name, argc) : nullptr;
	}

	/* The one instantiation over string of a one-type-parameter method - see
	 * test-detour.cpp's instantiated_over_string (), which this mirrors. */
	static MonoMethod *instantiated_over_string (MonoMethod *definition)
	{
		ERROR_DECL (error);
		MonoType *arguments[1] = { m_class_get_byval_arg (mono_get_string_class ()) };
		MonoGenericContext context;

		memset (&context, 0, sizeof (context));
		context.method_inst = mono_metadata_get_generic_inst (1, arguments);

		MonoMethod *inflated =
			mono_class_inflate_generic_method_checked (definition, &context, error);

		mono_error_assert_ok (error);
		return inflated;
	}

	/* A method of one reference instantiation of a generic class - mirrors
	 * test-detour.cpp's class_method_over (). */
	static MonoMethod *class_method_over (const char *type, const char *name, int argc,
	                                      MonoClass *argument)
	{
		ERROR_DECL (error);
		MonoClass *definition = class_named (type);

		if (definition == nullptr)
			return nullptr;

		MonoType *arguments[1] = { m_class_get_byval_arg (argument) };
		MonoGenericContext context;

		memset (&context, 0, sizeof (context));
		context.class_inst = mono_metadata_get_generic_inst (1, arguments);

		MonoClass *inflated =
			mono_class_inflate_generic_class_checked (definition, &context, error);

		mono_error_assert_ok (error);
		if (inflated == nullptr)
			return nullptr;

		return method_of (inflated, name, argc);
	}

	static MonoClass *nested_class_named (MonoClass *parent, const char *name)
	{
		void *iter = nullptr;
		MonoClass *nested;

		while ((nested = mono_class_get_nested_types (parent, &iter)) != nullptr) {
			if (strcmp (m_class_get_name (nested), name) == 0)
				return nested;
		}
		return nullptr;
	}
};

class EtwProfiler : public EtwProfilerBase {
public:
	void SetUp () override
	{
		MONO_SKIP_WITHOUT_CLASS_LIBRARY ();

		// The tier bits test needs a classic tier-0 body, which carries a jit
		// info. It also promotes through tier 1 and tier 2 for real.
		if (mono_llvm_jit_interp_tier0_enabled ())
			GTEST_SKIP () << "the interpreter is tier 0 here, so this checks nothing";
		if (!mono_llvm_jit_tier0_enabled ())
			GTEST_SKIP () << "tier 0 is off in this configuration";
		if (!mono_llvm_jit_tier2_enabled ())
			GTEST_SKIP () << "tier 2 is off in this configuration";
	}
};

/* The naming tests need only the loaded image and a method's metadata, so
 * they run wherever the class libraries do - none of EtwProfiler's tier
 * skips apply. */
class EtwProfilerNaming : public EtwProfilerBase {
public:
	void SetUp () override
	{
		MONO_SKIP_WITHOUT_CLASS_LIBRARY ();
	}
};

} // namespace

/*
 * A method promoted through all three tiers keeps one live body per tier
 * (test-tier.cpp's EachSupersededBodyKeepsItsOwnTier). Jitted has to be set
 * on all three bodies, and the tier bits have to name each one's own tier.
 */
TEST_F (EtwProfiler, FlagsCarryJittedAndTheRightTierBits)
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
	ASSERT_EQ (3u, bodies.size ());

	// MinOptJitted, then InstrumentedTier or QuickJitted depending on whether
	// tier 2 is on, then OptimizedTier1 - see clr_tier () in etw-profiler.cpp.
	uint32_t expected_tier[3] = { 1, mono_llvm_jit_tier2_enabled () ? 6u : 3u, 4 };

	for (size_t i = 0; i < bodies.size (); ++i) {
		ASSERT_NE (nullptr, bodies[i].jinfo);

		uint32_t flags = mono::etw_method_flags (method, bodies[i].jinfo);

		EXPECT_TRUE (flags & 0x8) << "Jitted must be set on body " << i;
		EXPECT_EQ (expected_tier[i], (flags >> 7) & 0x7) << "wrong tier bits on body " << i;
	}
}

/*
 * Generic is set from method->is_inflated alone, so it has to tell an
 * instantiation from the generic method definition it came from.
 */
TEST_F (EtwProfiler, GenericFlagSetOnlyForInflatedMethod)
{
	MonoMethod *definition = method_named ("Generic", 1);
	ASSERT_NE (nullptr, definition);

	MonoMethod *inflated = instantiated_over_string (definition);
	ASSERT_NE (nullptr, inflated);

	MonoJitInfo jinfo;
	memset (&jinfo, 0, sizeof (jinfo));

	EXPECT_TRUE (mono::etw_method_flags (inflated, &jinfo) & 0x2)
		<< "an instantiation is inflated";
	EXPECT_FALSE (mono::etw_method_flags (definition, &jinfo) & 0x2)
		<< "the generic method definition itself is not inflated";
}

/*
 * A method on an ordinary top-level class: etw_method_namespace () carries
 * the class alone, and the method's own bare name is the other half
 * TraceLog.cs concatenates.
 */
TEST_F (EtwProfilerNaming, OrdinaryMethod)
{
	MonoMethod *method = method_named ("Probe", 1);
	ASSERT_NE (nullptr, method);

	char *ns = mono::etw_method_namespace (method);
	EXPECT_STREQ ("Etw", ns);
	g_free (ns);

	EXPECT_STREQ ("Probe", mono_method_get_name (method));
}

/*
 * A method on a nested class: the chain back to the top-level class has to
 * survive, the way it does in mono_type_get_full_name () for any other
 * reflection-shaped name.
 */
TEST_F (EtwProfilerNaming, NestedClassMethod)
{
	MonoClass *outer = class_named ("Etw");
	ASSERT_NE (nullptr, outer);

	MonoClass *nested = nested_class_named (outer, "Nested");
	ASSERT_NE (nullptr, nested);

	MonoMethod *method = method_of (nested, "Method", 1);
	ASSERT_NE (nullptr, method);

	char *ns = mono::etw_method_namespace (method);
	EXPECT_STREQ ("Etw+Nested", ns);
	g_free (ns);

	EXPECT_STREQ ("Method", mono_method_get_name (method));
}

/*
 * A method on a reference instantiation of a generic class: the type
 * argument has to show up in etw_method_namespace ()'s result.
 */
TEST_F (EtwProfilerNaming, GenericClassInstantiationMethod)
{
	MonoMethod *method =
		class_method_over ("Boxed`1", "Method", 1, mono_get_string_class ());
	ASSERT_NE (nullptr, method);

	char *ns = mono::etw_method_namespace (method);
	EXPECT_STREQ ("Boxed`1[System.String]", ns);
	g_free (ns);

	EXPECT_STREQ ("Method", mono_method_get_name (method));
}

/*
 * A generic method: etw_method_namespace () and mono_method_get_name () read
 * off the declaring class and the bare method name. Neither changes with the
 * method's own instantiation - CoreCLR's MethodName field carries no method
 * type argument either, since MethodDesc::GetMethodInfoNoSig calls GetName ()
 * alone. The signature is what still tells an instantiation apart from its
 * definition.
 */
TEST_F (EtwProfilerNaming, GenericMethodNameIsTheSameAcrossInstantiations)
{
	MonoMethod *definition = method_named ("Generic", 1);
	ASSERT_NE (nullptr, definition);

	MonoMethod *inflated = instantiated_over_string (definition);
	ASSERT_NE (nullptr, inflated);

	char *definition_ns = mono::etw_method_namespace (definition);
	char *inflated_ns = mono::etw_method_namespace (inflated);
	EXPECT_STREQ ("Etw", definition_ns);
	EXPECT_STREQ (definition_ns, inflated_ns);
	g_free (definition_ns);
	g_free (inflated_ns);

	EXPECT_STREQ ("Generic", mono_method_get_name (definition));
	EXPECT_STREQ ("Generic", mono_method_get_name (inflated));

	char *definition_sig =
		mono_signature_get_desc (mono_method_signature_internal (definition), TRUE);
	char *inflated_sig =
		mono_signature_get_desc (mono_method_signature_internal (inflated), TRUE);
	EXPECT_STRNE (definition_sig, inflated_sig)
		<< "the signature is the only field that still tells the two apart";
	g_free (definition_sig);
	g_free (inflated_sig);
}

/*
 * A wrapper: mono_marshal_get_synchronized_wrapper () builds one over any
 * method, without needing [MethodImpl (Synchronized)] on it. It keeps the
 * target's own klass and name (mono_mb_new (), method-builder.c), so naming
 * a wrapper works the same as naming the method it wraps.
 */
TEST_F (EtwProfilerNaming, WrapperKeepsTheTargetsName)
{
	MonoMethod *target = method_named ("Probe", 1);
	ASSERT_NE (nullptr, target);

	MonoMethod *wrapper = mono_marshal_get_synchronized_wrapper (target);
	ASSERT_NE (nullptr, wrapper);
	ASSERT_NE (MONO_WRAPPER_NONE, wrapper->wrapper_type);

	char *target_ns = mono::etw_method_namespace (target);
	char *wrapper_ns = mono::etw_method_namespace (wrapper);
	EXPECT_STREQ (target_ns, wrapper_ns);
	g_free (target_ns);
	g_free (wrapper_ns);

	EXPECT_STREQ (mono_method_get_name (target), mono_method_get_name (wrapper));
}

/*
 * jinfo->llvm_seq_points is a per-body map, so a method with three live
 * bodies has to read back three different maps, one per jinfo.
 */
TEST_F (EtwProfiler, ILMapComesFromTheBodyNotTheMethod)
{
	ERROR_DECL (error);
	MonoDomain *domain = mono_domain_get ();
	MonoMethod *method = method_named ("ProbeMap", 1);

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
	ASSERT_EQ (3u, bodies.size ());
	ASSERT_NE (nullptr, bodies[0].jinfo);
	ASSERT_NE (nullptr, bodies[1].jinfo);
	ASSERT_NE (nullptr, bodies[2].jinfo);

	EXPECT_EQ (0u, bodies[0].jinfo->n_llvm_seq_points)
		<< "nothing under mono/mini/tier0/ publishes a per-body map";

	uint32_t tier1_il[64], tier1_native[64];
	uint32_t tier2_il[64], tier2_native[64];

	uint32_t n1 = mono::etw_body_il_map (bodies[1].jinfo, tier1_il, tier1_native, 64);
	uint32_t n2 = mono::etw_body_il_map (bodies[2].jinfo, tier2_il, tier2_native, 64);

	ASSERT_GT (n1, 0u) << "tier 1's translation recovered no map to check";
	ASSERT_GT (n2, 0u) << "tier 2's translation recovered no map to check";

	// Every pair this reports for a body is a row of that body's own jinfo.
	for (uint32_t i = 0; i < n1; ++i) {
		bool found = false;
		for (uint32_t j = 0; j < bodies[1].jinfo->n_llvm_seq_points; ++j)
			if (bodies[1].jinfo->llvm_seq_points[j].il_offset == tier1_il[i]
			    && bodies[1].jinfo->llvm_seq_points[j].native_offset == tier1_native[i])
				found = true;
		EXPECT_TRUE (found) << "entry " << i << " is not a row of tier 1's own jinfo";
	}

	// A method-keyed table would answer the same for both tiers.
	bool differs = n1 != n2;
	for (uint32_t i = 0; !differs && i < n1; ++i)
		differs = tier1_native[i] != tier2_native[i];
	EXPECT_TRUE (differs) << "tier 1 and tier 2 read back the same map";
}

/* mono::etw_body_il_map ()'s dedupe, exercised on a jinfo this constructs
 * directly rather than one a compile produced. */
TEST (EtwProfilerPure, ILMapDedupesByILOffsetAndStaysSorted)
{
	MonoLLVMSeqPoint rows[6] = {
		{ 0, 0 }, { 4, 0 }, { 8, 3 }, { 12, 3 }, { 16, 7 }, { 20, 7 },
	};

	MonoJitInfo jinfo;
	memset (&jinfo, 0, sizeof (jinfo));
	jinfo.llvm_seq_points = rows;
	jinfo.n_llvm_seq_points = 6;

	uint32_t il[16], native[16];
	uint32_t n = mono::etw_body_il_map (&jinfo, il, native, 16);

	ASSERT_EQ (3u, n);
	EXPECT_EQ (0u, il[0]);
	EXPECT_EQ (0u, native[0]);
	EXPECT_EQ (3u, il[1]);
	EXPECT_EQ (8u, native[1]);
	EXPECT_EQ (7u, il[2]);
	EXPECT_EQ (16u, native[2]);

	for (uint32_t i = 1; i < n; ++i)
		EXPECT_LT (native[i - 1], native[i]);
}

/* The 7000-entry cap in etw-profiler.cpp is a max_entries argument here, so
 * this checks the fill stops there without building 7000 rows. */
TEST (EtwProfilerPure, ILMapRespectsCap)
{
	std::vector<MonoLLVMSeqPoint> rows;

	for (uint32_t i = 0; i < 10; ++i)
		rows.push_back (MonoLLVMSeqPoint { i * 4, i });

	MonoJitInfo jinfo;
	memset (&jinfo, 0, sizeof (jinfo));
	jinfo.llvm_seq_points = rows.data ();
	jinfo.n_llvm_seq_points = (guint32) rows.size ();

	uint32_t il[4], native[4];
	uint32_t n = mono::etw_body_il_map (&jinfo, il, native, 4);

	ASSERT_EQ (4u, n);
	for (uint32_t i = 0; i < n; ++i) {
		EXPECT_EQ (i, il[i]);
		EXPECT_EQ (i * 4, native[i]);
	}
}

namespace {

// evntrace.h's EVENT_CONTROL_CODE_ENABLE_PROVIDER and
// EVENT_CONTROL_CODE_CAPTURE_STATE, mirrored the way etw-profiler.cpp's
// own kEventControlCode* constants mirror them, plus
// EVENT_CONTROL_CODE_DISABLE_PROVIDER (0), which has no counterpart there.
constexpr uint32_t kEnableProvider = 1;
constexpr uint32_t kCaptureState = 2;
constexpr uint32_t kDisableProvider = 0;

// CLR-ETW-Generated.h's own CLR_RUNDOWNSTART_KEYWORD and
// CLR_RUNDOWNEND_KEYWORD.
constexpr uint64_t kStartKeyword = 0x40;
constexpr uint64_t kEndKeyword = 0x100;

} // namespace

TEST (EtwProfilerPure, RundownPassNoneWhenNeitherKeywordSet)
{
	mono::EtwRundownPass pass = mono::etw_rundown_pass (kEnableProvider, 0, true);

	EXPECT_FALSE (pass.start);
	EXPECT_FALSE (pass.end);
}

TEST (EtwProfilerPure, RundownPassStartOnly)
{
	mono::EtwRundownPass pass = mono::etw_rundown_pass (kEnableProvider, kStartKeyword, true);

	EXPECT_TRUE (pass.start);
	EXPECT_FALSE (pass.end);
}

TEST (EtwProfilerPure, RundownPassEndOnly)
{
	mono::EtwRundownPass pass = mono::etw_rundown_pass (kEnableProvider, kEndKeyword, true);

	EXPECT_FALSE (pass.start);
	EXPECT_TRUE (pass.end);
}

TEST (EtwProfilerPure, RundownPassBothWhenBothKeywordsSet)
{
	mono::EtwRundownPass pass =
		mono::etw_rundown_pass (kEnableProvider, kStartKeyword | kEndKeyword, true);

	EXPECT_TRUE (pass.start);
	EXPECT_TRUE (pass.end);
}

/* CLR_RUNDOWNEND_KEYWORD is 0x100. 0x80 is CLR_ENDENUMERATION_KEYWORD
 * (CLR-ETW-Generated.h), a different keyword etw_rundown_pass () does not
 * read - this guards against confusing the two. */
TEST (EtwProfilerPure, RundownPassIgnoresStaleKeyword)
{
	mono::EtwRundownPass pass = mono::etw_rundown_pass (kEnableProvider, 0x80, true);

	EXPECT_FALSE (pass.start);
	EXPECT_FALSE (pass.end);
}

TEST (EtwProfilerPure, RundownPassNoneOnRegularProvider)
{
	mono::EtwRundownPass pass =
		mono::etw_rundown_pass (kEnableProvider, kStartKeyword | kEndKeyword, false);

	EXPECT_FALSE (pass.start);
	EXPECT_FALSE (pass.end);
}

TEST (EtwProfilerPure, RundownPassAlsoAnsweredForCaptureState)
{
	mono::EtwRundownPass pass = mono::etw_rundown_pass (kCaptureState, kEndKeyword, true);

	EXPECT_FALSE (pass.start);
	EXPECT_TRUE (pass.end);
}

TEST (EtwProfilerPure, RundownPassNoneOnDisableProvider)
{
	mono::EtwRundownPass pass =
		mono::etw_rundown_pass (kDisableProvider, kStartKeyword | kEndKeyword, true);

	EXPECT_FALSE (pass.start);
	EXPECT_FALSE (pass.end);
}

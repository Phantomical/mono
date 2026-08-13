/*
 * test-function-pointer.cpp: Unit test for the address a raw function pointer
 * request gives back.
 *
 * RuntimeMethodHandle.GetFunctionPointer () is mono_compile_method_checked ().
 * A native code patcher asks for that address and writes a jump over it, which
 * is how Harmony and MonoMod redirect a method. Unity mod code is built on
 * them, so the address they get decides whether any of it runs.
 *
 * The patch reaches the method only when the address is the method's own stub.
 * A stub has three properties the patch depends on:
 *
 *  - one address per method
 *  - the same address on every request
 *  - a position in front of whatever tier runs the method now
 *
 * A body fails the third. A promotion writes a new body, and the patch stays
 * behind on the old one. A shared entry fails the first. A write over it
 * reaches every method behind it, not the one the patcher asked for.
 *
 * MonoMod does its own check at startup and throws when the patch does not
 * arrive. That throw happens in a static constructor, so one bad address costs
 * every Harmony patch in the process rather than one.
 */

#include "config.h"

#include "metadata/class-internals.h"
#include "metadata/object-internals.h"
#include "mini/jit.h"

#include "llvm/runtime.h"

#include <gtest/gtest.h>

#include "harness.hpp"

namespace {

class FunctionPointer : public ::testing::Test {
public:
	static void SetUpTestSuite ()
	{
		MONO_SKIP_WITHOUT_CLASS_LIBRARY ();
		mono::test::init_runtime ();
	}

	void SetUp () override
	{
		MONO_SKIP_WITHOUT_CLASS_LIBRARY ();

		/*
		 * With tier 0 off every method is compiled, and the cases below
		 * then agree for a reason that has nothing to do with what they
		 * check. Say so as a skip instead of passing on it.
		 */
		if (!mono_llvm_jit_tier0_enabled ())
			GTEST_SKIP () << "tier 0 is off in this configuration";
	}
};

/*
 * Two corlib methods that the class libraries do not call while the runtime
 * starts, so a case finds them at tier 0. Both are ordinary IL with no generic
 * parameters, which is what runs_at_tier0 () accepts.
 *
 * Each case asserts the tier rather than assuming it, so a corlib that starts
 * reaching one of these fails here by name instead of quietly checking a
 * compiled method.
 */
MonoMethod *
cold_method (const char *name_space, const char *name, const char *method, int argc)
{
	ERROR_DECL (error);
	MonoClass *klass =
		mono_class_from_name_checked (mono_defaults.corlib, name_space, name, error);

	mono_error_assert_ok (error);
	if (klass == nullptr)
		return nullptr;

	MonoMethod *found =
		mono_class_get_method_from_name_checked (klass, method, argc, 0, error);

	mono_error_assert_ok (error);
	return found;
}

} // namespace

/*
 * The address is the stub the backend published, so the caller and the runtime
 * name the same thing. A compile path that answered around the backend would
 * give back an address the backend never published, and the two would differ.
 */
TEST_F (FunctionPointer, IsTheStubTheBackendPublished)
{
	ERROR_DECL (error);
	MonoMethod *method = cold_method ("System", "Version", "Clone", 0);

	ASSERT_NE (nullptr, method);
	ASSERT_GT (mono_llvm_jit_tier0_calls (method), 0)
		<< "this method no longer starts at tier 0, so it checks nothing";

	void *from_handle = mono_compile_method_checked (method, error);
	mono_error_assert_ok (error);
	ASSERT_NE (nullptr, from_handle);

	void *published = mono_llvm_jit_compile_method (method, mono_domain_get (), error);
	mono_error_assert_ok (error);

	EXPECT_EQ (published, from_handle);
}

/*
 * A second request gives the same address. A caller that wrote over the first
 * one keeps whatever it wrote, and a caller that cached it still calls the
 * method the address names.
 */
TEST_F (FunctionPointer, IsStableAcrossRequests)
{
	ERROR_DECL (error);
	MonoMethod *method = cold_method ("System", "Version", "Clone", 0);

	ASSERT_NE (nullptr, method);

	void *first = mono_compile_method_checked (method, error);
	mono_error_assert_ok (error);
	void *second = mono_compile_method_checked (method, error);
	mono_error_assert_ok (error);

	ASSERT_NE (nullptr, first);
	EXPECT_EQ (first, second);
}

/*
 * A promotion moves the method to a compiled body and leaves the address alone.
 * This is what a patch written over the address depends on: the stub is
 * redirected to the new body, so the patch keeps its place in front of it. An
 * address that moved would leave the patch on a body nothing calls any more.
 */
TEST_F (FunctionPointer, SurvivesPromotion)
{
	ERROR_DECL (error);
	MonoDomain *domain = mono_domain_get ();
	MonoMethod *method = cold_method ("System", "Version", "Clone", 0);

	ASSERT_NE (nullptr, method);
	ASSERT_GT (mono_llvm_jit_tier0_calls (method), 0)
		<< "this method no longer starts at tier 0, so it checks nothing";

	void *before = mono_compile_method_checked (method, error);
	mono_error_assert_ok (error);
	ASSERT_NE (nullptr, before);

	/* No body yet is what makes this a promotion rather than a second look. */
	ASSERT_EQ (nullptr, mono_llvm_jit_find_body (domain, method));
	ASSERT_TRUE (mono_llvm_jit_request_promotion (method, domain));

	/* The request returns before the compile does, so wait for the body. */
	void *body = nullptr;
	for (int waited = 0; body == nullptr && waited < 10000; ++waited) {
		body = mono_llvm_jit_find_body (domain, method);
		if (body == nullptr)
			g_usleep (1000);
	}
	ASSERT_NE (nullptr, body) << "the promotion never produced a body";

	void *after = mono_compile_method_checked (method, error);
	mono_error_assert_ok (error);

	EXPECT_EQ (before, after);
	EXPECT_NE (body, after);
}

/*
 * Two methods get two addresses. This is what makes the address usable as a
 * patch site: a write over one must not reach the other, which it would if the
 * shared entry behind them were handed out instead.
 */
TEST_F (FunctionPointer, IsDistinctPerMethod)
{
	ERROR_DECL (error);
	MonoMethod *first_method = cold_method ("System", "Version", "Clone", 0);
	MonoMethod *second_method = cold_method ("System", "Version", "GetHashCode", 0);

	ASSERT_NE (nullptr, first_method);
	ASSERT_NE (nullptr, second_method);
	ASSERT_NE (first_method, second_method);

	void *first = mono_compile_method_checked (first_method, error);
	mono_error_assert_ok (error);
	void *second = mono_compile_method_checked (second_method, error);
	mono_error_assert_ok (error);

	ASSERT_NE (nullptr, first);
	ASSERT_NE (nullptr, second);
	EXPECT_NE (first, second);
}

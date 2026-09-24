/*
 * test-unity-liveness.cpp: Unit test for the liveness walk Unity runs over
 * the managed heap with the world stopped.
 *
 * Each case checks that every object the walk reaches is reported once and
 * that no mark bit outlives mono_unity_liveness_calculation_end (). CMake
 * runs the suite once with one walker thread and once with several.
 */

#include "config.h"

#include "metadata/class-internals.h"
#include "metadata/object-internals.h"

#include <mono/metadata/appdomain.h>
#include <mono/metadata/assembly.h>
#include <mono/metadata/assembly-internals.h>
#include <mono/metadata/object.h>
#include <mono/utils/mono-error-internals.h>

#include <gtest/gtest.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <unordered_set>
#include <vector>

#include "harness.hpp"

extern "C" {
typedef struct _LivenessState LivenessState;
typedef void (*register_object_callback) (gpointer *arr, int size, void *callback_userdata);
typedef void (*WorldStateChanged) ();

LivenessState *mono_unity_liveness_calculation_begin (MonoClass *filter, guint max_count,
	register_object_callback callback, void *callback_userdata,
	WorldStateChanged onWorldStarted, WorldStateChanged onWorldStopped);
void mono_unity_liveness_calculation_end (LivenessState *state);
void mono_unity_liveness_calculation_from_statics (LivenessState *state);
void mono_unity_liveness_calculation_from_root (MonoObject *root, LivenessState *state);
}

namespace {

#define TESTPROG "unity-liveness.exe"

void collect (gpointer *arr, int size, void *userdata)
{
	auto *found = static_cast<std::vector<MonoObject *> *> (userdata);
	found->insert (found->end (), (MonoObject **) arr, (MonoObject **) arr + size);
}

class UnityLiveness : public ::testing::Test {
protected:
	MonoImage *image = nullptr;
	MonoClass *tracked = nullptr;

	void SetUp () override
	{
		MONO_SKIP_WITHOUT_CLASS_LIBRARY ();
		mono::test::init_runtime ();

		MonoDomain *domain = mono_domain_get ();
		MonoAssemblyOpenRequest req;
		mono_assembly_request_prepare_open (
			&req, MONO_ASMCTX_DEFAULT, mono_domain_default_alc (domain));

		MonoImageOpenStatus status;
		MonoAssembly *assembly = mono_assembly_request_open (TESTPROG, &req, &status);
		ASSERT_NE (nullptr, assembly) << "failed loading " TESTPROG;

		image = mono_assembly_get_image_internal (assembly);
		tracked = class_named ("Tracked");
		ASSERT_NE (nullptr, tracked);
	}

	void TearDown () override
	{
		if (image)
			invoke ("Drop", nullptr);
	}

	MonoClass *class_named (const char *name)
	{
		ERROR_DECL (error);
		MonoClass *klass = mono_class_from_name_checked (image, "", name, error);
		mono_error_assert_ok (error);
		return klass;
	}

	MonoObject *invoke (const char *method_name, int *arg)
	{
		ERROR_DECL (error);
		MonoMethod *method = mono_class_get_method_from_name_checked (
			class_named ("LivenessRoots"), method_name, arg ? 1 : 0, 0, error);
		mono_error_assert_ok (error);
		void *args[] = { arg };
		MonoObject *result = mono_runtime_invoke_checked (method, nullptr, args, error);
		mono_error_assert_ok (error);
		return result;
	}

	int invoke_int (const char *method_name, int arg)
	{
		return *(int *) mono_object_unbox_internal (invoke (method_name, &arg));
	}

	void expect_each_tracked_once (const std::vector<MonoObject *> &found, size_t expected)
	{
		EXPECT_EQ (expected, found.size ());
		std::unordered_set<MonoObject *> distinct (found.begin (), found.end ());
		EXPECT_EQ (found.size (), distinct.size ()) << "an object was reported twice";
		for (MonoObject *object : found) {
			ASSERT_EQ (0u, (gsize) object->vtable & 1) << "mark bit left set";
			ASSERT_EQ (tracked, mono_object_class (object));
		}
	}
};

TEST_F (UnityLiveness, FromStaticsReportsEachReachableObjectOnce)
{
	int expected = invoke_int ("Build", 200000);
	std::vector<MonoObject *> found;

	LivenessState *state = mono_unity_liveness_calculation_begin (
		tracked, 0, collect, &found, nullptr, nullptr);
	mono_unity_liveness_calculation_from_statics (state);
	mono_unity_liveness_calculation_end (state);

	expect_each_tracked_once (found, expected);
}

TEST_F (UnityLiveness, FromRootReportsOnlyWhatStaticsDidNot)
{
	int expected = invoke_int ("Build", 20000);
	int fresh = 1000;
	MonoObject *detached = invoke ("MakeDetached", &fresh);
	MonoGCHandle handle = mono_gchandle_new_internal (detached, FALSE);
	std::vector<MonoObject *> found;

	LivenessState *state = mono_unity_liveness_calculation_begin (
		tracked, 0, collect, &found, nullptr, nullptr);
	mono_unity_liveness_calculation_from_statics (state);
	size_t from_statics = found.size ();
	mono_unity_liveness_calculation_from_root (detached, state);
	mono_unity_liveness_calculation_end (state);
	mono_gchandle_free_internal (handle);

	EXPECT_EQ ((size_t) expected, from_statics);
	expect_each_tracked_once (found, expected + fresh);
}

TEST_F (UnityLiveness, ManySmallObjects)
{
	int expected = invoke_int ("BuildVessels", 20000);
	std::vector<MonoObject *> found;

	LivenessState *state = mono_unity_liveness_calculation_begin (
		tracked, 0, collect, &found, nullptr, nullptr);
	mono_unity_liveness_calculation_from_statics (state);
	mono_unity_liveness_calculation_end (state);

	expect_each_tracked_once (found, expected);
}

/*
 * Not a test: times from_statics over LIVENESS_BENCH_PARTS vessels. Run it
 * with --gtest_also_run_disabled_tests and MONO_UNITY_LIVENESS_THREADS set to
 * each thread count to compare.
 */
TEST_F (UnityLiveness, DISABLED_Benchmark)
{
	const char *parts = getenv ("LIVENESS_BENCH_PARTS");
	invoke_int ("BuildVessels", parts ? atoi (parts) : 200000);
	invoke_int ("Build", parts ? atoi (parts) : 200000);

	for (int run = 0; run < 5; run++) {
		std::vector<MonoObject *> found;
		found.reserve (16 * 1024 * 1024);
		LivenessState *state = mono_unity_liveness_calculation_begin (
			tracked, 0, collect, &found, nullptr, nullptr);
		auto start = std::chrono::steady_clock::now ();
		mono_unity_liveness_calculation_from_statics (state);
		auto walked = std::chrono::steady_clock::now ();
		mono_unity_liveness_calculation_end (state);
		auto ended = std::chrono::steady_clock::now ();
		printf ("from_statics %.2f ms, end %.2f ms, %zu objects\n",
			std::chrono::duration<double, std::milli> (walked - start).count (),
			std::chrono::duration<double, std::milli> (ended - walked).count (),
			found.size ());
	}
}

} // namespace

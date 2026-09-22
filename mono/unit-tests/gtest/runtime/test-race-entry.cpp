/* Regression tests for coalescing first-entry compilation and re-entry. */

#include "config.h"

#include "metadata/class-internals.h"
#include "metadata/domain-internals.h"
#include "metadata/object-internals.h"
#include "metadata/threads-types.h"
#include "mini/domain-method.h"
#include "mini/domain-method.hpp"

#include "llvm/runtime.h"

#include <mono/metadata/appdomain.h>
#include <mono/metadata/assembly.h>
#include <mono/metadata/assembly-internals.h>
#include <mono/metadata/object.h>
#include <mono/utils/mono-error-internals.h>

#include <gtest/gtest.h>

#include <atomic>
#include <thread>
#include <vector>

#include "harness.hpp"

namespace {

#define TESTPROG "race-entry.exe"

MonoImage *g_image;

class RaceEntry : public ::testing::Test {
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

	void SetUp () override { MONO_SKIP_WITHOUT_CLASS_LIBRARY (); }

protected:
	static MonoMethod *method_named_in (const char *klass_name, const char *name, int argc)
	{
		ERROR_DECL (error);
		MonoClass *klass = mono_class_from_name_checked (g_image, "", klass_name, error);

		mono_error_assert_ok (error);
		if (klass == nullptr)
			return nullptr;

		MonoMethod *found =
			mono_class_get_method_from_name_checked (klass, name, argc, 0, error);

		mono_error_assert_ok (error);
		return found;
	}

	static MonoMethod *method_named (const char *name, int argc)
	{
		return method_named_in ("RaceEntry", name, argc);
	}
};

} // namespace

TEST_F (RaceEntry, ConcurrentFirstCallsCoalesce)
{
	MonoMethod *caller = method_named ("Caller", 0);
	MonoMethod *callee = method_named ("Callee", 0);

	ASSERT_NE (nullptr, caller);
	ASSERT_NE (nullptr, callee);

	MonoDomain *domain = mono_domain_get ();
	constexpr int kThreads = 16;

	std::atomic<int> ready { 0 };
	std::atomic<bool> go { false };
	std::vector<std::thread> threads;

	for (int i = 0; i < kThreads; ++i) {
		threads.emplace_back ([&] {
			MonoThread *thread = mono_thread_internal_attach (domain);

			ready.fetch_add (1, std::memory_order_release);
			while (!go.load (std::memory_order_acquire)) {
			}

			ERROR_DECL (error);
			mono_runtime_invoke_checked (caller, nullptr, nullptr, error);
			mono_error_assert_ok (error);

			mono_thread_internal_detach (thread);
		});
	}

	while (ready.load (std::memory_order_acquire) < kThreads) {
	}
	go.store (true, std::memory_order_release);

	for (std::thread &t : threads)
		t.join ();

	mono::MonoDomainMethod *dm = mono::domain_method_find (domain, callee);

	ASSERT_NE (nullptr, dm);

	std::vector<mono::MonoMethodBody> bodies;
	dm->foreach_body ([&] (const mono::MonoMethodBody &body) { bodies.push_back (body); });

	EXPECT_EQ (1u, bodies.size ())
		<< kThreads << " threads racing Callee's first call published "
		<< bodies.size () << " separate bodies for it instead of coalescing "
		   "onto one compile";
}

TEST_F (RaceEntry, ReentrantCctorDuringFirstCompileDoesNotDeadlock)
{
	MonoMethod *caller = method_named_in ("ReentrantEntry", "Caller", 0);

	ASSERT_NE (nullptr, caller);

	ERROR_DECL (error);
	mono_runtime_invoke_checked (caller, nullptr, nullptr, error);
	mono_error_assert_ok (error);
}

TEST_F (RaceEntry, PublishedEntryIfReadyMatchesTheCompiledAddress)
{
	MonoMethod *target = method_named_in ("LookupFastPath", "Target", 0);

	ASSERT_NE (nullptr, target);

	MonoDomain *domain = mono_domain_get ();

	EXPECT_EQ (nullptr, mono::published_entry_if_ready (domain, target));

	ERROR_DECL (error);
	void *compiled = mono_llvm_jit_compile_method (target, domain, error);

	mono_error_assert_ok (error);
	ASSERT_NE (nullptr, compiled);

	EXPECT_EQ (compiled, mono::published_entry_if_ready (domain, target));
}

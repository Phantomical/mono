/*
 * test-unmanaged-callers-only.cpp: Unit test for a method [UnmanagedCallersOnly]
 * marks, whose one published entry native code calls.
 *
 * The convention is settled when the method is published rather than by the
 * caller that asks for the address: ldftn and
 * RuntimeMethodHandle.GetFunctionPointer () both hand out the one address the
 * method has. So the address has to be C-callable however it was asked for, and
 * managed code cannot call the method at all.
 */

#include "config.h"

#include "metadata/class-internals.h"
#include "metadata/metadata-internals.h"
#include "metadata/object-internals.h"
#include "mini/domain-method.h"
#include "mini/jit.h"

#include "llvm/runtime.h"

#include <mono/metadata/appdomain.h>
#include <mono/metadata/assembly.h>
#include <mono/metadata/assembly-internals.h>
#include <mono/metadata/object.h>
#include <mono/utils/mono-error-internals.h>

#include <gtest/gtest.h>

#include "harness.hpp"

namespace {

#define TESTPROG "unmanaged-callers-only.exe"

MonoImage *g_image;

class UnmanagedCallersOnly : public ::testing::Test {
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
	static MonoMethod *method_named (const char *name, int argc)
	{
		ERROR_DECL (error);
		MonoClass *klass =
			mono_class_from_name_checked (g_image, "", "UnmanagedCallers", error);

		mono_error_assert_ok (error);
		if (klass == nullptr)
			return nullptr;

		MonoMethod *found =
			mono_class_get_method_from_name_checked (klass, name, argc, 0, error);

		mono_error_assert_ok (error);
		return found;
	}

	/// The address the runtime hands native code for method, or null with the
	/// reason left on gtest's failure list.
	static void *entry_of (MonoMethod *method)
	{
		ERROR_DECL (error);
		void *entry = mono_compile_method_checked (method, error);

		EXPECT_TRUE (is_ok (error)) << mono_error_get_message (error);
		mono_error_cleanup (error);
		return entry;
	}

	/// The name of the class of the exception invoking method raises, or an
	/// empty string when it returns.
	static std::string raised_by (MonoMethod *method, void **args)
	{
		ERROR_DECL (error);
		MonoObject *thrown = nullptr;

		mono_runtime_try_invoke (method, nullptr, args, &thrown, error);
		mono_error_assert_ok (error);

		if (thrown == nullptr)
			return {};
		return m_class_get_name (mono_object_class (thrown));
	}

	/// Waits up to a second for a promotion to land, and answers the body or
	/// null.
	static void *await_body (MonoDomain *domain, MonoMethod *method)
	{
		for (int waited = 0; waited < 1000; ++waited) {
			if (void *body = mono_llvm_jit_find_body (domain, method))
				return body;
			g_usleep (1000);
		}
		return nullptr;
	}
};

} // namespace

/*
 * The address is entered as a C function. A call written this way passes its
 * arguments where the C convention puts them, so an entry published in this
 * engine's own convention reads them from the wrong places and answers a
 * value that is not the sum.
 */
TEST_F (UnmanagedCallersOnly, PublishesACEntry)
{
	MonoMethod *method = method_named ("Add", 2);

	ASSERT_NE (nullptr, method);

	auto *add = (int (*) (int, int)) entry_of (method);

	ASSERT_NE (nullptr, add);
	EXPECT_EQ (42, add (40, 2));
	EXPECT_EQ (-1, add (1, -2));
}

/*
 * One method, one address, however it was asked for. The two entry points
 * below are the ones the two engines reach: the icall behind
 * GetFunctionPointer () compiles, and the interpreter's ldftn asks for the
 * stub. A method whose answer depended on which of them ran would hand a
 * delegate built in one engine an address the other cannot call.
 */
TEST_F (UnmanagedCallersOnly, HasOneAddress)
{
	ERROR_DECL (error);
	MonoMethod *method = method_named ("Add", 2);

	ASSERT_NE (nullptr, method);

	void *compiled = entry_of (method);
	void *stub = mono_llvm_jit_stub_for (method, mono_domain_get (), error);

	mono_error_assert_ok (error);
	ASSERT_NE (nullptr, compiled);
	EXPECT_EQ (compiled, stub);
	EXPECT_EQ (compiled, entry_of (method));
}

/* Two methods get two addresses, so a patcher or a caller holding one reaches
 * the method it asked for. */
TEST_F (UnmanagedCallersOnly, IsDistinctPerMethod)
{
	MonoMethod *add = method_named ("Add", 2);
	MonoMethod *subtract = method_named ("Subtract", 2);

	ASSERT_NE (nullptr, add);
	ASSERT_NE (nullptr, subtract);

	auto *add_entry = (int (*) (int, int)) entry_of (add);
	auto *subtract_entry = (int (*) (int, int)) entry_of (subtract);

	ASSERT_NE (nullptr, add_entry);
	ASSERT_NE (nullptr, subtract_entry);
	EXPECT_NE ((void *) add_entry, (void *) subtract_entry);
	EXPECT_EQ (7, add_entry (5, 2));
	EXPECT_EQ (3, subtract_entry (5, 2));
}

/*
 * Both engines, in one case. The promotion is one way only, so the tier-0 arm
 * has to run before it; splitting the two would leave the order to gtest.
 */
TEST_F (UnmanagedCallersOnly, RefusesAManagedCallInBothEngines)
{
	int x = 40;
	int y = 2;
	void *args[2] = { &x, &y };
	MonoDomain *domain = mono_domain_get ();
	MonoMethod *caller = method_named ("CallAdd", 2);
	MonoMethod *plain = method_named ("CallPlain", 2);

	ASSERT_NE (nullptr, caller);
	ASSERT_NE (nullptr, plain);

	/* The same call shape against a method with no attribute. Without this a
	 * refusal of every static call passes the case. */
	EXPECT_EQ ("", raised_by (plain, args));

	if (mono_llvm_jit_tier0_enabled ())
		EXPECT_EQ ("NotSupportedException", raised_by (caller, args))
			<< "the interpreter's transform let the call through";

	ASSERT_TRUE (mono_promote_method (caller, domain));
	ASSERT_NE (nullptr, await_body (domain, caller))
		<< "the promotion never produced a body, so the translator ran on nothing";

	EXPECT_EQ ("NotSupportedException", raised_by (caller, args))
		<< "the translator let the call through";
}

/*
 * A delegate has no entry to bind. Invoke would call the method's one address
 * in this engine's own convention, and that address is in the C convention.
 */
TEST_F (UnmanagedCallersOnly, RefusesADelegate)
{
	MonoMethod *method = method_named ("MakeDelegate", 0);

	ASSERT_NE (nullptr, method);
	EXPECT_EQ ("NotSupportedException", raised_by (method, nullptr));
}

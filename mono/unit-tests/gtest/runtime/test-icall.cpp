/*
 * test-icall.cpp: Unit test for an internal call registered with no wrapper.
 *
 * mono_dangerous_add_internal_call_no_wrapper () gives a method a C function as
 * its whole implementation. The runtime publishes that address as the method's
 * entry, so the backend names the C function at the call site rather than the
 * thunk every other method is called through. No record and no thunk are built
 * for such a method, which is what the record case below asserts.
 *
 * The same registration promises that the function does not throw or raise. The
 * backend takes the promise as nounwind, so a call to one of these needs no
 * landing pad, and the two try cases hold the shapes that follow from it.
 *
 * Nothing in this tree registers an icall this way. The runtime keeps the
 * registration for an embedder, which is what these cases stand in for.
 */

#include "config.h"

#include "metadata/class-internals.h"
#include "metadata/icall-internals.h"
#include "metadata/metadata-internals.h"
#include "metadata/object-internals.h"
#include "mini/domain-method.h"
#include "mini/domain-method.hpp"
#include "mini/jit.h"
#include "mini/mini.h"

#include "llvm/runtime.h"

#include <mono/metadata/appdomain.h>
#include <mono/metadata/assembly.h>
#include <mono/metadata/assembly-internals.h>
#include <mono/metadata/object.h>
#include <mono/utils/mono-error-internals.h>

#include <gtest/gtest.h>

#include "harness.hpp"

namespace {

#define TESTPROG "icall.exe"

/* What Icalls::Plain runs. Registered with no wrapper, so this address is the
 * method's published entry and compiled code names it directly. */
extern "C" int
icall_plain_body (int x)
{
	return x + 100;
}

/* What Icalls::Wrapped runs. Registered the ordinary way, so the runtime builds
 * a marshalling wrapper and the entry is that wrapper's. */
extern "C" int
icall_wrapped_body (int x)
{
	return x + 200;
}

MonoImage *g_image;

class NoWrapperIcall : public ::testing::Test {
public:
	static void SetUpTestSuite ()
	{
		MONO_SKIP_WITHOUT_CLASS_LIBRARY ();

		mono::test::init_runtime ();

		/*
		 * After the runtime starts, since mono_icall_init () is what builds the
		 * table these go into, and before the assembly below is loaded. A caller
		 * compiled against an unregistered name decides for the wrapper, and it
		 * decides once.
		 */
		mono_dangerous_add_internal_call_no_wrapper ("Icalls::Plain",
		                                             (const void *) icall_plain_body);
		mono_add_internal_call_internal ("Icalls::Wrapped",
		                                 (const void *) icall_wrapped_body);

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
			mono_class_from_name_checked (g_image, "", "Icalls", error);

		mono_error_assert_ok (error);
		if (klass == nullptr)
			return nullptr;

		MonoMethod *found =
			mono_class_get_method_from_name_checked (klass, name, argc, 0, error);

		mono_error_assert_ok (error);
		return found;
	}

	/*
	 * The address \p method is entered at once a promotion has given it a body.
	 *
	 * Every case here has to run compiled code: an interpreted caller reaches
	 * its callees for itself, so it would answer for the interpreter rather
	 * than for the backend. This goes through the compile queue, which is how a
	 * method that is merely called gets a body.
	 */
	static void *compiled_entry (MonoMethod *method)
	{
		ERROR_DECL (error);
		MonoDomain *domain = mono_domain_get ();
		void *entry = mono_compile_method_checked (method, error);

		mono_error_assert_ok (error);
		EXPECT_NE (nullptr, entry);

		if (!mono_promote_method (method, domain))
			return nullptr;

		for (int waited = 0; waited < 1000; ++waited) {
			if (mono_llvm_jit_find_body (domain, method) != nullptr)
				return entry;
			g_usleep (1000);
		}
		return nullptr;
	}
};

} // namespace

/*
 * A compiled caller reaches the registered C function and gets its answer. This
 * is the arm every case below rests on: without it a wrong binding and a
 * refused compile read the same.
 */
TEST_F (NoWrapperIcall, ACompiledCallerReachesTheCFunction)
{
	MonoMethod *caller = method_named ("CallPlain", 1);

	ASSERT_NE (nullptr, caller);

	int (*entry) (int) = (int (*) (int)) compiled_entry (caller);

	ASSERT_NE (nullptr, entry) << "the caller never got a compiled body";
	EXPECT_EQ (101, entry (1));
}

/*
 * An icall registered the ordinary way still answers through the wrapper the
 * runtime builds for it. The two registrations take different paths through
 * icall_wrapper_target (), and only this arm exercises the wrapper one.
 */
TEST_F (NoWrapperIcall, AWrappedIcallStillAnswers)
{
	MonoMethod *caller = method_named ("CallWrapped", 1);

	ASSERT_NE (nullptr, caller);

	int (*entry) (int) = (int (*) (int)) compiled_entry (caller);

	ASSERT_NE (nullptr, entry) << "the caller never got a compiled body";
	EXPECT_EQ (201, entry (1));
}

/*
 * Calling the icall from compiled code builds no record for it, because the
 * call site names the C function rather than an entry the domain publishes.
 *
 * The managed callee is the control. It is called the same way, from a body
 * compiled the same way, and it does get a record - so a null answer here says
 * the icall took the other path, not that nothing ran.
 */
TEST_F (NoWrapperIcall, BuildsNoRecordForTheIcall)
{
	MonoDomain *domain = mono_domain_get ();
	MonoMethod *icall = method_named ("Plain", 1);
	MonoMethod *managed = method_named ("Managed", 1);
	MonoMethod *calls_icall = method_named ("CallPlain", 1);
	MonoMethod *calls_managed = method_named ("CallManaged", 1);

	ASSERT_NE (nullptr, icall);
	ASSERT_NE (nullptr, managed);
	ASSERT_NE (nullptr, calls_icall);
	ASSERT_NE (nullptr, calls_managed);

	int (*through_icall) (int) = (int (*) (int)) compiled_entry (calls_icall);
	int (*through_managed) (int) = (int (*) (int)) compiled_entry (calls_managed);

	ASSERT_NE (nullptr, through_icall) << "the caller never got a compiled body";
	ASSERT_NE (nullptr, through_managed) << "the caller never got a compiled body";
	ASSERT_EQ (101, through_icall (1));
	ASSERT_EQ (8, through_managed (1));

	EXPECT_NE (nullptr, mono::domain_method_find (domain, managed))
		<< "the control callee was folded in, so this case checks nothing";
	EXPECT_EQ (nullptr, mono::domain_method_find (domain, icall))
		<< "the icall was published as an entry rather than named as a C function";
}

/*
 * A try region whose only call is the icall still runs. Nothing in it can
 * unwind once the icall is nounwind, so the method reaches codegen with no
 * landing pad, and MonoEHGatherPass writes an empty clause table for it.
 */
TEST_F (NoWrapperIcall, RunsInsideATryThatHoldsNothingElse)
{
	MonoMethod *caller = method_named ("PlainInTry", 1);

	ASSERT_NE (nullptr, caller);

	int (*entry) (int) = (int (*) (int)) compiled_entry (caller);

	ASSERT_NE (nullptr, entry) << "the caller never got a compiled body";
	EXPECT_EQ (101, entry (1));
}

/*
 * A try region that also holds a bounds check keeps its clause. The icall
 * contributes no protected range any more, so the range the clause is written
 * from is the bounds check's alone - and the catch has to still run.
 */
TEST_F (NoWrapperIcall, RunsInsideATryThatStillCatches)
{
	ERROR_DECL (error);
	MonoMethod *caller = method_named ("PlainInTryWithThrow", 2);

	ASSERT_NE (nullptr, caller);

	int (*entry) (int, MonoArray *) =
		(int (*) (int, MonoArray *)) compiled_entry (caller);

	ASSERT_NE (nullptr, entry) << "the caller never got a compiled body";

	MonoArray *wide = mono_array_new_checked (mono_domain_get (),
	                                          mono_get_int32_class (), 5, error);
	mono_error_assert_ok (error);
	ASSERT_NE (nullptr, wide);
	mono_array_set_internal (wide, gint32, 4, 11);

	MonoArray *narrow = mono_array_new_checked (mono_domain_get (),
	                                            mono_get_int32_class (), 1, error);
	mono_error_assert_ok (error);
	ASSERT_NE (nullptr, narrow);

	EXPECT_EQ (112, entry (1, wide));
	EXPECT_EQ (-1, entry (1, narrow)) << "the clause lost the bounds check's range";
}

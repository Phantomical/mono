/*
 * Exercises the pinned-target excuse check_remset_consistency () gives a
 * missing remset, and MONO_SGEN_STRICT_REMSET_CHECK's narrowing of it
 * (sgen-debug.c).
 *
 * Managed code cannot produce an un-barriered reference store, because the
 * JIT always emits the barrier. This test makes one directly instead: a raw
 * C store into a major-heap holder's field, with no mono_gc_wbarrier_* call.
 *
 * This is its own binary rather than a case in test-mono-runtime.
 * check-remset-consistency reads MONO_GC_DEBUG once, at GC init, for the
 * whole process, and MONO_SGEN_STRICT_REMSET_CHECK aborts the process on a
 * confirmed miss. check-remset-missing.sh reads the checker's report, or its
 * absence, off the test's own output instead of anything asserted here past
 * the second collection.
 */

#include "config.h"

#define HAVE_SGEN_GC

#include "metadata/class-internals.h"
#include "metadata/domain-internals.h"
#include "metadata/object-internals.h"

#include <mono/metadata/appdomain.h>
#include <mono/metadata/assembly.h>
#include <mono/metadata/assembly-internals.h>
#include <mono/metadata/mono-gc.h>
#include <mono/metadata/object.h>
#include <mono/utils/mono-error-internals.h>

#include <mono/sgen/sgen-gc.h>

#include <gtest/gtest.h>

#include <cstdio>
#include <string>

#include "harness.hpp"

namespace {

#define TESTPROG "remset-missing.exe"

TEST (RemsetMissing, PinnedTargetExcuseNeedsConfirming)
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

	MonoImage *image = mono_assembly_get_image_internal (assembly);

	ERROR_DECL (error);
	MonoClass *klass = mono_class_from_name_checked (image, "", "RemsetHolder", error);
	mono_error_assert_ok (error);
	ASSERT_NE (nullptr, klass);

	MonoClassField *field = mono_class_get_field_from_name_full (klass, "Slot", nullptr);
	ASSERT_NE (nullptr, field);
	uint32_t offset = mono_field_get_offset (field);

	MonoVTable *vtable = mono_class_vtable_checked (domain, klass, error);
	mono_error_assert_ok (error);
	ASSERT_NE (nullptr, vtable);

	/* Major heap directly, so its address stays valid across the collections
	 * below -- a promoted nursery object would move under it. */
	MonoObject *holder = mono_object_new_mature (vtable, error);
	mono_error_assert_ok (error);
	ASSERT_NE (nullptr, holder);
	ASSERT_FALSE (sgen_ptr_in_nursery ((char *) holder));

	MonoObject *target = mono_object_new_checked (domain, mono_get_object_class (), error);
	mono_error_assert_ok (error);
	ASSERT_NE (nullptr, target);
	ASSERT_TRUE (sgen_ptr_in_nursery ((char *) target));

	/* Pinned, because the excuse under test only applies to a pinned target --
	 * an unpinned miss would report at its first sighting either way. */
	MonoGCHandle handle = mono_gchandle_new_internal (target, TRUE);

	/* The missing barrier: field->offset already includes the MonoObject
	 * header, so this is the field's address outright. */
	*(MonoObject **) ((char *) holder + offset) = target;

	/*
	 * Redirected so the test can read the first collection's own report.
	 * That is what proves the miss was seen as pinned, not as an ordinary
	 * one. Restored before the second collection: its report, if
	 * MONO_SGEN_STRICT_REMSET_CHECK confirms the miss, has to reach the
	 * process's stderr, where check-remset-missing.sh reads it back.
	 */
	char *captured = nullptr;
	size_t captured_size = 0;
	FILE *capture = open_memstream (&captured, &captured_size);
	FILE *real_debug_file = sgen_gc_debug_file;

	sgen_gc_debug_file = capture;
	mono_gc_collect (0);
	fflush (capture);
	sgen_gc_debug_file = real_debug_file;

	std::string first_report (captured, captured_size);
	fclose (capture);
	free (captured);

#ifdef SGEN_STRICT_REMSET_CHECK
	EXPECT_EQ (std::string::npos, first_report.find ("not found in remsets"))
		<< "an unconfirmed sighting should stay silent under the option";
#else
	EXPECT_NE (std::string::npos, first_report.find ("not found in remsets"))
		<< "the checker logs an excused miss even though it never aborts";
	EXPECT_NE (std::string::npos, first_report.find ("but object is pinned"))
		<< "the excuse text should name the pin";
#endif

	ASSERT_TRUE (sgen_ptr_in_nursery ((char *) target))
		<< "the target left the nursery, so the second collection checks nothing";

	/* Strictly later than the first collection, which is what confirms the
	 * miss under MONO_SGEN_STRICT_REMSET_CHECK. */
	mono_gc_collect (0);

	EXPECT_EQ (target, *(MonoObject **) ((char *) holder + offset));

	mono_gchandle_free_internal (handle);
}

} // namespace

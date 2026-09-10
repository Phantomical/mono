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

#include <glib.h>

#ifdef HOST_WIN32
/* fdopen (), which the CRT declares here rather than in unistd.h. */
#include <io.h>
#else
#include <unistd.h>
#endif

#include <cstdio>
#include <string>

#include "harness.hpp"

namespace {

#define TESTPROG "remset-missing.exe"

/// One capture of what the collector wrote to sgen_gc_debug_file.
class DebugFileCapture {
public:
	DebugFileCapture ()
	{
		int fd = g_file_open_tmp ("sgen-remset-XXXXXX", &path_, nullptr);
		if (fd < 0)
			return;
		file_ = fdopen (fd, "w+");
	}

	~DebugFileCapture ()
	{
		if (file_ != nullptr)
			fclose (file_);
		if (path_ != nullptr) {
			g_unlink (path_);
			g_free (path_);
		}
	}

	FILE *file () const { return file_; }

	/// What has been written so far, with the write position left at the end.
	std::string text () const
	{
		if (file_ == nullptr)
			return std::string ();

		fflush (file_);

		long end = ftell (file_);
		if (end <= 0)
			return std::string ();

		std::string out ((size_t) end, '\0');
		rewind (file_);
		size_t got = fread (&out[0], 1, (size_t) end, file_);
		out.resize (got);
		fseek (file_, 0, SEEK_END);
		return out;
	}

private:
	FILE *file_ = nullptr;
	char *path_ = nullptr;
};

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
	DebugFileCapture capture;
	ASSERT_NE (nullptr, capture.file ()) << "failed opening the capture file";

	FILE *real_debug_file = sgen_gc_debug_file;

	sgen_gc_debug_file = capture.file ();
	mono_gc_collect (0);
	sgen_gc_debug_file = real_debug_file;

	std::string first_report = capture.text ();

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

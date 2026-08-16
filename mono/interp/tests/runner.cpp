/*
 * Runs one test method out of a managed assembly.
 *
 * A test is a static method that takes nothing, returns int, and is named
 * test_<expected>_<what> -- the convention mono/mini's --regression harness
 * reads. It passes when it returns <expected>.
 *
 *   runner --list <assembly>              every test in it, one "Class:name" a line
 *   runner <assembly> <Class:name>        run that one
 *
 * A listing line carries the arms the test's class opted into after a space,
 * and the first line names every arm this runner knows.
 *
 * One method per process, which is what lets a test that takes the runtime down
 * name itself. Which engine runs the method is the caller's business: the
 * MONO_LLVM_JIT_TIER* variables decide it, and the suites pass them in.
 */

#include "config.h"

#include <glib.h>

#include <mono/metadata/appdomain.h>
#include <mono/metadata/assembly-internals.h>
#include <mono/metadata/class-internals.h>
#include <mono/metadata/debug-helpers.h>
#include <mono/metadata/domain-internals.h>
#include <mono/metadata/exception.h>
#include <mono/metadata/image-internals.h>
#include <mono/metadata/loader.h>
#include <mono/metadata/mono-debug.h>
#include <mono/metadata/profiler.h>
#include <mono/metadata/object-internals.h>
#include <mono/metadata/reflection-internals.h>
#include <mono/metadata/tabledefs.h>
#include <mono/metadata/tokentype.h>
#include <mono/mini/jit.h>
#include <mono/mini/mini-runtime.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

namespace {

/// The value a test's name says it returns, or -1 if the name does not carry one.
///
/// Only the digits right after "test_" count. A method called test_thing is not
/// a test, and saying so here keeps it out of the listing rather than turning it
/// into a case that can never pass.
int
expected_result (const char *name)
{
	if (strncmp (name, "test_", 5) != 0 || !g_ascii_isdigit (name [5]))
		return -1;

	return atoi (name + 5);
}

bool
is_test_method (MonoMethod *method)
{
	if (expected_result (method->name) < 0)
		return false;
	if ((method->flags & METHOD_ATTRIBUTE_STATIC) == 0)
		return false;

	MonoMethodSignature *signature = mono_method_signature_internal (method);

	return signature != nullptr && signature->param_count == 0 &&
	       signature->ret->type == MONO_TYPE_I4;
}

/// What each attribute opts a class into, spelled as the suites spell the arm.
///
/// The match is on the attribute's name alone, so a suite that has to be an
/// assembly of its own defines the attribute again rather than referencing the
/// one in cs/arms.cs.
const struct {
	std::string_view attribute;
	const char *arm;
} arm_attributes [] = {
	{ "NoOptAttribute",        "noopt" },
	{ "InstrumentedAttribute", "instrumented" },
};

/// The arms a class opted into, comma separated, or empty for none.
std::string
class_arms (MonoClass *klass)
{
	ERROR_DECL (error);
	MonoCustomAttrInfo *attributes = mono_custom_attrs_from_class_checked (klass, error);
	std::string arms;

	// A class whose attributes will not load opts into nothing, and stays in the
	// arms that take every test.
	mono_error_cleanup (error);

	if (attributes == nullptr)
		return arms;

	for (int i = 0; i < attributes->num_attrs; i++) {
		MonoMethod *ctor = attributes->attrs [i].ctor;

		if (ctor == nullptr)
			continue;

		for (const auto &known : arm_attributes) {
			if (known.attribute != m_class_get_name (ctor->klass))
				continue;

			if (!arms.empty ())
				arms += ',';
			arms += known.arm;
		}
	}

	mono_custom_attrs_free (attributes);
	return arms;
}

/// How mono_method_desc_new () spells this method: "Namespace.Class:name".
std::string
qualified_name (MonoMethod *method)
{
	const char *space = m_class_get_name_space (method->klass);
	std::string name;

	if (space != nullptr && space [0] != '\0') {
		name += space;
		name += '.';
	}

	name += m_class_get_name (method->klass);
	name += ':';
	name += method->name;
	return name;
}

MonoImage *
open_assembly (const char *path)
{
	MonoImageOpenStatus status = MONO_IMAGE_OK;
	MonoAssemblyOpenRequest request;

	mono_assembly_request_prepare_open (&request, MONO_ASMCTX_DEFAULT,
	                                    mono_domain_default_alc (mono_domain_get ()));

	MonoAssembly *assembly = mono_assembly_request_open (path, &request, &status);

	if (assembly == nullptr || status != MONO_IMAGE_OK) {
		fprintf (stderr, "cannot open %s (status %d)\n", path, (int) status);
		return nullptr;
	}

	return mono_assembly_get_image_internal (assembly);
}

/// Every image a test method can be in: the one carrying the manifest, then one
/// per row of its File table.
///
/// An assembly linked out of netmodules keeps its manifest in a module of its
/// own, whose MethodDef table is empty. A module that will not load is left out
/// rather than reported, since a test in it simply does not appear.
std::vector<MonoImage *>
assembly_images (MonoImage *manifest)
{
	std::vector<MonoImage *> images { manifest };
	int files = mono_image_get_table_rows (manifest, MONO_TABLE_FILE);

	for (int i = 1; i <= files; i++) {
		ERROR_DECL (error);
		MonoImage *module = mono_image_load_file_for_image_checked (manifest, i, error);

		if (module == nullptr) {
			mono_error_cleanup (error);
			continue;
		}

		/*
		 * The assembly load hook opens symbols for the image carrying the
		 * manifest and for no other, so a module's .mdb is never read. Without
		 * it the transform emits no sequence point, and a suite run with
		 * MONO_DEBUG=gen-seq-points passes while testing nothing.
		 */
		mono_debug_open_image_from_memory (module, NULL, 0);
		images.push_back (module);
	}

	return images;
}

int
list_tests (MonoImage *manifest)
{
	// Named so that an arm asking for something no attribute produces is a
	// failure rather than an arm that quietly holds no tests.
	std::string known;

	for (const auto &arm : arm_attributes) {
		if (!known.empty ())
			known += ',';
		known += arm.arm;
	}

	printf ("#arms %s\n", known.c_str ());

	for (MonoImage *image : assembly_images (manifest)) {
		int rows = mono_image_get_table_rows (image, MONO_TABLE_METHOD);

		for (int i = 0; i < rows; i++) {
			ERROR_DECL (error);
			MonoMethod *method =
				mono_get_method_checked (image, MONO_TOKEN_METHOD_DEF | (i + 1), NULL, NULL, error);

			if (method == nullptr) {
				mono_error_cleanup (error);
				continue;
			}

			if (!is_test_method (method))
				continue;

			std::string arms = class_arms (method->klass);

			printf ("%s%s%s\n", qualified_name (method).c_str (),
			        arms.empty () ? "" : " ", arms.c_str ());
		}
	}

	return 0;
}

int
run_test (MonoImage *manifest, const char *name)
{
	MonoMethodDesc *desc = mono_method_desc_new (name, TRUE);
	MonoMethod *method = nullptr;

	for (MonoImage *image : assembly_images (manifest)) {
		method = mono_method_desc_search_in_image (desc, image);
		if (method != nullptr)
			break;
	}

	mono_method_desc_free (desc);

	if (method == nullptr || !is_test_method (method)) {
		fprintf (stderr, "no test named %s\n", name);
		return 2;
	}

	ERROR_DECL (error);
	MonoObject *exception = nullptr;
	MonoObject *result = mono_runtime_try_invoke (method, NULL, NULL, &exception, error);

	if (!is_ok (error)) {
		fprintf (stderr, "%s could not be invoked: %s\n", name, mono_error_get_message (error));
		mono_error_cleanup (error);
		return 3;
	}

	if (exception != nullptr) {
		MonoString *message = ((MonoException *) exception)->message;
		char *text = message ? mono_string_to_utf8_checked_internal (message, error) : NULL;

		fprintf (stderr, "%s threw %s: %s\n", name,
		         m_class_get_name (mono_object_class (exception)),
		         text ? text : "(no message)");
		mono_error_cleanup (error);
		g_free (text);
		return 1;
	}

	int expected = expected_result (method->name);
	int got = *(int *) mono_object_unbox_internal (result);

	if (got != expected) {
		fprintf (stderr, "%s returned %d, its name says %d\n", name, got, expected);
		return 1;
	}

	return 0;
}

} // namespace

int
main (int argc, char *argv [])
{
	bool listing = argc > 1 && strcmp (argv [1], "--list") == 0;
	int first = listing ? 2 : 1;

	if (argc < first + 1 || (!listing && argc < 3)) {
		fprintf (stderr, "usage: %s --list <assembly>\n       %s <assembly> <Class:name>\n",
		         argv [0], argv [0]);
		return 2;
	}

	mono_set_assemblies_path (MONO_INTERP_TESTS_ASSEMBLIES);
	/*
	 * Symbols, so that a suite run with MONO_DEBUG=gen-seq-points gets sequence
	 * points: the transform emits them only for a method the debug subsystem can
	 * find line numbers for. `mono` does this when it is given --debug, and
	 * nothing in an embedded start does it.
	 */
	mono_debug_init (MONO_DEBUG_FORMAT_MONO);

	/*
	 * Nothing that prints may be on while listing: the caller reads the method
	 * names off stdout, and the transform's tracing goes to the same place.
	 */
	if (listing) {
		g_unsetenv ("MONO_VERBOSE_METHOD");
		g_unsetenv ("MONO_INTERP_TRACE");
	}

	/*
	 * `mono` takes --trace= on its command line and an embedded start has no
	 * command line, so the tracing the interpreter emits around a call has no
	 * other way to be turned on.
	 */
	if (const char *trace = listing ? nullptr : g_getenv ("MONO_INTERP_TESTS_TRACE"))
		mono_jit_set_trace_options (trace);

	/*
	 * The same for the interpreter's own options, which `mono` takes as
	 * --interp=<opts>. "-all" turns off inlining, constant propagation, the
	 * super instructions and the basic block merging, so the transform emits
	 * what it read rather than what it worked out.
	 */
	if (const char *opts = g_getenv ("MONO_INTERP_TESTS_OPTS"))
		mono_interp_opts_string = opts;

	/*
	 * Coverage instrumentation, which the transform emits per basic block and
	 * which otherwise needs a profiler module. The filter says yes to
	 * everything: what is under test is the instrumentation, not a report.
	 */
	if (!listing && g_getenv ("MONO_INTERP_TESTS_COVERAGE") &&
	    mono_profiler_enable_coverage ()) {
		MonoProfilerHandle handle = mono_profiler_create (nullptr);

		mono_profiler_set_coverage_filter_callback (
			handle, [] (MonoProfiler *, MonoMethod *) -> mono_bool { return TRUE; });
	}

	mono_jit_init_version_for_test_only ("mono-interp-tests", "v4.0.30319");

	MonoImage *image = open_assembly (argv [first]);

	if (image == nullptr)
		return 2;

	return listing ? list_tests (image) : run_test (image, argv [first + 1]);
}

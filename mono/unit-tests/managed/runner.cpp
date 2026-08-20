/*
 * Runs one test method out of a managed assembly.
 *
 * A test is a static method that takes nothing, returns int, and is named
 * test_<expected>_<what> -- the convention mono/mini's --regression harness
 * reads. It passes when it returns <expected>.
 *
 *   runner --list <assembly>              every test in it, one "Class:name" a line
 *   runner <assembly> <Class:name>        run that one
 *   runner --all <assembly> [filters]     run every test it selects, in turn
 *
 * A listing line carries the arms the test's class opted into after a space,
 * and the first line names every arm this runner knows.
 *
 * One method per process, which is what lets a test that takes the runtime down
 * name itself. --all gives that up to start the runtime once for a whole
 * assembly, and takes over the selection the caller does with the listing:
 * --only <regex> and --arm <name> narrow what runs, and --xfail <Class:name>
 * names a test that has to fail. Which engine runs the method is the caller's
 * business: the MONO_LLVM_JIT_TIER* variables decide it, and the suites pass
 * them in.
 *
 * --skip <Class:name> drops a test that ends the process rather than answering,
 * which one process per test survives and a shared one does not. Each one is
 * printed and counted, so the run says what it did not cover.
 */

#include "config.h"

#include <glib.h>

#include <mono/llvm/runtime.h>
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

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <regex>
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
	if (strncmp (name, "test_", 5) != 0 || !g_ascii_isdigit (name[5]))
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

	return signature != nullptr && signature->param_count == 0
	       && signature->ret->type == MONO_TYPE_I4;
}

/// What each attribute opts a class into, spelled as the suites spell the arm.
///
/// The match is on the attribute's name alone, so a suite that has to be an
/// assembly of its own defines the attribute again rather than referencing the
/// one in arms.cs.
const struct {
	std::string_view attribute;
	const char *arm;
} arm_attributes[] = {
	{"NoOptAttribute", "noopt"},
	{"InstrumentedAttribute", "instrumented"},
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
		MonoMethod *ctor = attributes->attrs[i].ctor;

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

	if (space != nullptr && space[0] != '\0') {
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
	std::vector<MonoImage *> images{manifest};
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

struct TestMethod {
	MonoMethod *method;
	std::string name;
	std::string arms;
};

/// Every test method in the assembly, manifest first and then module by module.
std::vector<TestMethod>
collect_tests (MonoImage *manifest)
{
	std::vector<TestMethod> tests;

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

			tests.push_back ({method, qualified_name (method), class_arms (method->klass)});
		}
	}

	return tests;
}

/// The arms this runner knows, comma separated.
std::string
known_arms ()
{
	std::string known;

	for (const auto &arm : arm_attributes) {
		if (!known.empty ())
			known += ',';
		known += arm.arm;
	}

	return known;
}

bool
in_arm (const std::string &arms, const char *arm)
{
	return ("," + arms + ",").find ("," + std::string (arm) + ",") != std::string::npos;
}

int
list_tests (MonoImage *manifest)
{
	// Named so that an arm asking for something no attribute produces is a
	// failure rather than an arm that quietly holds no tests.
	printf ("#arms %s\n", known_arms ().c_str ());

	for (const TestMethod &test : collect_tests (manifest)) {
		printf ("%s%s%s\n", test.name.c_str (), test.arms.empty () ? "" : " ",
		        test.arms.c_str ());
	}

	return 0;
}

int
invoke_test (MonoMethod *method, const char *name)
{
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
		         m_class_get_name (mono_object_class (exception)), text ? text : "(no message)");
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

	return invoke_test (method, name);
}

/// Runs every test the filters select, and answers non-zero if any of them did
/// something other than what the caller expects.
///
/// only takes the tests whose "Class:name" it matches, arm takes the tests whose
/// class opted into it, xfail names the tests that have to fail, and skip names
/// the tests that do not run at all. Selecting nothing is not an error: an arm no
/// class in this assembly opted into reports zero tests and passes.
int
run_all (MonoImage *manifest, const char *only, const char *arm,
         const std::vector<std::string> &xfail,
         const std::vector<std::string> &skip)
{
	if (arm != nullptr && !in_arm (known_arms (), arm)) {
		fprintf (stderr, "no arm named %s; this runner knows %s\n", arm,
		         known_arms ().c_str ());
		return 2;
	}

	std::regex pattern;

	if (only != nullptr) {
		try {
			pattern.assign (only);
		} catch (const std::regex_error &bad) {
			fprintf (stderr, "cannot read %s as a regex: %s\n", only, bad.what ());
			return 2;
		}
	}

	int ran = 0;
	int wrong = 0;
	int skipped = 0;

	for (const TestMethod &test : collect_tests (manifest)) {
		if (only != nullptr && !std::regex_search (test.name, pattern))
			continue;
		if (arm != nullptr && !in_arm (test.arms, arm))
			continue;

		if (std::find (skip.begin (), skip.end (), test.name) != skip.end ()) {
			printf ("%s ... skipped\n", test.name.c_str ());
			skipped++;
			continue;
		}

		// The name goes out before the test runs, so a test that takes the
		// process down leaves its own name as the last line of the output.
		printf ("%s ... ", test.name.c_str ());
		fflush (stdout);

		bool passed = invoke_test (test.method, test.name.c_str ()) == 0;
		bool wanted = std::find (xfail.begin (), xfail.end (), test.name) == xfail.end ();

		ran++;
		if (passed != wanted)
			wrong++;

		if (passed)
			printf ("%s\n", wanted ? "ok" : "passed, and is expected to fail");
		else
			printf ("%s\n", wanted ? "FAILED" : "failed as expected");
		fflush (stdout);
	}

	printf ("%d tests, %d of them wrong, %d skipped\n", ran, wrong, skipped);
	return wrong == 0 ? 0 : 1;
}

} // namespace

int
main (int argc, char *argv[])
{
	const char *assembly = nullptr;
	const char *test = nullptr;
	const char *only = nullptr;
	const char *arm = nullptr;
	std::vector<std::string> xfail;
	std::vector<std::string> skip;
	bool listing = false;
	bool all = false;
	int i = 1;

	if (i < argc && strcmp (argv[i], "--list") == 0) {
		listing = true;
		i++;
	} else if (i < argc && strcmp (argv[i], "--all") == 0) {
		all = true;
		i++;
	}

	if (i < argc)
		assembly = argv[i++];

	for (; i < argc; i++) {
		const char *value = i + 1 < argc ? argv[i + 1] : nullptr;

		if (all && value != nullptr && strcmp (argv[i], "--only") == 0)
			only = argv[++i];
		else if (all && value != nullptr && strcmp (argv[i], "--arm") == 0)
			arm = argv[++i];
		else if (all && value != nullptr && strcmp (argv[i], "--xfail") == 0)
			xfail.push_back (argv[++i]);
		else if (all && value != nullptr && strcmp (argv[i], "--skip") == 0)
			skip.push_back (argv[++i]);
		else if (!all && !listing && test == nullptr)
			test = argv[i];
		else
			break;
	}

	if (assembly == nullptr || i != argc || (!listing && !all && test == nullptr)) {
		fprintf (stderr,
		         "usage: %s --list <assembly>\n"
		         "       %s <assembly> <Class:name>\n"
		         "       %s --all <assembly> [--only <regex>] [--arm <name>]"
		         " [--xfail <Class:name>]... [--skip <Class:name>]...\n",
		         argv[0], argv[0], argv[0]);
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
	if (!listing && g_getenv ("MONO_INTERP_TESTS_COVERAGE") && mono_profiler_enable_coverage ()) {
		MonoProfilerHandle handle = mono_profiler_create (nullptr);

		mono_profiler_set_coverage_filter_callback (
			handle, [] (MonoProfiler *, MonoMethod *) -> mono_bool { return TRUE; });
	}

	mono_jit_init_version_for_test_only ("mono-interp-tests", "v4.0.30319");

	MonoImage *image = open_assembly (assembly);
	int result;

	if (image == nullptr)
		result = 2;
	else if (listing)
		result = list_tests (image);
	else if (all)
		result = run_all (image, only, arm, xfail, skip);
	else
		result = run_test (image, test);

	/*
	 * On every path, and before main returns. This waits for the compile in
	 * hand and queues nothing more. A process that exits with a tier-2
	 * promotion still on the compile worker destroys LLVM's static tables under
	 * it - the MVT list behind SDNode::getValueTypeList (), and the file system
	 * every PassBuilder holds. The run then ends in an assertion inside
	 * instruction selection, far from anything the test did.
	 *
	 * `mono` gets this from mini_cleanup (), which this harness cannot call:
	 * mono_cleanup () destroys the assemblies lock and then closes images
	 * through mono_debug_cleanup (), which takes it. Calling mono_jit_cleanup ()
	 * from here aborts every run.
	 */
	mono_llvm_jit_stop_compiling ();

	return result;
}

#include "harness.hpp"

#include "config.h"
#include <glib.h>

#include "mono/interp/mintops.hpp"

#include <mono/metadata/assembly-internals.h>
#include <mono/metadata/class-inlines.h>
#include <mono/metadata/class-internals.h>
#include <mono/metadata/debug-helpers.h>
#include <mono/metadata/domain-internals.h>
#include <mono/metadata/loader.h>
#include <mono/metadata/profiler-private.h>
#include <mono/mini/jit.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace mono {
namespace test {

namespace {

MonoImage *
load_image (const std::string &name)
{
	std::string path = std::string (MONO_INTERP_TESTS_DIR) + "/" + name + ".dll";
	MonoImageOpenStatus status = MONO_IMAGE_OK;
	MonoAssemblyOpenRequest request;

	mono_assembly_request_prepare_open (&request, MONO_ASMCTX_DEFAULT,
	                                    mono_domain_default_alc (mono_domain_get ()));

	MonoAssembly *assembly = mono_assembly_request_open (path.c_str (), &request, &status);

	if (assembly == nullptr || status != MONO_IMAGE_OK) {
		fprintf (stderr, "cannot open %s (status %d)\n", path.c_str (), (int) status);
		abort ();
	}

	return mono_assembly_get_image_internal (assembly);
}

MonoMethod *
find_method (const std::string &image, const std::string &name)
{
	MonoMethodDesc *desc = mono_method_desc_new (name.c_str (), TRUE);
	MonoMethod *method = mono_method_desc_search_in_image (desc, load_image (image));

	mono_method_desc_free (desc);

	if (method == nullptr) {
		fprintf (stderr, "no method %s in %s.dll\n", name.c_str (), image.c_str ());
		abort ();
	}

	mono_class_init_internal (method->klass);
	return method;
}

MonoMethodHeader *
read_header (MonoMethod *method)
{
	ERROR_DECL (error);
	MonoMethodHeader *header = mono_method_get_header_checked (method, error);

	mono_error_assert_ok (error);
	return header;
}

/*
 * The transform reads rtm->domain while it is being constructed, so the
 * InterpMethod has to be complete before it. A member initializer cannot fill
 * one in field by field, hence a whole one by value.
 */
InterpMethod
make_rtm (MonoMethod *method)
{
	InterpMethod rtm{};

	rtm.method = method;
	rtm.domain = mono_domain_get ();
	return rtm;
}

} // namespace

void
init_runtime ()
{
	static bool started = false;

	if (started)
		return;
	started = true;

	mono_set_assemblies_path (MONO_INTERP_TESTS_ASSEMBLIES);
	mono_jit_init_version_for_test_only ("mono-interp-tests", "v4.0.30319");
}

/*
 * The class library and the il/ images are built by the same gate, so one file
 * answers for both.  mscorlib is also what goes missing first -- init_runtime ()
 * dies on it before any image can be opened.
 */
bool
have_corpus ()
{
	return g_file_test (MONO_INTERP_TESTS_ASSEMBLIES "/mscorlib.dll", G_FILE_TEST_EXISTS);
}

Transform::Transform (const std::string &image, const std::string &method_name, int verbose_level)
	: method (find_method (image, method_name)),
	  header (read_header (method)),
	  rtm (make_rtm (method)),
	  td (method, header, &rtm)
{
	ERROR_DECL (error);

	if (verbose_level)
		td.verbose_level = verbose_level;

	mono_test_interp_method_compute_offsets (&td, &rtm,
	                                         mono_method_signature_internal (method), header);

	mono_test_interp_generate_code (&td, method, header, NULL, error);
	mono_error_assert_ok (error);
}

Transform::~Transform ()
{
	mono_metadata_free_mh (header);
}

void
Transform::cprop ()
{
	mono_test_interp_cprop (&td);
}

Code::Code (Transform &transform)
{
	// The instructions hang off the basic blocks, in IL order along next_bb.
	for (interp::InterpBasicBlock *bb = transform.get ()->entry_bb; bb != nullptr; bb = bb->next_bb) {
		for (interp::InterpInst *ins = bb->first_ins; ins != nullptr; ins = ins->next) {
			if (ins->opcode == MINT_NOP)
				continue;

			instructions.push_back (ins);
			names.push_back (mono::interp::opname (ins->opcode));
		}
	}
}

interp::InterpInst *
Code::at (size_t index) const
{
	return index < instructions.size () ? instructions [index] : nullptr;
}

} // namespace test
} // namespace mono

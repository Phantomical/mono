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

/*
 * The setup below is generate () in transform.c, field for field. Keep the two
 * in step: the transform reads several of these before anything sets them, so a
 * field left out here is a null dereference rather than a compile error.
 */
Transform::Transform (const std::string &image, const std::string &method_name,
                      int verbose_level)
{
	ERROR_DECL (error);
	MonoMethod *method = find_method (image, method_name);

	header = mono_method_get_header_checked (method, error);
	mono_error_assert_ok (error);

	memset (&rtm, 0, sizeof (rtm));
	rtm.method = method;
	rtm.domain = mono_domain_get ();

	memset (&td, 0, sizeof (td));
	td.method = method;
	td.rtm = &rtm;
	td.code_size = header->code_size;
	td.header = header;
	td.max_code_size = td.code_size;
	td.in_offsets = (int *) g_malloc0 ((header->code_size + 1) * sizeof (int));
	td.clause_indexes = (int *) g_malloc (header->code_size * sizeof (int));
	td.mempool = mono_mempool_new ();
	td.mem_manager = m_method_get_mem_manager (rtm.domain, method);
	td.data_items = NULL;
	td.data_hash = g_hash_table_new (NULL, NULL);
	td.gen_sdb_seq_points = mini_debug_options.gen_sdb_seq_points;
	td.seq_points = g_ptr_array_new ();
	td.verbose_level = verbose_level ? verbose_level : mono_interp_traceopt;
	td.prof_coverage = mono_profiler_coverage_instrumentation_enabled (method);

	mono_test_interp_method_compute_offsets (&td, &rtm,
	                                         mono_method_signature_internal (method), header);

	td.stack = (interp::StackInfo *) g_malloc0 ((header->max_stack + 1) * sizeof (td.stack [0]));
	td.stack_capacity = header->max_stack + 1;
	td.sp = td.stack;
	td.max_stack_height = 0;
	td.line_numbers = g_array_new (FALSE, TRUE, sizeof (MonoDebugLineNumberEntry));
	td.current_il_offset = -1;

	mono_test_interp_generate_code (&td, method, header, NULL, error);
	mono_error_assert_ok (error);
}

Transform::~Transform ()
{
	g_free (td.in_offsets);
	g_free (td.clause_indexes);
	g_free (td.data_items);
	g_free (td.stack);
	g_free (td.locals);
	g_hash_table_destroy (td.data_hash);
	g_ptr_array_free (td.seq_points, TRUE);
	if (td.line_numbers)
		g_array_free (td.line_numbers, TRUE);
	mono_mempool_destroy (td.mempool);
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

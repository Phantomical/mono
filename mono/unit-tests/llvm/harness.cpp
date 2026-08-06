#include "harness.hpp"

/*
 * assembly-internals.h has no linkage guard of its own, so it has to be pulled in
 * before anything else reaches it and given one here.
 */
#include "config.h"
#include <glib.h>
extern "C" {
#include <mono/metadata/assembly-internals.h>
}

#include "method-to-llvm.hpp"

#include <mono/metadata/debug-helpers.h>
#include <mono/metadata/domain-internals.h>
#include <mono/metadata/loader.h>
#include <mono/mini/jit.h>

/* mono-tls.h puts PIC back in scope, and it breaks some LLVM headers. */
#ifdef PIC
#undef PIC
#endif

#include <llvm/IR/Function.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/raw_ostream.h>

#include <cstdlib>
#include <cstring>

namespace mono {
namespace test {

namespace {

/*
 * Everything a method_to_llvm () call needs out of the MonoCompile it is handed.
 *
 * The translator reads the header - the IL, the locals signature and the clause
 * table - and it reads `opt` and `compile_llvm`, which together are all
 * MONO_CLASS_IS_SIMD looks at. Both are set the way an LLVM compile would have
 * them, so a SIMD class lowers to a vector here as it would in the JIT.
 *
 * The rest of a MonoCompile belongs to the mini pipeline, which is not running,
 * so it stays zeroed rather than half-filled with values nothing reads.
 */
class MinimalCompile {
public:
	MinimalCompile (MonoMethod *method, MonoError *error)
	{
		memset (&cfg, 0, sizeof (cfg));
		cfg.method = method;
		/* The translator interns ldstr literals into cfg->domain. */
		cfg.domain = mono_domain_get ();
		cfg.compile_llvm = TRUE;
		cfg.opt = MONO_OPT_SIMD;
		cfg.header = mono_method_get_header_checked (method, error);
	}

	~MinimalCompile () { mono_metadata_free_mh (cfg.header); }

	MinimalCompile (const MinimalCompile &) = delete;
	MinimalCompile &operator= (const MinimalCompile &) = delete;

	MonoCompile *get () { return &cfg; }

private:
	MonoCompile cfg;
};

} // namespace

void
init_runtime ()
{
	static bool started = false;

	if (started)
		return;
	started = true;

	mono_set_assemblies_path (MONO_LLVM_TESTS_ASSEMBLIES);
	mono_jit_init_version_for_test_only ("mono-llvm-tests", "v4.0.30319");
}

MonoImage *
load_image (const std::string &name)
{
	std::string path = std::string (MONO_LLVM_TESTS_DIR) + "/" + name + ".dll";
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

std::string
verify_function (llvm::Function &function)
{
	std::string complaint;
	llvm::raw_string_ostream os (complaint);

	if (!llvm::verifyFunction (function, &os))
		complaint.clear ();

	return complaint;
}

std::string
Translation::text () const
{
	if (function == nullptr)
		return std::string ();

	std::string out;
	llvm::raw_string_ostream os (out);

	function->print (os);
	return out;
}

size_t
Translation::count (const std::string &needle) const
{
	std::string body = text ();
	size_t found = 0;

	for (size_t at = body.find (needle); at != std::string::npos;
	     at = body.find (needle, at + needle.size ()))
		++found;

	return found;
}

std::unique_ptr<Translation>
translate_method (const std::string &image, const std::string &name)
{
	auto owned = std::make_unique<Translation> ();
	Translation &result = *owned;

	result.context = std::make_unique<llvm::LLVMContext> ();
	result.module = std::make_unique<llvm::Module> (image, *result.context);

	MonoMethodDesc *desc = mono_method_desc_new (name.c_str (), TRUE);
	MonoMethod *method = mono_method_desc_search_in_image (desc, load_image (image));

	mono_method_desc_free (desc);

	if (method == nullptr) {
		fprintf (stderr, "no method %s in %s.dll\n", name.c_str (), image.c_str ());
		abort ();
	}

	ERROR_DECL (metadata_error);
	MinimalCompile cfg (method, metadata_error);

	if (cfg.get ()->header == nullptr) {
		result.error = mono_error_get_message (metadata_error);
		mono_error_cleanup (metadata_error);
		return owned;
	}

	llvm::Expected<llvm::Function *> translated =
		method_to_llvm (result.module.get (), cfg.get (), method);

	if (!translated) {
		result.error = llvm::toString (translated.takeError ());
		return owned;
	}

	result.function = *translated;
	result.verifier_error = verify_function (*result.function);
	return owned;
}

const Translation &
TranslatorTest::translate (const std::string &image, const std::string &name)
{
	translations.push_back (translate_method (image, name));
	return *translations.back ();
}

void
TranslatorTest::TearDown ()
{
	for (const auto &translation : translations)
		if (!translation->verifier_error.empty ())
			ADD_FAILURE () << "the emitted IR does not verify:\n"
				       << translation->verifier_error << "\n"
				       << translation->text ();
}

} // namespace test
} // namespace mono

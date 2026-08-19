/**
 * \file
 * \brief Reading an override assembly and replacing the methods it names.
 */

#include "config.h"

#include "method-override.hpp"
#include "method-override.h"

#include "domain-method.h"
#include "domain-method.hpp"

#include <mono/metadata/appdomain.h>
#include <mono/metadata/assembly.h>
#include <mono/metadata/assembly-internals.h>
#include <mono/metadata/class-internals.h>
#include <mono/metadata/debug-helpers.h>
#include <mono/metadata/icall-internals.h>
#include <mono/metadata/metadata-internals.h>
#include <mono/metadata/object-internals.h>
#include <mono/metadata/reflection.h>
#include <mono/metadata/reflection-internals.h>
#include <mono/metadata/tokentype.h>
#include <mono/utils/mono-error-internals.h>

#include <llvm/ADT/DenseMap.h>
#include <llvm/ADT/SmallVector.h>

#include <atomic>
#include <mutex>
#include <string>

#ifdef HAVE_DLADDR
#include <dlfcn.h>
#endif

namespace mono {

namespace {

/// What the runtime looks for beside itself.
const char OVERRIDE_ASSEMBLY[] = "mono-overrides.dll";

/// The attribute an override carries. Matched by name in the assembly being
/// read, which declares it rather than referencing one.
const char OVERRIDE_NAMESPACE[] = "Mono.Overrides";
const char OVERRIDE_ATTRIBUTE[] = "MonoOverrideAttribute";

/// One line of the override assembly: a target named in text, and the method
/// that replaces it.
///
/// The target is kept as a description rather than a method because the
/// assembly holding it need not be loaded yet, and because several copies of it
/// can be loaded - each mod ships its own Harmony - and every copy is replaced.
struct PendingOverride {
	MonoMethodDesc *target;
	MonoMethod *replacement;
};

llvm::SmallVector<PendingOverride, 8> *g_pending;

/// Target definition to replacement definition, filled in as images load.
llvm::DenseMap<MonoMethod *, MonoMethod *> *g_matched;

std::mutex g_lock;

/*
 * Read without the lock on every method both engines ask for, so it has to be
 * free. It only ever rises, and a reader that is one image behind misses an
 * override it finds a moment later.
 */
std::atomic<bool> g_registered { false };

/// Whether the assembly load hook is in place. Reading a second override
/// assembly must not add a second one.
bool g_hooked;

/// The directory the runtime shared library was loaded from, or empty.
std::string runtime_directory ()
{
#ifdef HAVE_DLADDR
	Dl_info info;

	/* Any symbol in the runtime will do; this file is part of it. */
	if (dladdr ((void *) &runtime_directory, &info) && info.dli_fname != nullptr) {
		char *dir = g_path_get_dirname (info.dli_fname);
		std::string held (dir);

		g_free (dir);
		return held;
	}
#endif
	return std::string ();
}

/// The one string an override attribute is constructed with, or empty.
///
/// The blob is a prolog, a length-prefixed UTF-8 string and a named-argument
/// count. A null string is 0xFF rather than a length.
std::string attribute_target (const MonoCustomAttrEntry &entry)
{
	const mono_byte *data = entry.data;

	if (entry.data_size < 4 || data[0] != 0x01 || data[1] != 0x00)
		return std::string ();

	const char *cursor = (const char *) data + 2;

	if ((mono_byte) *cursor == 0xFF)
		return std::string ();

	uint32_t length = mono_metadata_decode_value (cursor, &cursor);

	if (cursor + length > (const char *) data + entry.data_size)
		return std::string ();

	return std::string (cursor, length);
}

/// How many type parameters a method carries, its class's included.
int total_generic_arity (MonoMethod *method)
{
	int arity = 0;

	if (MonoGenericContainer *klass = mono_class_try_get_generic_container (method->klass))
		arity += klass->type_argc;

	if (MonoGenericContainer *own = mono_method_get_generic_container (method))
		arity += own->type_argc;

	return arity;
}

/// Whether two types are the same as far as an override has to be checked.
///
/// By name, because the target and the replacement are in different images and
/// nothing in them is the same pointer. A generic parameter on either side is
/// taken on trust: the two sides number theirs differently and the arity check
/// above is what guards that.
bool same_type (MonoType *left, MonoType *right)
{
	if (left->type == MONO_TYPE_VAR || left->type == MONO_TYPE_MVAR
	    || right->type == MONO_TYPE_VAR || right->type == MONO_TYPE_MVAR)
		return true;

	char *left_name = mono_type_get_name_full (left, MONO_TYPE_NAME_FORMAT_FULL_NAME);
	char *right_name = mono_type_get_name_full (right, MONO_TYPE_NAME_FORMAT_FULL_NAME);
	bool same = left_name != nullptr && right_name != nullptr
	            && strcmp (left_name, right_name) == 0;

	g_free (left_name);
	g_free (right_name);
	return same;
}

/// Whether replacement can stand in for target, and says why in the log if not.
///
/// An override is always static. Where the target is an instance method the
/// replacement takes the receiver as its first parameter, which is not checked:
/// the target's class is in another image, so the override declares it as
/// object.
bool signature_matches (MonoMethod *target, MonoMethod *replacement)
{
	MonoMethodSignature *want = mono_method_signature_internal (target);
	MonoMethodSignature *have = mono_method_signature_internal (replacement);

	if (want == nullptr || have == nullptr)
		return false;

	const char *why = nullptr;
	int receiver = want->hasthis ? 1 : 0;

	if (have->hasthis)
		why = "the override is not static";
	else if (have->param_count != want->param_count + receiver)
		why = "the parameter counts do not match";
	else if (total_generic_arity (replacement) != total_generic_arity (target))
		why = "the type parameter counts do not match";
	else if (!same_type (want->ret, have->ret))
		why = "the return types do not match";

	for (int i = 0; why == nullptr && i < want->param_count; ++i)
		if (!same_type (want->params[i], have->params[i + receiver]))
			why = "the parameter types do not match";

	if (why == nullptr)
		return true;

	char *target_name = mono_method_full_name (target, TRUE);
	char *replacement_name = mono_method_full_name (replacement, TRUE);

	g_printerr ("mono-overrides: %s cannot replace %s: %s\n", replacement_name, target_name,
	            why);
	g_free (target_name);
	g_free (replacement_name);
	return false;
}

/// Looks for every pending target in a newly loaded image.
///
/// A description is never retired. Several copies of an assembly can be loaded
/// at once, and each copy's own methods have to be replaced.
void match_image (MonoImage *image)
{
	if (image == nullptr || g_pending == nullptr)
		return;

	llvm::SmallVector<PendingOverride, 8> pending;

	{
		std::lock_guard<std::mutex> held (g_lock);
		pending = *g_pending;
	}

	for (const PendingOverride &entry : pending) {
		/*
		 * Outside the lock. Naming a method loads the classes its signature
		 * names, which takes the loader lock, and a thread holding that one
		 * arrives here through domain_method_get () wanting this lock.
		 */
		MonoMethod *target = mono_method_desc_search_in_image (entry.target, image);

		if (target == nullptr || !signature_matches (target, entry.replacement))
			continue;

		std::lock_guard<std::mutex> held (g_lock);

		g_matched->insert ({ target, entry.replacement });
		g_registered.store (true, std::memory_order_release);
	}
}

void
assembly_loaded (MonoAssemblyLoadContext *alc, MonoAssembly *assembly, void *user_data,
                 MonoError *error)
{
	match_image (mono_assembly_get_image_internal (assembly));
}

/// Records every method in the override assembly that carries the attribute.
void collect_overrides (MonoImage *image, MonoClass *attribute)
{
	int rows = mono_image_get_table_rows (image, MONO_TABLE_TYPEDEF);

	for (int row = 1; row <= rows; ++row) {
		ERROR_DECL (error);
		MonoClass *klass =
			mono_class_get_checked (image, MONO_TOKEN_TYPE_DEF | row, error);

		if (!is_ok (error) || klass == nullptr) {
			mono_error_cleanup (error);
			continue;
		}

		void *iter = nullptr;
		while (MonoMethod *method = mono_class_get_methods (klass, &iter)) {
			ERROR_DECL (attr_error);
			MonoCustomAttrInfo *attrs =
				mono_custom_attrs_from_method_checked (method, attr_error);

			if (!is_ok (attr_error))
				mono_error_cleanup (attr_error);

			if (attrs == nullptr)
				continue;

			for (int i = 0; i < attrs->num_attrs; ++i) {
				if (attrs->attrs[i].ctor == nullptr
				    || attrs->attrs[i].ctor->klass != attribute)
					continue;

				std::string target = attribute_target (attrs->attrs[i]);

				if (target.empty ())
					continue;

				if (MonoMethodDesc *desc =
				            mono_method_desc_new (target.c_str (), TRUE))
					g_pending->push_back ({ desc, method });
				else
					g_printerr ("mono-overrides: cannot parse target \"%s\"\n",
					            target.c_str ());
			}

			mono_custom_attrs_free (attrs);
		}
	}
}

/// Mono.Overrides.MonoOverride::Install, which an override assembly calls to
/// replace a method it cannot name in an attribute.
///
/// The two handles are MonoMethod pointers, which is what
/// RuntimeMethodHandle.Value holds. Nothing checks them: the only caller is the
/// assembly the runtime read out of its own directory.
void
ves_icall_install_override (MonoMethod *target, MonoMethod *replacement)
{
	if (target == nullptr || replacement == nullptr)
		return;

	mono_install_method_override (target, mono_domain_get (), replacement);
}

/// Whether the icall is registered. Reading a second override assembly must not
/// register it twice.
bool g_icall_registered;

void register_icall ()
{
	if (g_icall_registered)
		return;

	g_icall_registered = true;
	mono_add_internal_call_internal ("Mono.Overrides.MonoOverride::Install",
	                                 (const void *) ves_icall_install_override);
}

} // namespace

bool
method_overrides_registered ()
{
	return g_registered.load (std::memory_order_acquire);
}

MonoMethod *
registered_override_for (MonoMethod *method)
{
	if (!method_overrides_registered ())
		return nullptr;

	MonoMethod *definition =
		method->is_inflated ? ((MonoMethodInflated *) method)->declaring : method;
	MonoMethod *replacement = nullptr;

	{
		std::lock_guard<std::mutex> held (g_lock);
		auto it = g_matched->find (definition);

		if (it == g_matched->end ())
			return nullptr;
		replacement = it->second;
	}

	if (method == definition)
		return replacement;

	/*
	 * The override is written against the definition, so it has to be given the
	 * target's own type arguments. It is a static method of a plain class, so
	 * every one of them is a method type argument here however the target
	 * spelled it - the class's first, then the method's.
	 */
	MonoGenericContext *context = mono_method_get_context (method);
	llvm::SmallVector<MonoType *, 4> arguments;

	if (context != nullptr && context->class_inst != nullptr)
		for (guint i = 0; i < context->class_inst->type_argc; ++i)
			arguments.push_back (context->class_inst->type_argv[i]);

	if (context != nullptr && context->method_inst != nullptr)
		for (guint i = 0; i < context->method_inst->type_argc; ++i)
			arguments.push_back (context->method_inst->type_argv[i]);

	if (arguments.empty ())
		return replacement;

	ERROR_DECL (error);
	MonoGenericContext inflated = {
		nullptr, mono_metadata_get_generic_inst ((int) arguments.size (), arguments.data ())
	};
	MonoMethod *result =
		mono_class_inflate_generic_method_checked (replacement, &inflated, error);

	if (!is_ok (error)) {
		mono_error_cleanup (error);
		return nullptr;
	}

	return result;
}

bool
method_overrides_load (const char *path)
{
	/* No override assembly is the ordinary case and is not worth a word. */
	if (!g_file_test (path, G_FILE_TEST_IS_REGULAR))
		return false;

	register_icall ();

	MonoAssemblyOpenRequest req;
	mono_assembly_request_prepare_open (&req, MONO_ASMCTX_DEFAULT,
	                                    mono_domain_default_alc (mono_domain_get ()));

	MonoImageOpenStatus status;
	MonoAssembly *assembly = mono_assembly_request_open (path, &req, &status);

	if (assembly == nullptr) {
		g_printerr ("mono-overrides: cannot load %s\n", path);
		return false;
	}

	MonoImage *image = mono_assembly_get_image_internal (assembly);
	ERROR_DECL (error);
	MonoClass *attribute = mono_class_from_name_checked (image, OVERRIDE_NAMESPACE,
	                                                     OVERRIDE_ATTRIBUTE, error);

	if (!is_ok (error) || attribute == nullptr) {
		mono_error_cleanup (error);
		g_printerr ("mono-overrides: %s declares no %s.%s\n", path, OVERRIDE_NAMESPACE,
		            OVERRIDE_ATTRIBUTE);
		return false;
	}

	if (g_pending == nullptr) {
		g_pending = new llvm::SmallVector<PendingOverride, 8> ();
		g_matched = new llvm::DenseMap<MonoMethod *, MonoMethod *> ();
	}

	collect_overrides (image, attribute);

	if (g_pending->empty ())
		return true;

	/*
	 * The hook covers what loads from here on. Whatever is already loaded -
	 * corlib, and the override assembly itself - is walked now, since an
	 * override may well name one of them.
	 */
	if (!g_hooked) {
		g_hooked = true;
		mono_install_assembly_load_hook_v2 (assembly_loaded, nullptr, FALSE);
	}

	mono_assembly_foreach ([] (void *loaded, void *) {
		match_image (mono_assembly_get_image_internal ((MonoAssembly *) loaded));
	}, nullptr);

	return true;
}

void
method_overrides_init ()
{
	std::string directory = runtime_directory ();

	if (directory.empty ())
		return;

	method_overrides_load ((directory + G_DIR_SEPARATOR_S + OVERRIDE_ASSEMBLY).c_str ());
}

} // namespace mono

void
mono_method_overrides_init (void)
{
	mono::method_overrides_init ();
}

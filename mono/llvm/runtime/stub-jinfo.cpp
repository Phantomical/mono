#include "stub-jinfo.hpp"

#include "naming.hpp"

#include <llvm/ADT/ArrayRef.h>
#include <llvm/ADT/StringRef.h>

#include <vector>

#include "mini-runtime.h"

#include "mono/metadata/class-internals.h"

namespace mono {

namespace {

llvm::ArrayRef<uint8_t>
stub_unwind_info ()
{
	static const std::vector<uint8_t> encoded = [] {
		// A stub is a bare jump and pushes nothing, so the frame at any
		// point inside it is still the caller's. That is exactly what
		// the arch CIE describes on its own.
		GSList *ops = mono_arch_get_cie_program ();
		guint32 len = 0;
		guint8 *bytes = mono_unwind_ops_encode (ops, &len);
		std::vector<uint8_t> program (bytes, bytes + len);

		g_free (bytes);
		mono_free_unwind_info (ops);
		return program;
	}();

	return encoded;
}

} // namespace

MonoJitInfo *
register_stub_jinfo (MonoDomain *domain, MonoMethod *method, void *stub, size_t size,
                     const std::string &name)
{
	llvm::ArrayRef<uint8_t> unwind = stub_unwind_info ();
	guint8 *uw_info = const_cast<guint8 *> (unwind.data ());
	guint32 uw_info_len = (guint32) unwind.size ();

	/*
	 * Everything that reads this record's name only ever prints it: a profile,
	 * a stack dump, the jit map. Each of them already has the address to tell
	 * two records apart with. The name is carried in the form a reader wants,
	 * not as the symbol.
	 *
	 * The stub over the plain body symbol needs a suffix of its own, or it
	 * prints exactly as the body it jumps to. The two are worth telling apart:
	 * one is the method, the other is the sixteen bytes in front of it.
	 */
	std::string display = display_name (method, name);

	if (llvm::StringRef (name).ends_with (identity_of (method)))
		display += "$stub";

	/*
	 * mono_jit_info_table_remove () frees what it unregisters, so a record that
	 * has to come out again has to come from the allocator that call uses.
	 */
	if (method->dynamic)
		return mono_tramp_info_register_reclaimable (domain, method, stub,
		                                             (guint32) size, display.c_str (),
		                                             uw_info, uw_info_len);

	MonoTrampInfo *tramp = g_new0 (MonoTrampInfo, 1);

	tramp->code = (guint8 *) stub;
	tramp->code_size = (guint32) size;
	tramp->name = g_strdup (display.c_str ());
	tramp->method = method;
	tramp->uw_info = uw_info;
	tramp->uw_info_len = uw_info_len;
	mono_tramp_info_register (tramp, domain);
	return nullptr;
}

} // namespace mono

#include "naming.hpp"

#include "method-to-llvm.hpp"

#include <llvm/IR/Module.h>

#include "mono/metadata/class-internals.h"
#include "mono/metadata/debug-helpers.h"
#include "mono/metadata/marshal.h"

namespace mono {

std::string
identity_of (MonoMethod *method)
{
	char buf[32];

	snprintf (buf, sizeof (buf), "@%p", (void *) method);
	return buf;
}

llvm::StringRef
stub_suffix (Entry entry)
{
	switch (entry) {
	case Entry::body:
		return "";
	case Entry::interop:
		return "$interop";
	case Entry::unbox:
		return "$unbox";
	}
	return "";
}

std::string
stub_symbol (MonoMethod *method, Entry entry)
{
	char *name = mono_method_full_name (method, TRUE);
	std::string symbol =
		std::string (name) + identity_of (method) + stub_suffix (entry).str ();

	g_free (name);
	return symbol;
}

std::string
definition_symbol (MonoMethod *method, Entry entry)
{
	llvm::StringRef suffix;

	switch (entry) {
	case Entry::interop:
		suffix = "$entry";
		break;
	case Entry::unbox:
		suffix = "$unboxentry";
		break;
	case Entry::body:
		g_assert_not_reached ();
		break;
	}

	return stub_symbol (method, Entry::body) + suffix.str ();
}

llvm::Error
bind_symbols (llvm::Module &m)
{
	return bind_method_symbols (
		m, [] (MonoMethod *target, Entry entry) -> llvm::Expected<std::string> {
			return stub_symbol (target, entry);
		});
}

std::string
display_name (MonoMethod *method, llvm::StringRef symbol)
{
	std::string identity = identity_of (method);
	size_t at = symbol.rfind (identity);

	if (at == llvm::StringRef::npos)
		return symbol.str ();

	return symbol.take_front (at).str ()
	       + symbol.drop_front (at + identity.size ()).str ();
}

bool
wants_unbox_entry (MonoMethod *method, MonoMethodSignature *sig)
{
	return sig != nullptr && sig->hasthis && m_class_is_valuetype (method->klass);
}

bool
publishes_interop_entry (MonoMethod *method)
{
	MonoMethodSignature *sig = mono_method_signature_internal (method);

	return sig != nullptr && sig->pinvoke != 0
	       && method->wrapper_type != MONO_WRAPPER_NONE;
}

bool
publishes_unbox_entry (MonoMethod *method)
{
	return !implemented_outside_il (method)
	       && wants_unbox_entry (method, mono_method_signature_internal (method));
}

} // namespace mono

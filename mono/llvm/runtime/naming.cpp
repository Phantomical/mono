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

std::string
stub_symbol (MonoMethod *method)
{
	char *name = mono_method_full_name (method, TRUE);
	std::string symbol = std::string (name) + identity_of (method);

	g_free (name);
	return symbol;
}

std::string
interop_symbol (MonoMethod *method)
{
	return stub_symbol (method) + "$entry";
}

llvm::Error
bind_symbols (llvm::Module &m)
{
	return bind_method_symbols (m,
	                            [] (MonoMethod *target) -> llvm::Expected<std::string> {
		                            return stub_symbol (target);
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
	if (mono_method_is_unmanaged_callers_only (method))
		return true;

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

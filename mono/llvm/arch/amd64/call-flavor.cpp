/**
 * \file
 * \brief Reading a signature for which boundary convention it speaks.
 */

#include "arch/arch.hpp"

#include "mini.h"

#include "mono/metadata/metadata.h"

namespace mono::arch {

/*
 * The hidden return pointer sits behind the first argument whenever the
 * runtime keeps a receiver there: the trampolines that recover a receiver from
 * a call site always look in the first register. The same applies when the
 * first declared parameter is a reference type, because delegate-invoke
 * wrappers make virtual calls through calli signatures with hasthis unset
 * (mini-amd64.c, get_call_info).
 */
LegacyFlavor
managed_call_flavor (MonoMethodSignature *sig)
{
	if (sig->hasthis)
		return LegacyFlavor::ManagedVret1;
	if (sig->param_count > 0
	    && MONO_TYPE_IS_REFERENCE (mini_get_underlying_type (sig->params[0])))
		return LegacyFlavor::ManagedVret1;
	return LegacyFlavor::Managed;
}

} // namespace mono::arch

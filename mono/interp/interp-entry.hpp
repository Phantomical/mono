#ifndef __MONO_INTERP_INTERP_ENTRY_HPP__
#define __MONO_INTERP_INTERP_ENTRY_HPP__

/**
 * \file
 * \brief The address a caller outside this engine uses to reach a method.
 */

#include "interp-internals.h"
#include "mono/utils/mono-error-internals.h"

namespace mono::interp {

/* The address that stands for imethod, minted if this is the first ask. */
gpointer entry_for_imethod (InterpMethod *imethod, MonoError *error);

/*
 * The address that stands for imethod outside this engine, recording that the
 * address is now in native hands. A patcher writes a jump over what it is given,
 * so both engines have to name the same address for a method.
 */
gpointer escaping_entry_for_imethod (InterpMethod *imethod, MonoError *error);

} // namespace mono::interp

#endif

#ifndef __MONO_INTERP_INTERP_CALLBACKS_HPP__
#define __MONO_INTERP_INTERP_CALLBACKS_HPP__

/**
 * \file
 * \brief Declares the answers this engine gives to mini's execution-engine hooks.
 */

#include "interp-internals.hpp"
#include "mono/mini/ee.h"

namespace mono::interp {

/* Generated from the same list that builds the callback table, so a definition
 * whose signature has drifted is a compile error rather than a wrong entry. */
#undef MONO_EE_CALLBACK
#define MONO_EE_CALLBACK(ret, name, sig) ret interp_##name sig;
MONO_EE_CALLBACKS
#undef MONO_EE_CALLBACK

} // namespace mono::interp

#endif

#ifndef __MONO_INTERP_INTERP_IMETHOD_HPP__
#define __MONO_INTERP_INTERP_IMETHOD_HPP__

/**
 * \file
 * \brief The per-domain table of methods this engine has seen.
 */

#include "interp-internals.h"

namespace mono::interp {

/* Answers only for a method the engine already knows; mono_interp_get_imethod ()
 * is the one that makes an entry. */
InterpMethod *lookup_imethod (MonoDomain *domain, MonoMethod *method);

} // namespace mono::interp

#endif

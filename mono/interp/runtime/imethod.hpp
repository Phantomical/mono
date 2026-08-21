#ifndef __MONO_INTERP_INTERP_IMETHOD_HPP__
#define __MONO_INTERP_INTERP_IMETHOD_HPP__

/**
 * \file
 * \brief How to reach what this engine keeps for a method.
 */

#include "internals.hpp"

namespace mono::interp {

/// Returns a record only for a method the engine already knows.
/// mono_interp_get_imethod () is what makes the record.
InterpMethod *lookup_imethod (MonoDomain *domain, MonoMethod *method);

} // namespace mono::interp

#endif

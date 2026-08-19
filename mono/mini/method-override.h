/**
 * \file
 * \brief What mono's C runtime sees of the override table.
 *
 * The table itself is C++ - see method-override.hpp.
 */

#ifndef MONO_MINI_METHOD_OVERRIDE_H
#define MONO_MINI_METHOD_OVERRIDE_H

#include <mono/utils/mono-publib.h>

MONO_BEGIN_DECLS

/// Reads the override assembly beside the runtime, where there is one.
///
/// Call this once the root domain can load an assembly, and before anything an
/// override could name has run. A missing assembly is not an error.
void mono_method_overrides_init (void);

MONO_END_DECLS

#endif /* MONO_MINI_METHOD_OVERRIDE_H */

/**
 * \file
 * \brief The C-callable half of jit-dump.hpp.
 */

#ifndef MONO_MINI_JIT_DUMP_H
#define MONO_MINI_JIT_DUMP_H

#include <glib.h>

#include <mono/utils/mono-publib.h>

typedef struct _MonoMethod MonoMethod;

MONO_BEGIN_DECLS

/// Whether this method matches the tier-0 assembly dump settings.
gboolean mono_tier0_asm_dump_wanted (MonoMethod *method);

/// Writes this method's `tier0-asm` dump.
void mono_tier0_asm_dump_write (MonoMethod *method, const char *text);

/// Blocks until all queued directory dumps have been written.
///
/// Gives up when no dump has been written for a few seconds.
void mono_jit_dump_flush (void);

MONO_END_DECLS

#endif

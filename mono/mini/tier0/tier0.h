/**
 * \file
 * \brief What the runtime calls into the classic compiler through.
 *
 * Declares only what C sees, so mono/mini can include it without the classic
 * compiler's own types.
 */

#ifndef __MONO_MINI_TIER0_H__
#define __MONO_MINI_TIER0_H__

#include <glib.h>
#include <mono/utils/mono-error.h>
#include <mono/utils/mono-forward.h>

G_BEGIN_DECLS

typedef struct _MonoMethod MonoMethod;

/* Fills in the backend description that every compile reads. */
void mono_tier0_init (void);

/// Registers the classic compiler's opcode emulation icalls.
///
/// Call after mono_create_icall_signatures (): before that runs, the icall
/// signatures this uses still hold a raw type count, not a parameter count.
void mono_tier0_register_opcode_emulations (void);

/// Compiles \p method with the classic compiler, for \p domain.
///
/// The jit info is in \p domain's table already when this returns, so the
/// caller must not add it again. Nothing points at the code yet: publishing it
/// is the caller's. Returns FALSE and sets \p error on a failed compile.
gboolean mono_tier0_compile (MonoMethod *method, MonoDomain *domain,
                             gpointer *out_code, MonoJitInfo **out_jinfo,
                             MonoError *error);

G_END_DECLS

#endif /* __MONO_MINI_TIER0_H__ */

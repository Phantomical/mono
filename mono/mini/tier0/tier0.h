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

G_BEGIN_DECLS

/* Fills in the backend description that every compile reads. */
void mono_tier0_init (void);

G_END_DECLS

#endif /* __MONO_MINI_TIER0_H__ */

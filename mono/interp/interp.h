/**
 * \file
 * \brief What the rest of the runtime can say to the interpreter.
 *
 * The engine is C++ and keeps its own headers to itself. This is the C surface
 * mono/mini compiles against.
 */

#ifndef __MONO_INTERP_H__
#define __MONO_INTERP_H__
#include <mono/mini/mini-runtime.h>

MONO_BEGIN_DECLS

/* How many arguments a method can have and still be entered through one of the
 * pre-built entry functions rather than a generic one. */
#define MAX_INTERP_ENTRY_ARGS 8

enum {
	INTERP_OPT_NONE = 0,
	INTERP_OPT_INLINE = 1,
	INTERP_OPT_CPROP = 2,
	INTERP_OPT_BBLOCKS = 8,
	INTERP_OPT_DEFAULT = INTERP_OPT_INLINE | INTERP_OPT_CPROP | INTERP_OPT_BBLOCKS
};

/* Call this from mini_init (), or before it in an embedding scenario, never both. */
MONO_API void mono_ee_interp_init (const char *);

MONO_END_DECLS

#endif /* __MONO_INTERP_H__ */

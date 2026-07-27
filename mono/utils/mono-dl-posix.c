/**
 * \file
 * Interface to the dynamic linker
 *
 * Author:
 *    Mono Team (http://www.mono-project.com)
 *
 * Copyright 2001-2004 Ximian, Inc.
 * Copyright 2004-2009 Novell, Inc.
 * Licensed under the MIT license. See LICENSE file in the project root for full license information.
 */
#include <config.h>

#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif

#if defined(_POSIX_VERSION) && !defined (HOST_WASM)

#include "mono/utils/mono-dl.h"
#include "mono/utils/mono-embed.h"
#include "mono/utils/mono-path.h"

#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>
#include <string.h>
#include <glib.h>
#include <dlfcn.h>

#if !defined (TARGET_MACH)
const char *
mono_dl_get_so_prefix (void)
{
	return "lib";
}
const char **
mono_dl_get_so_suffixes (void)
{
	static const char *suffixes[] = {
		".so",
#if defined (_AIX)
/*
 * libtool generating sysv style names (.so) still results in an .a archive,
 * (AIX prefers to put shared objects inside the same ar archive used for
 * static members, likely as a fat library system) so try common member names
 * when no suffix is given. The same path with member names can be tried, then
 * .a and .so extensions with member names. (The suffixless member names are
 * for when .so names are specified, but the archive is a member - the runtime
 * should just keep trying with one of these suffixes then.)
 *
 * Unfortunately, this strategy won't work for "aix" style libtool sonames -
 * it tries something awful like like "libfoo.a(libfoo.so.9)" which requires
 * you to hardcode a version in the member name.
 */
		"(shr.o)",
		"(shr_64.o)",
		".a(shr.o)",
		".a(shr_64.o)",
		".so(shr.o)",
		".so(shr_64.o)",
#endif
		"",
	};
	return suffixes;
}

int
mono_dl_get_executable_path (char *buf, int buflen)
{
#if defined(HAVE_READLINK)
	return readlink ("/proc/self/exe", buf, buflen - 1);
#else
	return -1;
#endif
}

const char*
mono_dl_get_system_dir (void)
{
	return NULL;
}

#endif

void *
mono_dl_open_file (const char *file, int flags)
{
#ifdef HOST_ANDROID
	/* Bionic doesn't support NULL filenames */
	if (!file)
		return NULL;
	/* The intention of calling `g_file_test (file, G_FILE_TEST_EXISTS)` is
	 * to speed up probing for non-existent libraries.  The problem is that
	 * if file is just a simple "libdl.so" then `dlopen (file)` doesn't just
	 * look for it in the current working directory, it will probe some
	 * other paths too.  (For example on desktop linux you'd also look in
	 * all the directories in LD_LIBRARY_PATH).  So the g_file_test() call
	 * is not a robust way to avoid calling dlopen if the filename is
	 * relative.
	 */
	if (g_path_is_absolute (file) && !g_file_test (file, G_FILE_TEST_EXISTS))
		return NULL;
#endif
#if defined(_AIX)
	/*
	 * dlopen is /weird/ on AIX 
	 * shared libraries (really, all oobjects are, since PPC is PIC)
	 * can cohabitate with not just SOs of the other arch, but also
	 * with regular objects in an archive used for static linking
	 * 
	 * we have to pass RTLD_MEMBER, otherwise lib.a(lib.o) doesn't work
	 */
	return dlopen (file, flags | RTLD_MEMBER);
#else
	return dlopen (file, flags);
#endif
}

void
mono_dl_close_handle (MonoDl *module)
{
	dlclose (module->handle);
}

void*
mono_dl_lookup_symbol (MonoDl *module, const char *name)
{
	return dlsym (module->handle, name);
}

int
mono_dl_convert_flags (int mono_flags, int native_flags)
{
	int lflags = native_flags;

#ifdef ENABLE_NETCORE
	// Specifying both will default to LOCAL
	if (mono_flags & MONO_DL_GLOBAL && !(mono_flags & MONO_DL_LOCAL))
		lflags |= RTLD_GLOBAL;
	else 
		lflags |= RTLD_LOCAL;
#else
	lflags = mono_flags & MONO_DL_LOCAL ? RTLD_LOCAL : RTLD_GLOBAL;
#endif

	if (mono_flags & MONO_DL_LAZY)
		lflags |= RTLD_LAZY;
	else
		lflags |= RTLD_NOW;

	return lflags;
}

char*
mono_dl_current_error_string (void)
{
	return g_strdup (dlerror ());
}

/*
 * The helper libraries we dlopen for P/Invoke - libmono-native above all - bind a
 * couple of runtime symbols at load time without linking against us, so they need
 * to find those in the global scope. That is how a normal install looks, where the
 * runtime is the executable or a library someone loaded RTLD_GLOBAL. An embedder
 * that dlopens us with RTLD_LOCAL, which is the default and what Unity does, leaves
 * our symbols invisible and the helper fails to load at all.
 *
 * Re-opening ourselves with RTLD_GLOBAL moves them into the global scope. RTLD_NOLOAD
 * means we look up the copy that is already mapped rather than pulling in a second
 * one, and we deliberately keep the handle: dropping it can undo the promotion, and
 * the runtime is staying for the life of the process regardless.
 */
void
mono_dl_ensure_self_global (void)
{
	static gboolean done = FALSE;
	Dl_info info;

	if (done)
		return;
	done = TRUE;

	if (!dladdr ((void *) &mono_dl_ensure_self_global, &info) || !info.dli_fname)
		return;

	dlopen (info.dli_fname, RTLD_LAZY | RTLD_NOLOAD | RTLD_GLOBAL);
}

#else

#include <mono/utils/mono-compiler.h>

MONO_EMPTY_SOURCE_FILE (mono_dl_posix);

#endif

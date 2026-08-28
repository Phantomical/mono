/*
 * Loads the shared runtime with dlopen and resolves one symbol out of it.
 *
 * An embedder that hosts mono inside a process it did not start loads the
 * runtime this way - Unity does. That path has a limit nothing else here meets:
 * the mono_tls_* variables are initial-exec, so ld.so must place the module's
 * whole TLS block in the small static surplus it keeps for dlopen'd modules,
 * and a module that does not fit is refused with "cannot allocate memory in
 * static TLS block". One oversized thread_local anywhere in the runtime is
 * enough to cross it.
 *
 * Every other test in the tree links the runtime instead, so without this one a
 * runtime no embedder can load still passes the suite.
 *
 * Windows has no static TLS surplus to run out of -- its loader grows the TLS
 * array for a module loaded at run time -- but the rest of what this checks
 * holds there too: that the library loads with every import bound, and that it
 * answers for the runtime's entry point.
 */

#include <config.h>
#include <stdio.h>

#ifdef HOST_WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

int
main (int argc, char **argv)
{
	void *symbol;

	if (argc != 2) {
		fprintf (stderr, "usage: %s <path to the runtime library>\n", argv [0]);
		return 2;
	}

#ifdef HOST_WIN32
	{
		HMODULE handle = LoadLibraryA (argv [1]);

		if (handle == NULL) {
			fprintf (stderr, "LoadLibrary (%s) failed: %lu\n", argv [1],
			         (unsigned long) GetLastError ());
			return 1;
		}

		/* The loader bound every import before it answered. This says the
		 * library is a mono runtime rather than something else of that name. */
		symbol = (void *) GetProcAddress (handle, "mono_jit_init_version");
		if (symbol == NULL) {
			fprintf (stderr, "%s has no mono_jit_init_version: %lu\n", argv [1],
			         (unsigned long) GetLastError ());
			return 1;
		}
	}
#else
	{
		void *handle = dlopen (argv [1], RTLD_NOW | RTLD_LOCAL);

		if (handle == NULL) {
			fprintf (stderr, "dlopen (%s) failed: %s\n", argv [1], dlerror ());
			return 1;
		}

		/* RTLD_NOW already bound every relocation. This says the library that
		 * answered is a mono runtime rather than something else of that name. */
		symbol = dlsym (handle, "mono_jit_init_version");
		if (symbol == NULL) {
			fprintf (stderr, "%s has no mono_jit_init_version: %s\n", argv [1], dlerror ());
			return 1;
		}
	}
#endif

	printf ("loaded %s\n", argv [1]);

	/* Deliberately not closed: the runtime does not expect to be unloaded,
	 * and the test has its answer already. */
	return 0;
}

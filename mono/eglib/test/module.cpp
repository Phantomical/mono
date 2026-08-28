#include <config.h>

#include <glib.h>
#include <stdio.h>

#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif

#if defined (G_OS_WIN32)
#ifndef PSAPI_VERSION
#define PSAPI_VERSION 2 // Use the Windows 7 or newer version more directly.
#endif
#include <windows.h>
#include <wchar.h>
#include <psapi.h>
#define EXTERNAL_SYMBOL "GetProcAddress"
#else
#define EXTERNAL_SYMBOL "system"
#endif

#include <glib.h>
#include <gmodule.h>
#include <gtest/gtest.h>

/* Looked up below through the main module, so it has to survive --gc-sections. */
extern "C" void G_MODULE_EXPORT
dummy_test_export (void);

void G_MODULE_EXPORT
dummy_test_export (void)
{
}

/* test for g_module_open (NULL, ...) */
TEST (module, symbol_null)
{
	gpointer proc = GINT_TO_POINTER (42);

	GModule *m = g_module_open (NULL, G_MODULE_BIND_LAZY);
	ASSERT_NE (nullptr, m) << "bind to main module failed";

	ASSERT_FALSE (g_module_symbol (m, "__unlikely_\nexistent__", &proc))
		<< "non-existent symbol lookup should fail";
	ASSERT_EQ (nullptr, proc) << "a failed lookup must clear the out-parameter";

	ASSERT_TRUE (g_module_symbol (m, EXTERNAL_SYMBOL, &proc)) << "external lookup failed";
	ASSERT_NE (nullptr, proc) << "external lookup failed";

	ASSERT_TRUE (g_module_symbol (m, "dummy_test_export", &proc)) << "in-proc lookup failed";
	ASSERT_NE (nullptr, proc) << "in-proc lookup failed";

	ASSERT_TRUE (g_module_close (m)) << "close failed";
}

/*
 * mono_get_module_filename and friends have to agree with the Win32 calls they
 * wrap, including where those truncate.  There is nothing to compare against
 * anywhere else.
 */
TEST (module, get_module_filename)
{
#if defined (G_OS_WIN32)
	const HMODULE mods [ ] = {NULL, LoadLibraryW (L"msvcrt.dll"), (HMODULE)(gssize)-1 };

	for (int i = 0; i < G_N_ELEMENTS (mods); ++i) {
		const HMODULE mod = mods [i];
		for (int j = 0; j <= 2; ++j) {
			wchar_t* str = { 0 };
			guint32 length = { 0 };
			wchar_t buf2 [999] = { 0 };
			wchar_t buf3 [2] = { 0 };
			gboolean success = { 0 };
			guint32 length2 = { 0 };
			gboolean success2 = { 0 };
			guint32 length3 = { 0 };
			gboolean success3 = { 0 };

			switch (j) {
			case 0:
				success = mono_get_module_filename (mod, &str, &length);
				length2 = GetModuleFileNameW (mod, buf2, G_N_ELEMENTS (buf2)); // large buf
				length3 = GetModuleFileNameW (mod, buf3, 1); // small buf
				break;
			case 1:
				success = mono_get_module_filename_ex (GetCurrentProcess (), mods [i], &str, &length);
				length2 = GetModuleFileNameExW (GetCurrentProcess (), mod, buf2, G_N_ELEMENTS (buf2)); // large buf
				length3 = GetModuleFileNameExW (GetCurrentProcess (), mod, buf3, 1); // small buf
				break;
			case 2:
				success = mono_get_module_basename (GetCurrentProcess (), mod, &str, &length);
				length2 = GetModuleBaseNameW (GetCurrentProcess (), mod, buf2, G_N_ELEMENTS (buf2)); // large buf
				length3 = GetModuleBaseNameW (GetCurrentProcess (), mod, buf3, 1); // small buf
				break;
			}
			success2 = length2 && length2 < G_N_ELEMENTS (buf2);
			success3 = length3 == 1;
			ASSERT_EQ (success2, success);
			ASSERT_TRUE (success == success3 || j > 0);
			ASSERT_TRUE (!success || str [0] == buf2 [0]);
			ASSERT_TRUE (length3 == 0 || length3 == 1);
			ASSERT_EQ (success2 ? wcslen (buf2) : 0, length);
			ASSERT_TRUE (!success || !wcscmp (str, buf2));
			ASSERT_TRUE (!success || str);
			g_free (str);
		}
	}
#else
	GTEST_SKIP () << "Win32 only";
#endif
}

TEST (module, get_current_directory)
{
#if defined (G_OS_WIN32)
	wchar_t* str = { 0 };
	guint32 length = { 0 };
	gboolean success = mono_get_current_directory (&str, &length);
	wchar_t buf2 [999] = { 0 };
	const int length2 = GetCurrentDirectoryW (G_N_ELEMENTS (buf2), buf2);
	const gboolean success2 = length2 && length2 < G_N_ELEMENTS (buf2);
	wchar_t buf3 [2] = { 0 };
	const int length3 = GetCurrentDirectoryW (G_N_ELEMENTS (buf3), buf3);
	const gboolean success3 = length3 > 0;

	ASSERT_EQ (length2, length);
	ASSERT_EQ (success2, success);
	ASSERT_EQ (success3, success);
	ASSERT_TRUE (!success || !wcscmp (str, buf2));
	ASSERT_TRUE (!success || str);
	g_free (str);
#else
	GTEST_SKIP () << "Win32 only";
#endif
}

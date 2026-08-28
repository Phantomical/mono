/*
 * Directory utility functions.
 *
 * Author:
 *   Gonzalo Paniagua Javier (gonzalo@novell.com)
 *
 * (C) 2006 Novell, Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE
 * LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION
 * OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
 * WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */
#include "config.h"
#include <glib.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include "../utils/mono-errno.h"
#include <sys/types.h>
#include <sys/stat.h>
#include <io.h>

#include <winsock2.h>

struct _GDir {
	HANDLE handle;
	gchar* current;
	gchar* next;
};

GDir *
g_dir_open (const gchar *path, guint flags, GError **gerror)
{
	GDir *dir;
	gunichar2* path_utf16;
	gunichar2* path_utf16_search;
	WIN32_FIND_DATAW find_data;

	g_return_val_if_fail (path != NULL, NULL);
	g_return_val_if_fail (gerror == NULL || *gerror == NULL, NULL);

	dir = g_new0 (GDir, 1);
	path_utf16 = u8to16 (path);
	path_utf16_search = g_malloc ((wcslen(path_utf16) + 3)*sizeof(gunichar2));
	wcscpy (path_utf16_search, path_utf16);
	wcscat (path_utf16_search, L"\\*");

	dir->handle = FindFirstFileW (path_utf16_search, &find_data);
	if (dir->handle == INVALID_HANDLE_VALUE) {
		if (gerror) {
			gint err = errno;
			*gerror = g_error_new (G_LOG_DOMAIN, g_file_error_from_errno (err), strerror (err));
		}
		g_free (path_utf16_search);
		g_free (path_utf16);
		g_free (dir);
		return NULL;
	}
	g_free (path_utf16_search);
	g_free (path_utf16);

	while ((wcscmp (find_data.cFileName, L".") == 0) || (wcscmp (find_data.cFileName, L"..") == 0)) {
		if (!FindNextFileW (dir->handle, &find_data)) {
			if (gerror) {
				gint err = errno;
				*gerror = g_error_new (G_LOG_DOMAIN, g_file_error_from_errno (err), strerror (err));
			}
			g_free (dir);
			return NULL;
		}
	}

	dir->current = NULL;
	dir->next = u16to8 (find_data.cFileName);
	return dir;
}

const gchar *
g_dir_read_name (GDir *dir)
{
	WIN32_FIND_DATAW find_data;

	g_return_val_if_fail (dir != NULL && dir->handle != 0, NULL);

	if (dir->current)
		g_free (dir->current);
	dir->current = NULL;

	dir->current = dir->next;

	if (!dir->current)
		return NULL;

	dir->next = NULL;

	do {
		if (!FindNextFileW (dir->handle, &find_data)) {
			dir->next = NULL;
			return dir->current;
		}
	} while ((wcscmp (find_data.cFileName, L".") == 0) || (wcscmp (find_data.cFileName, L"..") == 0));

	dir->next = u16to8 (find_data.cFileName);
	return dir->current;
}

void
g_dir_rewind (GDir *dir)
{
}

void
g_dir_close (GDir *dir)
{
	g_return_if_fail (dir != NULL && dir->handle != 0);

	if (dir->current)
		g_free (dir->current);
	dir->current = NULL;
	if (dir->next)
		g_free (dir->next);
	dir->next = NULL;
	FindClose (dir->handle);
	dir->handle = 0;
	g_free (dir);
}

/*
 * The mode argument is POSIX's and there is no Windows equivalent: a directory
 * created here inherits its parent's ACL, which is what CreateDirectory does
 * with a null descriptor.
 *
 * A component that already exists is not an error, and neither is a prefix that
 * names a drive or a UNC share -- those are not directories anyone creates, and
 * CreateDirectory answers ERROR_ACCESS_DENIED rather than ERROR_ALREADY_EXISTS
 * for them.
 */
int
g_mkdir_with_parents (const gchar *pathname, int mode)
{
	gunichar2 *path;
	size_t i;

	if (!pathname || *pathname == '\0') {
		mono_set_errno (EINVAL);
		return -1;
	}

	path = u8to16 (pathname);

	for (i = 0; ; i++) {
		gunichar2 orig;

		if (path [i] != L'\\' && path [i] != L'/' && path [i] != L'\0')
			continue;

		/* A leading separator, and the one after a drive letter or a UNC
		 * prefix, close off nothing there is to create. */
		if (i == 0 || path [i - 1] == L':' || path [i - 1] == L'\\'
		    || path [i - 1] == L'/') {
			if (path [i] == L'\0')
				break;
			continue;
		}

		orig = path [i];
		path [i] = L'\0';

		if (!CreateDirectoryW (path, NULL)) {
			DWORD err = GetLastError ();

			if (err != ERROR_ALREADY_EXISTS && err != ERROR_ACCESS_DENIED) {
				path [i] = orig;
				g_free (path);
				mono_set_errno (err == ERROR_PATH_NOT_FOUND ? ENOENT : EACCES);
				return -1;
			}
		}

		path [i] = orig;
		if (orig == L'\0')
			break;
	}

	g_free (path);
	return 0;
}



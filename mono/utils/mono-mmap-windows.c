/**
 * \file
 * Windows support for mapping code into the process address space
 *
 * Author:
 *   Mono Team (mono-list@lists.ximian.com)
 *
 * Copyright 2001-2008 Novell, Inc.
 * Licensed under the MIT license. See LICENSE file in the project root for full license information.
 */

#include <config.h>
#include <glib.h>

#if defined(HOST_WIN32)
#include <io.h>
#include <windows.h>
#include <mono/utils/mono-counters.h>
#include "mono/utils/mono-mmap.h"
#include "mono/utils/mono-mmap-internals.h"
#include <mono/utils/w32subset.h>

static void *malloced_shared_area;

int
mono_pagesize (void)
{
	SYSTEM_INFO info;
	static int saved_pagesize = 0;
	if (saved_pagesize)
		return saved_pagesize;
	GetSystemInfo (&info);
	saved_pagesize = info.dwPageSize;
	return saved_pagesize;
}

int
mono_valloc_granule (void)
{
	SYSTEM_INFO info;
	static int saved_valloc_granule = 0;
	if (saved_valloc_granule)
		return saved_valloc_granule;
	GetSystemInfo (&info);
	saved_valloc_granule = info.dwAllocationGranularity;
	return saved_valloc_granule;
}

int
mono_mmap_win_prot_from_flags (int flags)
{
	int prot = flags & (MONO_MMAP_READ|MONO_MMAP_WRITE|MONO_MMAP_EXEC);
	switch (prot) {
	case 0: prot = PAGE_NOACCESS; break;
	case MONO_MMAP_READ: prot = PAGE_READONLY; break;
	case MONO_MMAP_READ|MONO_MMAP_EXEC: prot = PAGE_EXECUTE_READ; break;
	case MONO_MMAP_READ|MONO_MMAP_WRITE: prot = PAGE_READWRITE; break;
	case MONO_MMAP_READ|MONO_MMAP_WRITE|MONO_MMAP_EXEC: prot = PAGE_EXECUTE_READWRITE; break;
	case MONO_MMAP_WRITE: prot = PAGE_READWRITE; break;
	case MONO_MMAP_WRITE|MONO_MMAP_EXEC: prot = PAGE_EXECUTE_READWRITE; break;
	case MONO_MMAP_EXEC: prot = PAGE_EXECUTE; break;
	default:
		g_assert_not_reached ();
	}
	return prot;
}

/**
 * mono_setmmapjit:
 * \param flag indicating whether to enable or disable the use of MAP_JIT in mmap
 *
 * Call this method to enable or disable the use of MAP_JIT to create the pages
 * for the JIT to use.   This is only needed for scenarios where Mono is bundled
 * as an App in MacOS
 */
void
mono_setmmapjit (int flag)
{
	/* Ignored on HOST_WIN32 */
}

/*
 * Any two addresses below this are within a signed 32-bit displacement of each
 * other.  Generated code branches and reaches its own data that way, so the
 * code manager asks for MONO_MMAP_32BIT to stay under it.
 */
#define MONO_MMAP_32BIT_LIMIT ((uintptr_t) 0x80000000)

/*
 * Reserves `length` bytes below MONO_MMAP_32BIT_LIMIT.
 *
 * There is no VirtualAlloc flag for this the way there is a MAP_32BIT for
 * mmap, so the free regions below the boundary are walked and the first one
 * that fits is taken.  A caller that gets NULL is out of that range, which is
 * what it wants to hear: an address anywhere else would be a branch that
 * silently does not reach.
 */
static void *
valloc_below_2gb (size_t length, DWORD mflags, DWORD prot)
{
	const uintptr_t limit = MONO_MMAP_32BIT_LIMIT;
	SYSTEM_INFO si;
	uintptr_t granularity, at;

	GetSystemInfo (&si);
	granularity = (uintptr_t) si.dwAllocationGranularity;
	if (granularity == 0)
		return NULL;

	at = (uintptr_t) si.lpMinimumApplicationAddress;
	at = (at + granularity - 1) & ~(granularity - 1);

	while (at != 0 && at + length <= limit) {
		MEMORY_BASIC_INFORMATION mbi;
		uintptr_t next;

		if (VirtualQuery ((void *) at, &mbi, sizeof (mbi)) == 0)
			break;

		next = (uintptr_t) mbi.BaseAddress + mbi.RegionSize;

		if (mbi.State == MEM_FREE && next - at >= length) {
			void *ptr = VirtualAlloc ((void *) at, length, mflags, prot);

			if (ptr != NULL)
				return ptr;

			/*
			 * Another thread reserved this address between the query
			 * and the request. The rest of the region is still free,
			 * and a region here is routinely most of the low 2GB, so
			 * step one granule. Stepping over the region instead
			 * gives up the whole range on a single lost race.
			 */
			next = at + granularity;
		}

		/* A region shorter than the granularity would leave `at` where it is. */
		next = (next + granularity - 1) & ~(granularity - 1);
		if (next <= at)
			break;
		at = next;
	}

	return NULL;
}

void*
mono_valloc (void *addr, size_t length, int flags, MonoMemAccountType type)
{
	if (!mono_valloc_can_alloc (length))
		return NULL;

	void *ptr;
	int mflags = MEM_RESERVE|MEM_COMMIT;
	int prot = mono_mmap_win_prot_from_flags (flags);
	/* translate the flags */

	if (flags & MONO_MMAP_32BIT) {
		uintptr_t want = (uintptr_t) addr;

		/*
		 * The boundary is a constraint and the preferred address only a
		 * hint, so a hint that puts any of the range at or past the
		 * boundary is dropped.  The code manager asks for the address
		 * behind its last chunk, and honouring that one once the chunks
		 * have grown up to the boundary is what puts code out of reach.
		 */
		if (addr != NULL
		    && (want >= MONO_MMAP_32BIT_LIMIT
		        || length > MONO_MMAP_32BIT_LIMIT - want))
			addr = NULL;

		ptr = addr != NULL ? VirtualAlloc (addr, length, mflags, prot)
		                   : valloc_below_2gb (length, (DWORD) mflags, (DWORD) prot);
	} else {
		ptr = VirtualAlloc (addr, length, mflags, prot);
	}

	if (ptr == NULL)
		return NULL;

	mono_account_mem (type, (ssize_t)length);

	return ptr;
}

void*
mono_valloc_aligned (size_t length, size_t alignment, int flags, MonoMemAccountType type)
{
	int prot = mono_mmap_win_prot_from_flags (flags);
	char *mem = (char*)VirtualAlloc (NULL, length + alignment, MEM_RESERVE, prot);
	char *aligned;

	if (!mem)
		return NULL;

	if (!mono_valloc_can_alloc (length))
		return NULL;

	aligned = mono_aligned_address (mem, length, alignment);

	aligned = (char*)VirtualAlloc (aligned, length, MEM_COMMIT, prot);
	g_assert (aligned);

	mono_account_mem (type, (ssize_t)length);

	return aligned;
}

int
mono_vfree (void *addr, size_t length, MonoMemAccountType type)
{
	MEMORY_BASIC_INFORMATION mbi;
	SIZE_T query_result = VirtualQuery (addr, &mbi, sizeof (mbi));
	BOOL res;

	g_assert (query_result);

	res = VirtualFree (mbi.AllocationBase, 0, MEM_RELEASE);

	g_assert (res);

	mono_account_mem (type, -(ssize_t)length);

	return 0;
}

#if HAVE_API_SUPPORT_WIN32_FILE_MAPPING || HAVE_API_SUPPORT_WIN32_FILE_MAPPING_FROM_APP
static void
remove_trailing_whitespace_utf8 (gchar *s)
{
	glong length = g_utf8_strlen (s, -1);
	glong const original_length = length;
	while (length > 0 && g_ascii_isspace (s [length - 1]))
		--length;
	if (length != original_length)
		s [length] = 0;
}

#if HAVE_API_SUPPORT_WIN32_FORMAT_MESSAGE
gchar *
format_win32_error_string (gint32 win32_error)
{
	gchar *ret = NULL;
#if HAVE_API_SUPPORT_WIN32_LOCAL_ALLOC_FREE
	PWSTR buf = NULL;
	if (FormatMessageW (FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_IGNORE_INSERTS, NULL,
		win32_error, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), (PWSTR)&buf, 0, NULL)) {
		ret = u16to8 (buf);
		LocalFree (buf);
	}
#else
	WCHAR local_buf [1024];
	if (!FormatMessageW (FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, NULL,
		win32_error, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), local_buf, G_N_ELEMENTS (local_buf) - 1, NULL) )
		local_buf [0] = TEXT('\0');

	ret = u16to8 (local_buf)
#endif
	if (ret)
		remove_trailing_whitespace_utf8 (ret);
	return ret;
}
#elif !HAVE_EXTERN_DEFINED_WIN32_FORMAT_MESSAGE
gchar *
format_win32_error_string (gint32 error_code)
{
	return g_strdup_printf ("GetLastError=%d. FormatMessage not supported.", GetLastError ());
}
#endif /* HAVE_API_SUPPORT_WIN32_FORMAT_MESSAGE */

void*
mono_file_map_error (size_t length, int flags, int fd, guint64 offset, void **ret_handle,
	const char *filepath, char **error_message)
{
	void *ptr = NULL;
	HANDLE mapping = NULL;
	int const prot = mono_mmap_win_prot_from_flags (flags);
	/* translate the flags */
	/*if (flags & MONO_MMAP_PRIVATE)
		mflags |= MAP_PRIVATE;
	if (flags & MONO_MMAP_SHARED)
		mflags |= MAP_SHARED;
	if (flags & MONO_MMAP_ANON)
		mflags |= MAP_ANONYMOUS;
	if (flags & MONO_MMAP_FIXED)
		mflags |= MAP_FIXED;
	if (flags & MONO_MMAP_32BIT)
		mflags |= MAP_32BIT;*/
	int const mflags = (flags & MONO_MMAP_WRITE) ? FILE_MAP_COPY : FILE_MAP_READ;
	HANDLE const file = (HANDLE)_get_osfhandle (fd);
	const char *failed_function = NULL;

	// The size of the mapping is the maximum file offset to map.
	//
	// It is not, as you might expect, the maximum view size to be mapped from it.
	//
	// If it were the maximum view size, the size parameter would have just
	// been one DWORD in 32bit Windows, expanded to SIZE_T in 64bit Windows.
	// It is 64bits even on 32bit Windows to allow large files.
	//
	// See https://docs.microsoft.com/en-us/windows/desktop/Memory/creating-a-file-mapping-object.
	const guint64 mapping_length = offset + length;

#if HAVE_API_SUPPORT_WIN32_FILE_MAPPING

	failed_function = "CreateFileMapping";
	mapping = CreateFileMappingW (file, NULL, prot, (DWORD)(mapping_length >> 32), (DWORD)mapping_length, NULL);
	if (mapping == NULL)
		goto exit;

	failed_function = "MapViewOfFile";
	ptr = MapViewOfFile (mapping, mflags, (DWORD)(offset >> 32), (DWORD)offset, length);
	if (ptr == NULL)
		goto exit;

#elif HAVE_API_SUPPORT_WIN32_FILE_MAPPING_FROM_APP

	failed_function = "CreateFileMappingFromApp";
	mapping = CreateFileMappingFromApp (file, NULL, prot, mapping_length, NULL);
	if (mapping == NULL)
		goto exit;

	failed_function = "MapViewOfFileFromApp";
	ptr = MapViewOfFileFromApp (mapping, mflags, offset, length);
	if (ptr == NULL)
		goto exit;

#else
#error unknown Windows variant
#endif

exit:
	if (!ptr && (mapping || error_message)) {
		int const win32_error = GetLastError ();
		if (mapping)
			CloseHandle (mapping);
		if (error_message) {
			gchar *win32_error_string = format_win32_error_string (win32_error);
			*error_message = g_strdup_printf ("%s failed file:%s length:0x%IX offset:0x%I64X function:%s error:%s(0x%X)\n",
				__func__, filepath ? filepath : "", length, offset, failed_function, win32_error_string, win32_error);
			g_free (win32_error_string);
		}
		SetLastError (win32_error);
	}
	*ret_handle = mapping;
	return ptr;
}

int
mono_file_unmap (void *addr, void *handle)
{
	UnmapViewOfFile (addr);
	CloseHandle (handle);
	return 0;
}
#elif !HAVE_EXTERN_DEFINED_WIN32_FILE_MAPPING && !HAVE_EXTERN_DEFINED_WIN32_FILE_MAPPING_FROM_APP
void*
mono_file_map_error (size_t length, int flags, int fd, guint64 offset, void **ret_handle,
	const char *filepath, char **error_message)
{
	g_unsupported_api ("CreateFileMapping");
	*ret_handle = NULL;
	SetLastError (ERROR_NOT_SUPPORTED);
	return NULL;
}

int
mono_file_unmap (void *addr, void *handle)
{
	g_unsupported_api ("UnmapViewOfFile");
	SetLastError (ERROR_NOT_SUPPORTED);

	return 0;
}
#endif /* HAVE_API_SUPPORT_WIN32_FILE_MAPPING || HAVE_API_SUPPORT_WIN32_FILE_MAPPING_FROM_APP */

void*
mono_file_map (size_t length, int flags, int fd, guint64 offset, void **ret_handle)
{
	return mono_file_map_error (length, flags, fd, offset, ret_handle, NULL, NULL);
}

int
mono_mprotect (void *addr, size_t length, int flags)
{
	DWORD oldprot;
	int prot = mono_mmap_win_prot_from_flags (flags);

	if (flags & MONO_MMAP_DISCARD) {
		VirtualFree (addr, length, MEM_DECOMMIT);
		VirtualAlloc (addr, length, MEM_COMMIT, prot);
		return 0;
	}
	return VirtualProtect (addr, length, prot, &oldprot) == 0;
}

void*
mono_shared_area (void)
{
	if (!malloced_shared_area)
		malloced_shared_area = mono_malloc_shared_area (0);
	/* get the pid here */
	return malloced_shared_area;
}

void
mono_shared_area_remove (void)
{
	if (malloced_shared_area)
		g_free (malloced_shared_area);
	malloced_shared_area = NULL;
}

void*
mono_shared_area_for_pid (void *pid)
{
	return NULL;
}

void
mono_shared_area_unload (void *area)
{
}

int
mono_shared_area_instances (void **array, int count)
{
	return 0;
}

#else

#include <mono/utils/mono-compiler.h>

MONO_EMPTY_SOURCE_FILE (mono_mmap_windows);

#endif // HOST_WIN32

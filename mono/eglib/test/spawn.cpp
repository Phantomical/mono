#include <config.h>

#include <stdio.h>
#include <string.h>

#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif
#ifdef G_OS_UNIX
#include <sys/types.h>
#include <sys/wait.h>
#endif

#include <glib.h>
#include <gtest/gtest.h>

#ifdef G_OS_WIN32
#include <io.h>
#define read _read
#define close _close
#endif

TEST (spawn, command_line_sync)
{
#if HAVE_G_SPAWN
	gchar *out = NULL;
	gchar *err = NULL;
	gint status = -1;
	GError *gerror = NULL;

	/*
	 * g_spawn forks, and the intermediate child leaves through exit() rather
	 * than _exit(), so it flushes whatever the parent had sitting in its stdio
	 * buffers.  With stdout on a pipe -- which is how ctest runs this -- that
	 * duplicates every line printed so far into the middle of the output.
	 */
	fflush (NULL);

	ASSERT_TRUE (g_spawn_command_line_sync ("ls", &out, &err, &status, &gerror))
		<< "Error executing 'ls'";
	ASSERT_EQ (0, status);
	ASSERT_NE (nullptr, out) << "Didn't get any output from ls!?";
	ASSERT_NE (0u, strlen (out)) << "Didn't get any output from ls!?";

	g_free (out);
	g_free (err);
#else
	GTEST_SKIP () << "no g_spawn on this platform";
#endif
}

TEST (spawn, async_with_pipes)
{
#if HAVE_G_SPAWN
	char *argv [15];
	int stdout_fd = -1;
	char buffer [512];
	GPid child_pid = 0;

	memset (argv, 0, sizeof (argv));
	argv [0] = (char*)"ls";

	fflush (NULL);

	ASSERT_TRUE (g_spawn_async_with_pipes (NULL, argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL,
					       &child_pid, NULL, &stdout_fd, NULL, NULL))
		<< "Failed to run ls";
	ASSERT_NE (0, child_pid) << "child pid not returned";
	ASSERT_NE (-1, stdout_fd) << "out fd is -1";

	while (read (stdout_fd, buffer, sizeof (buffer)) > 0)
		;
	close (stdout_fd);
#else
	GTEST_SKIP () << "no g_spawn on this platform";
#endif
}

#include <config.h>

#include <stdlib.h>
#include <string.h>

#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif

#include <glib.h>
#include <gtest/gtest.h>

#ifdef G_OS_WIN32
#include <io.h>
#define close _close
#endif

TEST (file, get_contents)
{
#ifdef G_OS_WIN32
	const gchar *filename = "c:\\Windows\\system.ini";
#elif defined(__PASE__)
	/* Most etc files don't exist in PASE. Try one that should exist. */
	const gchar *filename = "/etc/magic";
#else
	const gchar *filename = "/etc/hosts";
#endif
	GError *gerror = NULL;
	gchar *content;
	gsize length;

	ASSERT_FALSE (g_file_get_contents ("", &content, NULL, &gerror))
		<< "the empty file name must not open anything";
	ASSERT_NE (nullptr, gerror) << "Got nothing as error.";
	ASSERT_EQ (nullptr, content) << "Content is uninitialized";
	g_error_free (gerror);

	gerror = NULL;
	ASSERT_TRUE (g_file_get_contents (filename, &content, &length, &gerror))
		<< "reading " << filename << ": " << (gerror ? gerror->message : "");
	ASSERT_EQ (nullptr, gerror) << "Got an error returning TRUE";
	ASSERT_NE (nullptr, content) << "Content is NULL";
	ASSERT_EQ (length, strlen (content));
	g_free (content);
}

TEST (file, open_tmp)
{
	GError *gerror = NULL;
	gchar *name = (gchar *) GINT_TO_POINTER (-1);
	gint fd;

	/*
	 * Okay, this works, but creates a .xxx file in /tmp on every run. Disabled.
	 * fd = g_file_open_tmp (NULL, NULL, NULL);
	 */
	fd = g_file_open_tmp ("invalidtemplate", NULL, &gerror);
	ASSERT_EQ (-1, fd) << "The template was invalid and accepted";
	ASSERT_NE (nullptr, gerror) << "No error returned.";
	g_error_free (gerror);

	gerror = NULL;
	fd = g_file_open_tmp ("i/nvalidtemplate", &name, &gerror);
	ASSERT_EQ (-1, fd) << "The template was invalid and accepted";
	ASSERT_NE (nullptr, gerror) << "No error returned.";
	ASSERT_NE (nullptr, name) << "'name' is not reset";
	g_error_free (gerror);

	gerror = NULL;
	fd = g_file_open_tmp ("valid-XXXXXX", &name, &gerror);
	ASSERT_NE (-1, fd) << "This should be valid";
	ASSERT_EQ (nullptr, gerror);
	ASSERT_NE (nullptr, name) << "No name returned.";
	close (fd);
	unlink (name);
	g_free (name);
}

TEST (file, file_test)
{
	const gchar *tmp = g_get_tmp_dir ();

	ASSERT_FALSE (g_file_test (NULL, G_FILE_TEST_EXISTS)) << "a NULL name tests as nothing";
	ASSERT_FALSE (g_file_test (tmp, (GFileTest) 0)) << "an empty test set is always FALSE";
	ASSERT_FALSE (g_file_test ("__no_such_file_here__", G_FILE_TEST_EXISTS));

	ASSERT_TRUE (g_file_test (tmp, G_FILE_TEST_EXISTS)) << "tmp does not exist.";
	ASSERT_FALSE (g_file_test (tmp, G_FILE_TEST_IS_REGULAR)) << "tmp is regular";
	ASSERT_TRUE (g_file_test (tmp, G_FILE_TEST_IS_DIR)) << "tmp is not a directory";
	ASSERT_TRUE (g_file_test (tmp, G_FILE_TEST_IS_EXECUTABLE)) << "tmp is not executable";

	ASSERT_TRUE (g_file_test (tmp, G_FILE_TEST_EXISTS | G_FILE_TEST_IS_SYMLINK));
	ASSERT_FALSE (g_file_test (tmp, G_FILE_TEST_IS_REGULAR | G_FILE_TEST_IS_SYMLINK));
	ASSERT_TRUE (g_file_test (tmp, G_FILE_TEST_IS_DIR | G_FILE_TEST_IS_SYMLINK));
	ASSERT_TRUE (g_file_test (tmp, G_FILE_TEST_IS_EXECUTABLE | G_FILE_TEST_IS_SYMLINK));

	gchar *path;
	close (g_file_open_tmp (NULL, &path, NULL)); /* create an empty file */
	ASSERT_TRUE (g_file_test (path, G_FILE_TEST_EXISTS)) << path << " should exist";
	ASSERT_TRUE (g_file_test (path, G_FILE_TEST_IS_REGULAR)) << path << " is not regular";
	ASSERT_FALSE (g_file_test (path, G_FILE_TEST_IS_DIR));
	ASSERT_FALSE (g_file_test (path, G_FILE_TEST_IS_EXECUTABLE));
	ASSERT_FALSE (g_file_test (path, G_FILE_TEST_IS_SYMLINK));

#ifndef G_OS_WIN32 /* FIXME */
	gchar *sympath = g_strconcat (path, "-link", (const char*)NULL);
	ASSERT_EQ (0, symlink (path, sympath));

	/* A live symlink answers for its target, except for IS_SYMLINK. */
	ASSERT_TRUE (g_file_test (sympath, G_FILE_TEST_EXISTS));
	ASSERT_TRUE (g_file_test (sympath, G_FILE_TEST_IS_REGULAR));
	ASSERT_FALSE (g_file_test (sympath, G_FILE_TEST_IS_DIR));
	ASSERT_FALSE (g_file_test (sympath, G_FILE_TEST_IS_EXECUTABLE));
	ASSERT_TRUE (g_file_test (sympath, G_FILE_TEST_IS_SYMLINK));

	unlink (path);

	/* Dangling, and now only IS_SYMLINK answers. */
	ASSERT_FALSE (g_file_test (sympath, G_FILE_TEST_EXISTS));
	ASSERT_FALSE (g_file_test (sympath, G_FILE_TEST_IS_REGULAR));
	ASSERT_FALSE (g_file_test (sympath, G_FILE_TEST_IS_DIR));
	ASSERT_FALSE (g_file_test (sympath, G_FILE_TEST_IS_EXECUTABLE));
	ASSERT_TRUE (g_file_test (sympath, G_FILE_TEST_IS_SYMLINK));

	unlink (sympath);
	g_free (sympath);
#else
	unlink (path);
#endif
	g_free (path);
}

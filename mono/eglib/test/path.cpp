#include <config.h>

#include <string_view>

#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif

#include <glib.h>
#include <gtest/gtest.h>

#ifdef G_OS_WIN32
#include <direct.h>
#define chdir _chdir
#endif

namespace {

/*
 * Two of these tests reach for process-wide state -- one chdirs, one blanks
 * $PATH -- and the rest of the suite assumes both are where they started.  Put
 * them back whatever the case does, including walking out through a failed
 * assertion.
 */
class path : public ::testing::Test {
protected:
	void SetUp () override
	{
		saved_cwd = g_get_current_dir ();
		const char *env = g_getenv ("PATH");
		saved_path = env ? g_strdup (env) : NULL;
	}

	void TearDown () override
	{
		if (saved_cwd) {
			EXPECT_EQ (0, chdir (saved_cwd));
			g_free (saved_cwd);
		}
		if (saved_path) {
			g_setenv ("PATH", saved_path, TRUE);
			g_free (saved_path);
		}
	}

private:
	char *saved_cwd = NULL;
	char *saved_path = NULL;
};

}

/* This test is just to be used with valgrind */
TEST_F (path, build_path)
{
	char *s;

	s = g_build_path ("/", "hola///", "//mundo", (const char*)NULL);
	ASSERT_STREQ ("hola/mundo", s);
	g_free (s);

	s = g_build_path ("/", "hola/", "/mundo", (const char*)NULL);
	ASSERT_STREQ ("hola/mundo", s);
	g_free (s);

	s = g_build_path ("/", "hola/", "mundo", (const char*)NULL);
	ASSERT_STREQ ("hola/mundo", s);
	g_free (s);

	s = g_build_path ("/", "hola", "/mundo", (const char*)NULL);
	ASSERT_STREQ ("hola/mundo", s);
	g_free (s);

	s = g_build_path ("/", "/hello", "world/", (const char*)NULL);
	ASSERT_STREQ ("/hello/world/", s);
	g_free (s);

	/* Now test multi-char-separators */
	s = g_build_path ("**", "hello", "world", (const char*)NULL);
	ASSERT_STREQ ("hello**world", s);
	g_free (s);

	s = g_build_path ("**", "hello**", "world", (const char*)NULL);
	ASSERT_STREQ ("hello**world", s);
	g_free (s);

	s = g_build_path ("**", "hello**", "**world", (const char*)NULL);
	ASSERT_STREQ ("hello**world", s);
	g_free (s);

	s = g_build_path ("1234567890", "hello", "world", (const char*)NULL);
	ASSERT_STREQ ("hello1234567890world", s);
	g_free (s);

	s = g_build_path ("1234567890", "hello1234567890", "1234567890world", (const char*)NULL);
	ASSERT_STREQ ("hello1234567890world", s);
	g_free (s);

	s = g_build_path ("1234567890", "hello12345678901234567890", "1234567890world", (const char*)NULL);
	ASSERT_STREQ ("hello1234567890world", s);
	g_free (s);

	/* Multiple */
	s = g_build_path ("/", "a", "b", "c", "d", (const char*)NULL);
	ASSERT_STREQ ("a/b/c/d", s);
	g_free (s);

	s = g_build_path ("/", "/a", "", "/c/", (const char*)NULL);
	ASSERT_STREQ ("/a/c/", s);
	g_free (s);

	/* Null */
	s = g_build_path ("/", NULL, (const char*)NULL);
	ASSERT_NE (nullptr, s) << "must get a non-NULL return";
	ASSERT_STREQ ("", s) << "must get an empty string";
	g_free (s);

	// This is to test the regression introduced by Levi for the Windows support
	// that code errouneously read below the allowed area (in this case dir [-1]).
	// and caused all kinds of random errors.
	const char *dir = "//";
	dir++;
	s = g_build_filename (dir, "var/private", (const char*)NULL);
	ASSERT_EQ ('/', s [0]) << "Must have a '/' at the start";
	g_free (s);
}

TEST_F (path, build_filename)
{
	char *s;

	s = g_build_filename ("a", "b", "c", "d", (const char*)NULL);
#ifdef G_OS_WIN32
	ASSERT_STREQ ("a\\b\\c\\d", s);
#else
	ASSERT_STREQ ("a/b/c/d", s);
#endif
	g_free (s);

#ifdef G_OS_WIN32
	s = g_build_filename ("C:\\", "a", (const char*)NULL);
	ASSERT_STREQ ("C:\\a", s);
	g_free (s);
#else
	s = g_build_filename ("/", "a", (const char*)NULL);
	ASSERT_STREQ ("/a", s);
	g_free (s);

	s = g_build_filename ("/", "foo", "/bar", "tolo/", "/meo/", (const char*)NULL);
	ASSERT_STREQ ("/foo/bar/tolo/meo/", s);
	g_free (s);
#endif
}

TEST_F (path, get_dirname)
{
	char *s;

#ifdef G_OS_WIN32
	s = g_path_get_dirname ("c:\\home\\miguel");
	ASSERT_STREQ ("c:\\home", s);
	g_free (s);

	s = g_path_get_dirname ("c:/home/miguel");
	ASSERT_STREQ ("c:/home", s);
	g_free (s);

	s = g_path_get_dirname ("c:\\home\\dingus\\");
	ASSERT_STREQ ("c:\\home\\dingus", s);
	g_free (s);

	s = g_path_get_dirname ("dir.c");
	ASSERT_STREQ (".", s);
	g_free (s);

	s = g_path_get_dirname ("c:\\index.html");
	ASSERT_STREQ ("c:", s);
	g_free (s);
#else
	s = g_path_get_dirname ("/home/miguel");
	ASSERT_STREQ ("/home", s);
	g_free (s);

	s = g_path_get_dirname ("/home/dingus/");
	ASSERT_STREQ ("/home/dingus", s);
	g_free (s);

	s = g_path_get_dirname ("dir.c");
	ASSERT_STREQ (".", s);
	g_free (s);

	s = g_path_get_dirname ("/index.html");
	ASSERT_STREQ ("/", s);
	g_free (s);
#endif
}

TEST_F (path, get_basename)
{
	char *s;

	s = g_path_get_basename ("");
	ASSERT_STREQ (".", s);
	g_free (s);

#ifdef G_OS_WIN32
	s = g_path_get_basename ("c:\\home\\dingus\\");
	ASSERT_STREQ ("dingus", s);
	g_free (s);

	s = g_path_get_basename ("c:/home/dingus/");
	ASSERT_STREQ ("dingus", s);
	g_free (s);

	s = g_path_get_basename ("c:\\home\\dingus");
	ASSERT_STREQ ("dingus", s);
	g_free (s);

	s = g_path_get_basename ("c:/home/dingus");
	ASSERT_STREQ ("dingus", s);
	g_free (s);
#else
	s = g_path_get_basename ("/home/dingus/");
	ASSERT_STREQ ("dingus", s);
	g_free (s);

	s = g_path_get_basename ("/home/dingus");
	ASSERT_STREQ ("dingus", s);
	g_free (s);
#endif
}

TEST_F (path, find_program_in_path)
{
#ifdef G_OS_WIN32
	const gchar *searchfor = "explorer.exe";
#else
	const gchar *searchfor = "ls";
#endif

	char *s = g_find_program_in_path (searchfor);
	ASSERT_NE (nullptr, s) << "No " << searchfor << " on this system?";
	g_free (s);
}

/*
 * With $PATH empty the only place left to look is the working directory, which
 * is why these tests run from the directory the binary was linked into.
 */
TEST_F (path, find_program_in_path_cwd)
{
#ifdef G_OS_WIN32
	const gchar *searchfor = "test-eglib.exe";
#else
	const gchar *searchfor = "test-eglib";
#endif

	g_setenv ("PATH", "", TRUE);

	char *s = g_find_program_in_path ("ls");
	ASSERT_EQ (nullptr, s) << "Found something interesting here: " << (s ? s : "");
	g_free (s);

	s = g_find_program_in_path (searchfor);
	ASSERT_NE (nullptr, s) << "It should find '" << searchfor << "' in the current directory.";
	g_free (s);
}

TEST_F (path, cwd)
{
#ifdef DISABLE_FILESYSTEM_TESTS
	GTEST_SKIP () << "filesystem tests are disabled";
#else
#ifdef G_OS_WIN32
	const gchar *newdir = "C:\\Windows";
#else
	/*
	 * AIX/PASE have /bin -> /usr/bin, and chdir/getcwd follows links.
	 * Use a directory available on all systems that shouldn't be a link.
	 */
	const gchar *newdir = "/";
#endif

	char *dir = g_get_current_dir ();
	ASSERT_NE (nullptr, dir) << "No current directory?";
	g_free (dir);

	ASSERT_NE (-1, chdir (newdir)) << "No " << newdir << "?";

	dir = g_get_current_dir ();
	ASSERT_STREQ (newdir, dir);
	g_free (dir);
#endif
}

TEST_F (path, misc)
{
	ASSERT_NE (nullptr, g_get_home_dir ()) << "Where did my home go?";
	ASSERT_NE (nullptr, g_get_tmp_dir ()) << "Where did my /tmp go?";
}

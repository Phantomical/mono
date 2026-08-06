#include <config.h>

#include <string_view>

#include <glib.h>
#include <gtest/gtest.h>

TEST (dir, open_and_read)
{
	GError *gerror;
	GDir *dir;

	/*
	dir = g_dir_open (NULL, 0, NULL);
	*/
	dir = g_dir_open ("", 0, NULL);
	ASSERT_EQ (nullptr, dir) << "the empty name should be an error";

	dir = g_dir_open ("", 9, NULL);
	ASSERT_EQ (nullptr, dir) << "the empty name should be an error whatever the flags";

	gerror = NULL;
	dir = g_dir_open (".ljasdslakjd", 9, &gerror);
	ASSERT_EQ (nullptr, dir) << "opendir should fail";
	ASSERT_NE (nullptr, gerror) << "got no error";
	g_error_free (gerror);

	gerror = NULL;
	dir = g_dir_open (g_get_tmp_dir (), 9, &gerror);
	ASSERT_NE (nullptr, dir) << "opendir should succeed";
	ASSERT_EQ (nullptr, gerror) << "got an error";

	const gchar *name = g_dir_read_name (dir);
	ASSERT_NE (nullptr, name) << "didn't read a file name";

	/* "." and ".." are filtered out, unlike readdir's. */
	while ((name = g_dir_read_name (dir)) != NULL) {
		std::string_view entry (name);
		ASSERT_NE (std::string_view ("."), entry) << ". directory found";
		ASSERT_NE (std::string_view (".."), entry) << ".. directory found";
	}

	g_dir_close (dir);
}

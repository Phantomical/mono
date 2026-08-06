#include <stdlib.h>
#include <string.h>
#include <string_view>

#include <glib.h>
#include <gtest/gtest.h>

TEST (string, append_speed)
{
	GString *s = g_string_new ("");

	for (gint i = 0; i < 1024; i++)
		g_string_append (s, "x");

	ASSERT_EQ (1024u, strlen (s->str)) << "Incorrect string size";

	g_string_free (s, TRUE);
}

TEST (string, append_c_speed)
{
	GString *s = g_string_new ("");

	for (gint i = 0; i < 1024; i++)
		g_string_append_c (s, 'x');

	ASSERT_EQ (1024u, strlen (s->str)) << "Incorrect string size";

	g_string_free (s, TRUE);
}

TEST (string, ctor_append)
{
	GString *s = g_string_new_len ("My stuff", 2);

	ASSERT_STREQ ("My", s->str) << "Expected only 'My' on the string";
	g_string_free (s, TRUE);

	s = g_string_new_len ("My\0\0Rest", 6);
	ASSERT_EQ (0, s->str [2]) << "Null was not copied";
	ASSERT_STREQ ("Re", s->str + 4) << "Did not find the 'Re' part";

	g_string_append (s, "lalalalalalalalalalalalalalalalalalalalalalal");
	ASSERT_EQ (0, s->str [2]) << "Null was not copied";
	ASSERT_EQ (std::string_view ("Relala"), std::string_view (s->str + 4, 6))
		<< "Did not copy correctly";

	g_string_free (s, TRUE);

	s = g_string_new ("");
	for (int i = 0; i < 1024; i++)
		g_string_append_c (s, 'x');
	ASSERT_EQ (1024u, strlen (s->str)) << "Incorrect string size";
	g_string_free (s, TRUE);

	s = g_string_new ("hola");
	g_string_sprintfa (s, "%s%d", ", bola", 5);
	ASSERT_STREQ ("hola, bola5", s->str);
	g_string_free (s, TRUE);

	s = g_string_new ("Hola");
	g_string_printf (s, "Dingus");

	/* Test that it does not release it */
	char *ret = g_string_free (s, FALSE);
	ASSERT_STREQ ("Dingus", ret);
	g_free (ret);

	s = g_string_new_len ("H" "\000" "H", 3);
	g_string_append_len (s, "1" "\000" "2", 3);
	const char expected [] = { 'H', 0, 'H', '1', 0, '2' };
	for (size_t i = 0; i < sizeof (expected); i++)
		ASSERT_EQ (expected [i], s->str [i]) << "at index " << i;
	g_string_free (s, TRUE);
}

TEST (string, ctor_sized)
{
	GString *s = g_string_sized_new (20);

	ASSERT_EQ (0, s->str [0]) << "Expected an empty string";
	ASSERT_EQ (0u, s->len) << "Expected an empty len";

	g_string_free (s, TRUE);
}

TEST (string, truncate)
{
	GString *s = g_string_new ("0123456789");

	g_string_truncate (s, 3);
	ASSERT_EQ (3u, strlen (s->str));
	g_string_free (s, TRUE);

	s = g_string_new ("a");
	s = g_string_truncate (s, 10);
	ASSERT_EQ (1u, strlen (s->str)) << "truncating past the end must not grow";
	g_string_truncate (s, (gsize)-1);
	ASSERT_EQ (1u, strlen (s->str)) << "a huge length must not grow it either";
	g_string_truncate (s, 0);
	ASSERT_EQ (0u, strlen (s->str));

	g_string_free (s, TRUE);
}

TEST (string, append_len)
{
	GString *s = g_string_new ("");

	g_string_append_len (s, "boo\000x", 0);
	ASSERT_EQ (0u, s->len);
	g_string_append_len (s, "boo\000x", 5);
	ASSERT_EQ (5u, s->len);
	g_string_append_len (s, "ha", -1);
	ASSERT_EQ (7u, s->len);

	g_string_free (s, TRUE);
}

/*
 * G_STRLOC has to name this file and a plausible line in it.  The file name is
 * the compiler's, so this breaks if the source is ever renamed.
 */
TEST (string, macros)
{
	char *s = g_strdup (G_STRLOC);
	char *p = strchr (s + 2, ':');

	ASSERT_NE (nullptr, p) << "Did not find a separator in " << s;
	ASSERT_GT (atoi (p + 1), 0) << "did not find a valid line number";

	*p = 0;
	ASSERT_EQ (std::string_view ("string.cpp"),
		   std::string_view (s + strlen (s) - 10))
		<< "G_STRLOC did not store the file name: " << s;

	g_free (s);
}

TEST (string, strnlen)
{
	ASSERT_EQ (0u, g_strnlen ("abc", 0));
	ASSERT_EQ (1u, g_strnlen ("abc", 1));
	ASSERT_EQ (2u, g_strnlen ("abc", 2));
	ASSERT_EQ (3u, g_strnlen ("abc", 3));
	ASSERT_EQ (3u, g_strnlen ("abc", 4));
	ASSERT_EQ (3u, g_strnlen ("abc", 5));
}

#include <string.h>
#include <string_view>

#include <glib.h>
#include <gtest/gtest.h>

namespace {

void
expect_uri (const char *filename, const char *expected)
{
	char *s = g_filename_to_uri (filename, NULL, NULL);
	EXPECT_STREQ (expected, s) << "for filename " << filename;
	g_free (s);
}

void
expect_no_uri (const char *filename)
{
	char *s = g_filename_to_uri (filename, NULL, NULL);
	EXPECT_STREQ (nullptr, s) << "for filename " << filename;
	g_free (s);
}

void
expect_filename (const char *uri, const char *expected)
{
	char *s = g_filename_from_uri (uri, NULL, NULL);
	EXPECT_STREQ (expected, s) << "for uri " << uri;
	g_free (s);
}

void
expect_no_filename (const char *uri)
{
	char *s = g_filename_from_uri (uri, NULL, NULL);
	EXPECT_STREQ (nullptr, s) << "for uri " << uri;
	g_free (s);
}

#define G_STR_DELIMITERS "_-|> <."

void
delimit_all (char *a, const char *old, char replacement)
{
	old = old ? old : G_STR_DELIMITERS;
	while (*old)
		g_strdelimit (a, *old++, replacement);
}

const char NUMBERS [] = "0123456789";

}

/* This test is just to be used with valgrind */
TEST (strutil, strfreev)
{
	gchar **array = g_new (gchar *, 4);
	array [0] = g_strdup ("one");
	array [1] = g_strdup ("two");
	array [2] = g_strdup ("three");
	array [3] = NULL;

	g_strfreev (array);
	g_strfreev (NULL);
}

TEST (strutil, strconcat)
{
	gchar *x = g_strconcat ("Hello", ", ", "world", (const char*)NULL);
	ASSERT_STREQ ("Hello, world", x);
	g_free (x);
}

TEST (strutil, strsplit)
{
	const gchar *to_split = "Hello world, how are we doing today?";
	gint i;
	gchar **v;

	v = g_strsplit (to_split, " ", 0);
	ASSERT_NE (nullptr, v) << "split failed, got NULL vector (1)";
	for (i = 0; v [i] != NULL; i++)
		;
	ASSERT_EQ (7, i) << "expected 7 tokens";
	g_strfreev (v);

	v = g_strsplit (to_split, ":", -1);
	ASSERT_NE (nullptr, v) << "split failed, got NULL vector (2)";
	for (i = 0; v [i] != NULL; i++)
		;
	ASSERT_EQ (1, i) << "expected 1 token";
	ASSERT_STREQ (to_split, v [0]);
	g_strfreev (v);

	v = g_strsplit ("", ":", 0);
	ASSERT_NE (nullptr, v) << "g_strsplit returned NULL";
	g_strfreev (v);

	v = g_strsplit ("/home/miguel/dingus", "/", 0);
	ASSERT_STREQ ("", v [0]) << "Got a non-empty first element";
	g_strfreev (v);

	v = g_strsplit ("appdomain1, Version=0.0.0.0, Culture=neutral", ",", 4);
	ASSERT_STREQ ("appdomain1", v [0]);
	ASSERT_STREQ (" Version=0.0.0.0", v [1]);
	ASSERT_STREQ (" Culture=neutral", v [2]);
	ASSERT_EQ (nullptr, v [3]) << "Expected only 3 elements";
	g_strfreev (v);

	v = g_strsplit ("abcXYdefXghiXYjklYmno", "XY", 4);
	ASSERT_STREQ ("abc", v [0]);
	ASSERT_STREQ ("defXghi", v [1]);
	ASSERT_STREQ ("jklYmno", v [2]);
	ASSERT_EQ (nullptr, v [3]) << "Expected only 3 elements (1)";
	g_strfreev (v);

	v = g_strsplit ("abcXYdefXghiXYjklYmno", "XY", 2);
	ASSERT_STREQ ("abc", v [0]);
	ASSERT_STREQ ("defXghiXYjklYmno", v [1]);
	ASSERT_EQ (nullptr, v [2]) << "Expected only 2 elements (2)";
	g_strfreev (v);

	v = g_strsplit ("abcXYdefXghiXYjklYmnoXY", "XY", 3);
	ASSERT_STREQ ("abc", v [0]);
	ASSERT_STREQ ("defXghi", v [1]);
	ASSERT_STREQ ("jklYmnoXY", v [2]);
	ASSERT_EQ (nullptr, v [3]) << "Expected only 3 elements (3)";
	g_strfreev (v);

	v = g_strsplit ("abcXYXYXYdefXY", "XY", -1);
	ASSERT_STREQ ("abc", v [0]);
	ASSERT_STREQ ("", v [1]);
	ASSERT_STREQ ("", v [2]);
	ASSERT_STREQ ("def", v [3]);
	ASSERT_STREQ ("", v [4]);
	ASSERT_EQ (nullptr, v [5]) << "Expected only 5 elements (4)";
	g_strfreev (v);

	v = g_strsplit ("XYXYXYabcXYdef", "XY", -1);
	ASSERT_STREQ ("", v [0]);
	ASSERT_STREQ ("", v [1]);
	ASSERT_STREQ ("", v [2]);
	ASSERT_STREQ ("abc", v [3]);
	ASSERT_STREQ ("def", v [4]);
	ASSERT_EQ (nullptr, v [5]) << "Expected only 5 elements (5)";
	g_strfreev (v);

	v = g_strsplit ("value=", "=", 2);
	ASSERT_STREQ ("value", v [0]);
	ASSERT_STREQ ("", v [1]);
	ASSERT_EQ (nullptr, v [2]) << "Expected only 2 elements (6)";
	g_strfreev (v);
}

TEST (strutil, strsplit_set)
{
	gchar **v;

	v = g_strsplit_set ("abcXYdefXghiXYjklYmno", "XY", 6);
	ASSERT_STREQ ("abc", v [0]);
	ASSERT_STREQ ("", v [1]);
	ASSERT_STREQ ("def", v [2]);
	ASSERT_STREQ ("ghi", v [3]);
	ASSERT_STREQ ("", v [4]);
	ASSERT_STREQ ("jklYmno", v [5]);
	ASSERT_EQ (nullptr, v [6]) << "Expected only 6 elements (1)";
	g_strfreev (v);

	v = g_strsplit_set ("abcXYdefXghiXYjklYmno", "XY", 3);
	ASSERT_STREQ ("abc", v [0]);
	ASSERT_STREQ ("", v [1]);
	ASSERT_STREQ ("defXghiXYjklYmno", v [2]);
	ASSERT_EQ (nullptr, v [3]) << "Expected only 3 elements (2)";
	g_strfreev (v);

	v = g_strsplit_set ("abcXdefYghiXjklYmnoX", "XY", 5);
	ASSERT_STREQ ("abc", v [0]);
	ASSERT_STREQ ("def", v [1]);
	ASSERT_STREQ ("ghi", v [2]);
	ASSERT_STREQ ("jkl", v [3]);
	ASSERT_STREQ ("mnoX", v [4]);
	ASSERT_EQ (nullptr, v [5]) << "Expected only 5 elements (5)";
	g_strfreev (v);

	v = g_strsplit_set ("abcXYXdefXY", "XY", -1);
	ASSERT_STREQ ("abc", v [0]);
	ASSERT_STREQ ("", v [1]);
	ASSERT_STREQ ("", v [2]);
	ASSERT_STREQ ("def", v [3]);
	ASSERT_STREQ ("", v [4]);
	ASSERT_STREQ ("", v [5]);
	ASSERT_EQ (nullptr, v [6]) << "Expected only 6 elements (4)";
	g_strfreev (v);

	v = g_strsplit_set ("XYXabcXYdef", "XY", -1);
	ASSERT_STREQ ("", v [0]);
	ASSERT_STREQ ("", v [1]);
	ASSERT_STREQ ("", v [2]);
	ASSERT_STREQ ("abc", v [3]);
	ASSERT_STREQ ("", v [4]);
	ASSERT_STREQ ("def", v [5]);
	ASSERT_EQ (nullptr, v [6]) << "Expected only 6 elements (5)";
	g_strfreev (v);
}

TEST (strutil, strreverse)
{
	gchar *a = g_strdup ("onetwothree");
	gchar *b = g_strdup ("onetwothre");
	gchar *c = g_strdup ("");

	g_strreverse (a);
	EXPECT_STREQ ("eerhtowteno", a);

	g_strreverse (b);
	EXPECT_STREQ ("erhtowteno", b);

	g_strreverse (c);
	EXPECT_STREQ ("", c);

	g_free (c);
	g_free (b);
	g_free (a);
}

TEST (strutil, strjoin)
{
	char *s;

	s = g_strjoin (NULL, "a", "b", (const char*)NULL);
	ASSERT_STREQ ("ab", s) << "Join of two strings with no separator fails";
	g_free (s);

	s = g_strjoin ("", "a", "b", (const char*)NULL);
	ASSERT_STREQ ("ab", s) << "Join of two strings with empty separator fails";
	g_free (s);

	s = g_strjoin ("-", "a", "b", (const char*)NULL);
	ASSERT_STREQ ("a-b", s) << "Join of two strings with separator fails";
	g_free (s);

	s = g_strjoin ("-", "aaaa", "bbbb", "cccc", "dddd", (const char*)NULL);
	ASSERT_STREQ ("aaaa-bbbb-cccc-dddd", s) << "Join of multiple strings fails";
	g_free (s);

	s = g_strjoin ("-", (const char*)NULL);
	ASSERT_STREQ ("", s) << "Failed to join empty arguments";
	g_free (s);
}

TEST (strutil, strchug)
{
	char *str = g_strdup (" \t\n hola");

	g_strchug (str);
	EXPECT_STREQ ("hola", str);
	g_free (str);
}

TEST (strutil, strchomp)
{
	char *str = g_strdup ("hola  \t");

	g_strchomp (str);
	EXPECT_STREQ ("hola", str);
	g_free (str);
}

TEST (strutil, strstrip)
{
	char *str = g_strdup (" \t hola   ");

	g_strstrip (str);
	EXPECT_STREQ ("hola", str);
	g_free (str);
}

TEST (strutil, filename_to_uri)
{
	expect_uri ("/a", "file:///a");
	expect_uri ("/home/miguel", "file:///home/miguel");
	expect_uri ("/home/mig uel", "file:///home/mig%20uel");
	expect_uri ("/\303\241", "file:///%C3%A1");
	expect_uri ("/\303\241/octal", "file:///%C3%A1/octal");
	expect_uri ("/%", "file:///%25");
	expect_uri ("/\001\002\003\004\005\006\007\010\011\012\013\014\015\016\017\020\021\022\023\024\025\026\027\030\031\032\033\034\035\036\037\040",
		    "file:///%01%02%03%04%05%06%07%08%09%0A%0B%0C%0D%0E%0F%10%11%12%13%14%15%16%17%18%19%1A%1B%1C%1D%1E%1F%20");
	expect_uri ("/!$&'()*+,-./", "file:///!$&'()*+,-./");
	expect_uri ("/\042\043\045", "file:///%22%23%25");
	expect_uri ("/0123456789:=", "file:///0123456789:=");
	expect_uri ("/\073\074\076\077", "file:///%3B%3C%3E%3F");
	expect_uri ("/\133\134\135\136_\140\173\174\175", "file:///%5B%5C%5D%5E_%60%7B%7C%7D");
	expect_uri ("/\173\174\175\176\177\200", "file:///%7B%7C%7D~%7F%80");
	expect_uri ("/@ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz",
		    "file:///@ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz");

	expect_no_uri ("a");
	expect_no_uri ("./hola");
}

TEST (strutil, filename_from_uri)
{
	expect_filename ("file:///a", "/a");
	expect_filename ("file:///%41", "/A");
	expect_filename ("file:///home/miguel", "/home/miguel");
	expect_filename ("file:///home/mig%20uel", "/home/mig uel");
	expect_filename ("file:///home/c%2B%2B", "/home/c++");
	expect_filename ("file:///home/c%2b%2b", "/home/c++");

	expect_no_filename ("/a");
	expect_no_filename ("a");
	expect_no_filename ("file://a");
	expect_no_filename ("file:a");
	expect_no_filename ("file:///%");
	expect_no_filename ("file:///%0");
	expect_no_filename ("file:///%jj");
}

TEST (strutil, ascii_xdigit_value)
{
	ASSERT_EQ (-1, g_ascii_xdigit_value ('9' + 1));
	ASSERT_EQ (-1, g_ascii_xdigit_value ('0' - 1));
	ASSERT_EQ (-1, g_ascii_xdigit_value ('a' - 1));
	ASSERT_EQ (-1, g_ascii_xdigit_value ('f' + 1));
	ASSERT_EQ (-1, g_ascii_xdigit_value ('A' - 1));
	ASSERT_EQ (-1, g_ascii_xdigit_value ('F' + 1));

	for (gchar j = '0'; j < '9'; j++)
		ASSERT_EQ (j - '0', g_ascii_xdigit_value (j)) << "for digit " << j;
	for (gchar j = 'a'; j < 'f'; j++)
		ASSERT_EQ (j - 'a' + 10, g_ascii_xdigit_value (j)) << "for lower " << j;
	for (gchar j = 'A'; j < 'F'; j++)
		ASSERT_EQ (j - 'A' + 10, g_ascii_xdigit_value (j)) << "for upper " << j;
}

TEST (strutil, strdelimit)
{
	gchar *str;

	str = g_strdup (G_STR_DELIMITERS);
	delimit_all (str, NULL, 'a');
	ASSERT_STREQ ("aaaaaaa", str) << "All delimiters";
	g_free (str);

	str = g_strdup ("hola");
	delimit_all (str, "ha", '+');
	ASSERT_STREQ ("+ol+", str) << "2 delimiters";
	g_free (str);
}

TEST (strutil, strlcpy)
{
	const gchar *src = "onetwothree";
	const gsize srclen = strlen (src);
	gchar *dest = g_new0 (gchar, srclen + 1);

	ASSERT_EQ (srclen, g_strlcpy (dest, src, (gsize)-1));
	ASSERT_STREQ (src, dest);

	ASSERT_EQ (srclen, g_strlcpy (dest, src, 3));
	ASSERT_STREQ ("on", dest);

	ASSERT_EQ (srclen, g_strlcpy (dest, src, 1));
	ASSERT_STREQ ("", dest);

	ASSERT_EQ (srclen, g_strlcpy (dest, src, 12345));
	ASSERT_STREQ (src, dest);
	g_free (dest);

	/* This is a test for g_filename_from_utf8, even if it does not look like it */
	dest = g_filename_from_utf8 (NUMBERS, strlen (NUMBERS), NULL, NULL, NULL);
	ASSERT_STREQ (NUMBERS, dest);
	g_free (dest);
}

TEST (strutil, strescape)
{
	gchar *str;

	str = g_strescape ("abc", NULL);
	ASSERT_STREQ ("abc", str);
	g_free (str);

	str = g_strescape ("\t\b\f\n\r\\\"abc", NULL);
	ASSERT_STREQ ("\\t\\b\\f\\n\\r\\\\\\\"abc", str);
	g_free (str);

	str = g_strescape ("\001abc", NULL);
	ASSERT_STREQ ("\\001abc", str);
	g_free (str);

	str = g_strescape ("\001abc", "\001");
	ASSERT_STREQ ("\001abc", str) << "an excluded character must come through raw";
	g_free (str);
}

TEST (strutil, ascii_strncasecmp)
{
	ASSERT_EQ (0, g_ascii_strncasecmp ("123", "123", 1));
	ASSERT_GT (g_ascii_strncasecmp ("423", "123", 1), 0);
	ASSERT_LT (g_ascii_strncasecmp ("123", "423", 1), 0);
	ASSERT_EQ (0, g_ascii_strncasecmp ("1", "1", 10));
}

TEST (strutil, ascii_strdown)
{
	const gchar *a = "~09+AaBcDeFzZ$0909EmPAbCdEEEEEZZZZAAA";
	const gchar *b = "~09+aabcdefzz$0909empabcdeeeeezzzzaaa";
	gint l = (gint) strlen (b);

	gchar *c = g_ascii_strdown (a, l);
	ASSERT_EQ (0, g_ascii_strncasecmp (b, c, l)) << "got " << c;
	g_free (c);
}

TEST (strutil, strdupv)
{
	gchar **one = g_strdupv (NULL);
	ASSERT_EQ (nullptr, one) << "g_strdupv (NULL) should be NULL";

	one = g_new (gchar *, 1);
	*one = NULL;
	gchar **two = g_strdupv (one);
	ASSERT_NE (nullptr, two);
	ASSERT_EQ (0u, g_strv_length (two));
	g_strfreev (two);
	g_strfreev (one);
}

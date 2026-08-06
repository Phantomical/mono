#include <glib.h>
#include <gtest/gtest.h>

namespace {

bool
matches (const char *pattern, const char *string)
{
	GPatternSpec *spec = g_pattern_spec_new (pattern);
	gboolean res = g_pattern_match_string (spec, string);
	g_pattern_spec_free (spec);
	return res != FALSE;
}

}

TEST (pattern, pattern_spec)
{
	/* spec = g_pattern_spec_new (NULL); */
	EXPECT_TRUE (matches ("*", "hola"));
	EXPECT_TRUE (matches ("hola", "hola"));
	EXPECT_TRUE (matches ("????", "hola"));
	EXPECT_TRUE (matches ("???a", "hola"));
	EXPECT_TRUE (matches ("h??a", "hola"));
	EXPECT_TRUE (matches ("h??*", "hola"));
	EXPECT_TRUE (matches ("h*", "hola"));
	EXPECT_TRUE (matches ("*hola", "hola"));
	EXPECT_TRUE (matches ("*l*", "hola"));
	EXPECT_TRUE (matches ("h*??", "hola"));
	EXPECT_TRUE (matches ("h*???", "hola"));
	EXPECT_TRUE (matches ("?o??", "hola"));
	EXPECT_TRUE (matches ("*h*o*l*a*", "hola"));
	EXPECT_TRUE (matches ("h*o*l*a", "hola"));
	EXPECT_TRUE (matches ("h?*?", "hola"));

	EXPECT_FALSE (matches ("", "hola"));
	EXPECT_FALSE (matches ("?????", "hola"));
	EXPECT_FALSE (matches ("???", "hola"));
	EXPECT_FALSE (matches ("*o", "hola"));
	EXPECT_FALSE (matches ("h", "hola"));
	EXPECT_FALSE (matches ("h*????", "hola"));
}

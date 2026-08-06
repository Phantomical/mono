#include <glib.h>
#include <gtest/gtest.h>

TEST (unicode, unichar_type)
{
	ASSERT_EQ (G_UNICODE_UPPERCASE_LETTER, g_unichar_type ('A'));
	ASSERT_EQ (G_UNICODE_LOWERCASE_LETTER, g_unichar_type ('a'));
	ASSERT_EQ (G_UNICODE_DECIMAL_NUMBER, g_unichar_type ('1'));
	ASSERT_EQ (G_UNICODE_CURRENCY_SYMBOL, g_unichar_type (0xA3));
}

TEST (unicode, unichar_toupper)
{
	ASSERT_EQ (0u, g_unichar_toupper (0));
	ASSERT_EQ ((gunichar)'A', g_unichar_toupper ('a'));
	ASSERT_EQ ((gunichar)'1', g_unichar_toupper ('1'));
	ASSERT_EQ (0x1C4u, g_unichar_toupper (0x1C4));
	ASSERT_EQ (0x1F1u, g_unichar_toupper (0x1F2));
	ASSERT_EQ (0x1F1u, g_unichar_toupper (0x1F3));
	ASSERT_EQ (0xFFFFu, g_unichar_toupper (0xFFFF));
	ASSERT_EQ (0x10400u, g_unichar_toupper (0x10428));
}

TEST (unicode, unichar_tolower)
{
	ASSERT_EQ (0u, g_unichar_tolower (0));
	ASSERT_EQ ((gunichar)'a', g_unichar_tolower ('A'));
	ASSERT_EQ ((gunichar)'1', g_unichar_tolower ('1'));
	ASSERT_EQ (0x1C6u, g_unichar_tolower (0x1C5));
	ASSERT_EQ (0x1F3u, g_unichar_tolower (0x1F1));
	ASSERT_EQ (0x1F3u, g_unichar_tolower (0x1F2));
	ASSERT_EQ (0xFFFFu, g_unichar_tolower (0xFFFF));
}

TEST (unicode, unichar_totitle)
{
	ASSERT_EQ (0u, g_unichar_totitle (0));
	ASSERT_EQ ((gunichar)'A', g_unichar_totitle ('a'));
	ASSERT_EQ ((gunichar)'1', g_unichar_totitle ('1'));
	ASSERT_EQ (0x1C5u, g_unichar_totitle (0x1C4));
	ASSERT_EQ (0x1F2u, g_unichar_totitle (0x1F2));
	ASSERT_EQ (0x1F2u, g_unichar_totitle (0x1F3));
	ASSERT_EQ (0xFFFFu, g_unichar_totitle (0xFFFF));
}

/*
 * test-mono-string.cpp: Unit test for runtime MonoString* manipulation.
 */

#include "config.h"
#include <stdio.h>
#include "metadata/object-internals.h"
#include "mini/jit.h"

#include <gtest/gtest.h>

#include "harness.hpp"

namespace {

/* The Test suffix keeps the fixture from shadowing MonoString inside the bodies. */
class MonoStringTest : public ::testing::Test {
public:
	static void SetUpTestSuite ()
	{
		MONO_SKIP_WITHOUT_CLASS_LIBRARY ();
		mono::test::init_runtime ();
	}
};

} // namespace

TEST_F (MonoStringTest, NewFromAscii)
{
	ERROR_DECL (error);
	MonoString *s = mono_string_new_checked (mono_domain_get (), "abcd", error);
	static const gunichar2 u16s[] = { 0x61, 0x62, 0x63, 0x64, 0 }; /* u16 "abcd" */
	mono_error_assert_ok (error);
	gunichar2 *c = mono_string_chars_internal (s);

	ASSERT_NE (nullptr, c);
	EXPECT_EQ (0, memcmp (&u16s, c, sizeof (u16s)));
}

TEST_F (MonoStringTest, NewFromUtf8)
{
	ERROR_DECL (error);
	const gunichar2 snowman = 0x2603;
	static const unsigned char bytes[] = { 0xE2, 0x98, 0x83, 0x00 }; /* U+2603 NUL */
	MonoString *s = mono_string_new_checked (mono_domain_get (), (const char*)bytes, error);
	mono_error_assert_ok (error);
	gunichar2 *c = mono_string_chars_internal (s);

	ASSERT_NE (nullptr, c);
	EXPECT_EQ (snowman, c [0]);
	EXPECT_EQ (0, c [1]);
}

/* An invalid UTF-8 byte has to come back as a MonoError carrying a message. */
TEST_F (MonoStringTest, NewFromInvalidUtf8)
{
	ERROR_DECL (error);
	static const unsigned char bytes[] = { 'a', 0xFC, 'b', 'c', 0 };
	MonoString G_GNUC_UNUSED *s = mono_string_new_checked (mono_domain_get (), (const char*)bytes, error);

	ASSERT_FALSE (is_ok (error));
	const char *msg = mono_error_get_message (error);
	EXPECT_NE (nullptr, msg);
	mono_error_cleanup (error);
}

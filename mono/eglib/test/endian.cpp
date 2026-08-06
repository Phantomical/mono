#include <glib.h>
#include <gtest/gtest.h>

TEST (endian, swap)
{
	guint32 a = 0xabcdef01;
	guint64 b = (((guint64) a) << 32) | a;
	guint64 b_expect = (((guint64) 0x1efcdab) << 32) | 0x01efcdab;
	guint16 c = 0xabcd;

	ASSERT_EQ (0x01efcdabu, GUINT32_SWAP_LE_BE (a));
	ASSERT_EQ (0x1000000u, GUINT32_SWAP_LE_BE (1));
	ASSERT_EQ (b_expect, GUINT64_SWAP_LE_BE (b));
	ASSERT_EQ (0xcdab, GUINT16_SWAP_LE_BE (c));
}

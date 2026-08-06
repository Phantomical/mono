/*
 * G_ENUM_FUNCTIONS gives a C++ enum the bitwise operators C gives it for free.
 * eglib's headers are included from the runtime's C++ translation units, so
 * this has to keep compiling and keep meaning what the C spelling means.
 */
#include <glib.h>
#include <gtest/gtest.h>

namespace {

enum Color {
	Black = 0,
	Red = 1,
	Blue = 2,
	Purple = Red | Blue, // 3
	Green = 4,
	Yellow = Red | Green, // 5
	White = 7,
};

G_ENUM_FUNCTIONS (Color)

}

TEST (enums, bitwise_operators)
{
	const Color green = Green;
	const Color blue = Blue;
	const Color red = Red;
	const Color white = White;
	const Color purple = Purple;

	ASSERT_EQ (Black, red & blue);
	ASSERT_EQ (White, red | blue | green);
	ASSERT_EQ (Purple, red | blue);
	ASSERT_EQ (green, white ^ purple);

	Color c = Black;
	Color c2 = Black;
	c |= red;
	ASSERT_EQ (Red, c);
	c ^= red;
	ASSERT_EQ (Black, c);

	c |= (c2 |= Red) | Blue;
	ASSERT_EQ (Purple, c);
	ASSERT_EQ (Red, c2);

	c = c2 = Black;
	c |= (c2 |= Red) |= Blue;
	ASSERT_EQ (Purple, c);
	ASSERT_EQ (Purple, c2);

	c = red;
	c &= red;
	ASSERT_EQ (Red, c);
	c &= blue;
	ASSERT_EQ (Black, c);
}

/*
 * Tests to ensure that our type definitions are correct
 *
 * These depend on -Werror, -Wall being set to catch the build error.
 */
#include <stdint.h>
#include <stdio.h>

#include <glib.h>
#include <gtest/gtest.h>

namespace {

struct my_struct {
	int a;
	int b;
};

}

TEST (sizes, formats)
{
	char buffer [1024];
	gsize a = 1;

	sprintf (buffer, "%" G_GSIZE_FORMAT, a);
	ASSERT_STREQ ("1", buffer);
}

TEST (sizes, ptrconv)
{
	static const int ints [] = { G_MAXINT32, G_MININT32, 1, -1, 0 };
	static const unsigned int uints [] = { 0, 1, UINT32_MAX };

	for (int iv : ints) {
		gpointer ptr = GINT_TO_POINTER (iv);
		ASSERT_EQ (iv, GPOINTER_TO_INT (ptr))
			<< "int to pointer and back conversions fail";
	}

	for (unsigned int uv : uints) {
		gpointer ptr = GUINT_TO_POINTER (uv);
		ASSERT_EQ (uv, GPOINTER_TO_UINT (ptr))
			<< "uint to pointer and back conversions fail";
	}
}

TEST (sizes, g_struct_offset)
{
	ASSERT_EQ (0, G_STRUCT_OFFSET (my_struct, a)) << "offset of a is not zero";

	ASSERT_TRUE (G_STRUCT_OFFSET (my_struct, b) == 4 || G_STRUCT_OFFSET (my_struct, b) == 8)
		<< "offset of b should be 4 or 8, macro might be busted";
}

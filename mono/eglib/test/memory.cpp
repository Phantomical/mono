#include <glib.h>
#include <gtest/gtest.h>

TEST (memory, zero_size_allocations)
{
	ASSERT_EQ (nullptr, (gpointer) g_malloc (0))
		<< "Calling g_malloc with size zero should return NULL.";
	ASSERT_EQ (nullptr, (gpointer) g_malloc0 (0))
		<< "Calling g_malloc0 with size zero should return NULL.";
	ASSERT_EQ (nullptr, (gpointer) g_realloc (NULL, 0))
		<< "Calling g_realloc with size zero should return NULL.";
	ASSERT_EQ (nullptr, (gpointer) g_new (int, 0))
		<< "Calling g_new with size zero should return NULL.";
	ASSERT_EQ (nullptr, (gpointer) g_new0 (int, 0))
		<< "Calling g_new0 with size zero should return NULL.";
}

/*
 * ALIGN_TO / ALIGN_DOWN_TO / ALIGN_PTR_TO with the alignment as a signed int.
 * Signedness matters: the macros mix the alignment into pointer-width
 * arithmetic, so a signed operand that gets promoted the wrong way corrupts
 * the high half of the result.
 */
TEST (memory, align_signed_int)
{
	const gssize orig_value = 67;
	gpointer orig_ptr = (gpointer) orig_value;

	int align = 1;
	ASSERT_EQ (orig_value, ALIGN_TO (orig_value, align));
	ASSERT_EQ (orig_value, ALIGN_DOWN_TO (orig_value, align));
	ASSERT_EQ (orig_ptr, ALIGN_PTR_TO (orig_ptr, align));

	align = 8;
	ASSERT_EQ (72, ALIGN_TO (orig_value, align));
	ASSERT_EQ ((gpointer) 72, ALIGN_PTR_TO (orig_ptr, align));
	ASSERT_EQ (64, ALIGN_DOWN_TO (orig_value, align));
}

TEST (memory, align_unsigned_int)
{
	const gssize orig_value = 67;
	gpointer orig_ptr = (gpointer) orig_value;

	unsigned int align = 1;
	ASSERT_EQ (orig_value, ALIGN_TO (orig_value, align));
	ASSERT_EQ (orig_value, ALIGN_DOWN_TO (orig_value, align));
	ASSERT_EQ (orig_ptr, ALIGN_PTR_TO (orig_ptr, align));

	align = 16;
	ASSERT_EQ (80, ALIGN_TO (orig_value, align));
	ASSERT_EQ ((gpointer) 80, ALIGN_PTR_TO (orig_ptr, align));
	ASSERT_EQ (64, ALIGN_DOWN_TO (orig_value, align));
}

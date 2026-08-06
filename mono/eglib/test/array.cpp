#include <glib.h>
#include <gtest/gtest.h>

/* example from glib documentation */
TEST (array, big)
{
	/* We create a new array to store gint values.
	   We don't want it zero-terminated or cleared to 0's. */
	GArray *garray = g_array_new (FALSE, FALSE, sizeof (gint));
	for (gint i = 0; i < 10000; i++)
		g_array_append_val (garray, i);

	for (gint i = 0; i < 10000; i++)
		ASSERT_EQ (i, g_array_index (garray, gint, i)) << "at index " << i;

	g_array_free (garray, TRUE);
}

TEST (array, index)
{
	GArray *array = g_array_new (FALSE, FALSE, sizeof (int));
	int v = 27;

	g_array_append_val (array, v);
	ASSERT_EQ (27, g_array_index (array, int, 0));

	g_array_free (array, TRUE);
}

TEST (array, append_zero_term)
{
	GArray *array = g_array_new (TRUE, FALSE, sizeof (int));
	int v = 27;

	g_array_append_val (array, v);

	ASSERT_EQ (27, g_array_index (array, int, 0)) << "g_array_append_val failed";
	ASSERT_EQ (0, g_array_index (array, int, 1))
		<< "zero_terminated didn't append a zero element";

	g_array_free (array, TRUE);
}

TEST (array, append)
{
	GArray *array = g_array_new (FALSE, FALSE, sizeof (int));
	int v = 27;

	ASSERT_EQ (0u, array->len) << "initial array length not zero";

	g_array_append_val (array, v);

	ASSERT_EQ (1u, array->len) << "array append failed";

	g_array_free (array, TRUE);
}

TEST (array, insert_val)
{
	GArray *array = g_array_new (FALSE, FALSE, sizeof (gpointer));

	g_array_insert_val (array, 0, array);
	ASSERT_EQ (array, g_array_index (array, gpointer, 0));

	g_array_insert_val (array, 1, array);
	ASSERT_EQ (array, g_array_index (array, gpointer, 1));

	g_array_insert_val (array, 2, array);
	ASSERT_EQ (array, g_array_index (array, gpointer, 2));

	g_array_free (array, TRUE);

	array = g_array_new (FALSE, FALSE, sizeof (gpointer));
	gpointer ptr0 = array;
	gpointer ptr1 = array + 1;
	gpointer ptr2 = array + 2;
	gpointer ptr3 = array + 3;

	g_array_insert_val (array, 0, ptr0);
	g_array_insert_val (array, 1, ptr1);
	g_array_insert_val (array, 2, ptr2);
	g_array_insert_val (array, 1, ptr3);

	ASSERT_EQ (ptr0, g_array_index (array, gpointer, 0));
	ASSERT_EQ (ptr3, g_array_index (array, gpointer, 1));
	ASSERT_EQ (ptr1, g_array_index (array, gpointer, 2));
	ASSERT_EQ (ptr2, g_array_index (array, gpointer, 3));

	g_array_free (array, TRUE);
}

TEST (array, remove)
{
	GArray *array = g_array_new (FALSE, FALSE, sizeof (int));
	int v [] = {30, 29, 28, 27, 26, 25};

	g_array_append_vals (array, v, 6);
	ASSERT_EQ (6u, array->len) << "append_vals fail";

	g_array_remove_index (array, 3);

	ASSERT_EQ (5u, array->len) << "remove_index failed to update length";
	ASSERT_EQ (26, g_array_index (array, int, 3))
		<< "remove_index failed to update the array";

	g_array_free (array, TRUE);
}

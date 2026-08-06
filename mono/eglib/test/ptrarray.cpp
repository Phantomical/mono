#include <string_view>

#include <glib.h>
#include <gtest/gtest.h>

namespace {

/* Redefine the private structure only to verify proper allocations */
struct GPtrArrayPriv {
	gpointer *pdata;
	guint len;
	guint size;
};

/* Don't add more than 32 items to this please */
const char *items [] = {
	"Apples", "Oranges", "Plumbs", "Goats", "Snorps", "Grapes",
	"Tickle", "Place", "Coffee", "Cookies", "Cake", "Cheese",
	"Tseng", "Holiday", "Avenue", "Smashing", "Water", "Toilet",
	NULL
};

gchar * const letters [] = {
	(char*)"A", (char*)"B", (char*)"C", (char*)"D", (char*)"E"
};

GPtrArray *
alloc_and_fill (guint *item_count)
{
	GPtrArray *array = g_ptr_array_new ();
	gint i;

	for (i = 0; items [i] != NULL; i++)
		g_ptr_array_add (array, (gpointer) items [i]);

	if (item_count != NULL)
		*item_count = i;

	return array;
}

guint
guess_size (guint length)
{
	guint size = 1;

	while (size < length)
		size <<= 1;

	return size;
}

struct ForeachState {
	gint index;
	bool mismatch;
};

void
foreach_callback (gpointer data, gpointer user_data)
{
	ForeachState *state = (ForeachState *) user_data;

	if (data != items [state->index])
		state->mismatch = true;
	state->index++;
}

gint
sort_compare (gconstpointer a, gconstpointer b)
{
	return std::string_view (*(gchar **) a).compare (*(gchar **) b);
}

gint
sort_compare_with_data (gconstpointer a, gconstpointer b, gpointer user_data)
{
	EXPECT_EQ (std::string_view ("this is the data for qsort"),
		   std::string_view ((const char *) user_data));

	return std::string_view (*(gchar **) a).compare (*(gchar **) b);
}

}

TEST (ptrarray, alloc)
{
	guint i;
	GPtrArrayPriv *array = (GPtrArrayPriv *) alloc_and_fill (&i);

	ASSERT_EQ (guess_size (array->len), array->size);
	ASSERT_EQ (i, array->len);

	g_ptr_array_free ((GPtrArray *) array, TRUE);
}

TEST (ptrarray, for_iterate)
{
	GPtrArray *array = alloc_and_fill (NULL);

	for (guint i = 0; i < array->len; i++)
		ASSERT_EQ ((gpointer) items [i], g_ptr_array_index (array, i))
			<< "at index " << i;

	g_ptr_array_free (array, TRUE);
}

TEST (ptrarray, foreach_iterate)
{
	GPtrArray *array = alloc_and_fill (NULL);
	ForeachState state = { 0, false };

	g_ptr_array_foreach (array, foreach_callback, &state);

	g_ptr_array_free (array, TRUE);

	ASSERT_FALSE (state.mismatch) << "foreach visited the items out of order";
}

TEST (ptrarray, set_size)
{
	GPtrArray *array = g_ptr_array_new ();
	const guint grow_length = 50;

	g_ptr_array_add (array, (gpointer) items [0]);
	g_ptr_array_add (array, (gpointer) items [1]);
	g_ptr_array_set_size (array, grow_length);

	ASSERT_EQ (grow_length, array->len);
	ASSERT_EQ ((gpointer) items [0], array->pdata [0]) << "Item 0 was overwritten";
	ASSERT_EQ ((gpointer) items [1], array->pdata [1]) << "Item 1 was overwritten";

	for (guint i = 2; i < array->len; i++)
		ASSERT_EQ (nullptr, array->pdata [i]) << "Item " << i << " is not NULL";

	g_ptr_array_free (array, TRUE);
}

TEST (ptrarray, remove_index)
{
	guint i;
	GPtrArray *array = alloc_and_fill (&i);

	g_ptr_array_remove_index (array, 0);
	ASSERT_EQ ((gpointer) items [1], array->pdata [0]);

	g_ptr_array_remove_index (array, array->len - 1);
	ASSERT_EQ ((gpointer) items [array->len], array->pdata [array->len - 1]);

	g_ptr_array_free (array, TRUE);
}

TEST (ptrarray, remove_index_fast)
{
	guint i;
	GPtrArray *array = alloc_and_fill (&i);

	g_ptr_array_remove_index_fast (array, 0);
	ASSERT_EQ ((gpointer) items [array->len], array->pdata [0]);

	g_ptr_array_remove_index_fast (array, array->len - 1);
	ASSERT_EQ ((gpointer) items [array->len - 1], array->pdata [array->len - 1]);

	g_ptr_array_free (array, TRUE);
}

TEST (ptrarray, remove)
{
	guint i;
	GPtrArray *array = alloc_and_fill (&i);

	g_ptr_array_remove (array, (gpointer) items [7]);

	ASSERT_TRUE (g_ptr_array_remove (array, (gpointer) items [4]))
		<< "Item " << items [4] << " not removed";
	ASSERT_FALSE (g_ptr_array_remove (array, (gpointer) items [4]))
		<< "Item " << items [4] << " still in array after removal";
	ASSERT_EQ ((gpointer) items [array->len + 1], array->pdata [array->len - 1])
		<< "Last item in GPtrArray not correct";

	g_ptr_array_free (array, TRUE);
}

TEST (ptrarray, sort)
{
	GPtrArray *array = g_ptr_array_new ();

	g_ptr_array_add (array, letters [0]);
	g_ptr_array_add (array, letters [1]);
	g_ptr_array_add (array, letters [2]);
	g_ptr_array_add (array, letters [3]);
	g_ptr_array_add (array, letters [4]);

	g_ptr_array_sort (array, sort_compare);

	for (guint i = 0; i < array->len; i++)
		ASSERT_EQ ((gpointer) letters [i], array->pdata [i])
			<< "Array out of order at position " << i;

	g_ptr_array_free (array, TRUE);
}

TEST (ptrarray, remove_fast)
{
	GPtrArray *array = g_ptr_array_new ();

	ASSERT_FALSE (g_ptr_array_remove_fast (array, NULL)) << "Removing NULL succeeded";

	g_ptr_array_add (array, letters [0]);
	ASSERT_TRUE (g_ptr_array_remove_fast (array, letters [0])) << "Removing last element failed";
	ASSERT_EQ (0u, array->len);

	g_ptr_array_add (array, letters [0]);
	g_ptr_array_add (array, letters [1]);
	g_ptr_array_add (array, letters [2]);
	g_ptr_array_add (array, letters [3]);
	g_ptr_array_add (array, letters [4]);

	ASSERT_TRUE (g_ptr_array_remove_fast (array, letters [0])) << "Removing first element failed";
	ASSERT_EQ (4u, array->len);
	ASSERT_EQ ((gpointer) letters [4], array->pdata [0])
		<< "First element wasn't replaced with last upon removal";

	ASSERT_FALSE (g_ptr_array_remove_fast (array, letters [0]))
		<< "Succeeded removing a non-existing element";

	ASSERT_TRUE (g_ptr_array_remove_fast (array, letters [3])) << "Failed removing \"D\"";
	ASSERT_EQ (3u, array->len);

	ASSERT_TRUE (g_ptr_array_remove_fast (array, letters [1])) << "Failed removing \"B\"";
	ASSERT_EQ (2u, array->len);

	ASSERT_EQ ((gpointer) letters [4], array->pdata [0]) << "Last two elements are wrong";
	ASSERT_EQ ((gpointer) letters [2], array->pdata [1]) << "Last two elements are wrong";

	g_ptr_array_free (array, TRUE);
}

TEST (ptrarray, sort_with_data)
{
	GPtrArray *array = g_ptr_array_new ();

	g_ptr_array_add (array, letters [4]);
	g_ptr_array_add (array, letters [1]);
	g_ptr_array_add (array, letters [2]);
	g_ptr_array_add (array, letters [0]);
	g_ptr_array_add (array, letters [3]);

	g_ptr_array_sort_with_data (array, sort_compare_with_data,
				    (char*)"this is the data for qsort");

	for (guint i = 0; i < array->len; i++)
		ASSERT_EQ ((gpointer) letters [i], array->pdata [i])
			<< "Array out of order at position " << i;

	g_ptr_array_free (array, TRUE);
}

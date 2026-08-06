#include <string_view>

#include <glib.h>
#include <gtest/gtest.h>

namespace {

/* Orders by length, so equal-length entries land in insertion order. */
gint
compare_by_length (gconstpointer a, gconstpointer b)
{
	if (std::string_view ((const char *) a).size () < std::string_view ((const char *) b).size ())
		return -1;

	return 1;
}

gint
compare_strings (gconstpointer a, gconstpointer b)
{
	return std::string_view ((const char *) a).compare ((const char *) b);
}

int
compare_ints (gconstpointer p1, gconstpointer p2)
{
	return GPOINTER_TO_INT (p1) - GPOINTER_TO_INT (p2);
}

const char *
data_of (GList *node)
{
	return (const char *) node->data;
}

/* Ascending, `len` entries long, and doubly linked in both directions. */
bool
is_sorted (GList *list, int len)
{
	if (list->prev)
		return false;

	int prev = GPOINTER_TO_INT (list->data);
	len--;
	for (list = list->next; list; list = list->next) {
		int curr = GPOINTER_TO_INT (list->data);
		if (prev > curr)
			return false;
		prev = curr;

		if (!list->prev || list->prev->next != list)
			return false;

		if (len == 0)
			return false;
		len--;
	}
	return len == 0;
}

const int N_ELEMS = 101;

}

TEST (list, length)
{
	GList *list = g_list_prepend (NULL, (char*)"foo");
	ASSERT_EQ (1u, g_list_length (list));

	list = g_list_prepend (list, (char*)"bar");
	ASSERT_EQ (2u, g_list_length (list));

	list = g_list_append (list, (char*)"bar");
	ASSERT_EQ (3u, g_list_length (list));

	g_list_free (list);
}

TEST (list, nth)
{
	char *foo = (char*)"foo";
	char *bar = (char*)"bar";
	char *baz = (char*)"baz";

	GList *list = g_list_prepend (NULL, baz);
	list = g_list_prepend (list, bar);
	list = g_list_prepend (list, foo);

	ASSERT_EQ (foo, g_list_nth (list, 0)->data);
	ASSERT_EQ (bar, g_list_nth (list, 1)->data);
	ASSERT_EQ (baz, g_list_nth (list, 2)->data);
	ASSERT_EQ (nullptr, g_list_nth (list, 3));

	g_list_free (list);
}

TEST (list, index)
{
	char *foo = (char*)"foo";
	char *bar = (char*)"bar";
	char *baz = (char*)"baz";

	GList *list = g_list_prepend (NULL, baz);
	list = g_list_prepend (list, bar);
	list = g_list_prepend (list, foo);

	ASSERT_EQ (0, g_list_index (list, foo));
	ASSERT_EQ (1, g_list_index (list, bar));
	ASSERT_EQ (2, g_list_index (list, baz));

	g_list_free (list);
}

TEST (list, append)
{
	GList *list = g_list_prepend (NULL, (char*)"first");
	ASSERT_EQ (1u, g_list_length (list)) << "Prepend failed";

	list = g_list_append (list, (char*)"second");
	ASSERT_EQ (2u, g_list_length (list)) << "Append failed";

	g_list_free (list);
}

TEST (list, last)
{
	GList *foo = g_list_prepend (NULL, (char*)"foo");
	GList *bar = g_list_prepend (NULL, (char*)"bar");

	foo = g_list_concat (foo, bar);
	ASSERT_EQ (bar, g_list_last (foo));

	foo = g_list_concat (foo, g_list_prepend (NULL, (char*)"baz"));
	foo = g_list_concat (foo, g_list_prepend (NULL, (char*)"quux"));

	ASSERT_STREQ ("quux", data_of (g_list_last (foo)));

	g_list_free (foo);
}

TEST (list, concat)
{
	GList *foo = g_list_prepend (NULL, (char*)"foo");
	GList *bar = g_list_prepend (NULL, (char*)"bar");
	GList *list = g_list_concat (foo, bar);

	ASSERT_EQ (2u, g_list_length (list));
	ASSERT_STREQ ("foo", data_of (list));
	ASSERT_STREQ ("bar", data_of (list->next));
	ASSERT_EQ (foo, g_list_first (list));
	ASSERT_EQ (bar, g_list_last (list));

	g_list_free (list);
}

TEST (list, insert_sorted)
{
	GList *list = g_list_prepend (NULL, (char*)"a");
	list = g_list_append (list, (char*)"aaa");

	/* insert at the middle */
	list = g_list_insert_sorted (list, (char*)"aa", compare_by_length);
	ASSERT_STREQ ("aa", data_of (list->next));

	/* insert at the beginning */
	list = g_list_insert_sorted (list, (char*)"", compare_by_length);
	ASSERT_STREQ ("", data_of (list));

	/* insert at the end */
	list = g_list_insert_sorted (list, (char*)"aaaa", compare_by_length);
	ASSERT_STREQ ("aaaa", data_of (g_list_last (list)));

	g_list_free (list);
}

TEST (list, insert_before)
{
	GList *foo = g_list_prepend (NULL, (char*)"foo");
	foo = g_list_insert_before (foo, NULL, (char*)"bar");

	GList *bar = g_list_last (foo);
	ASSERT_STREQ ("bar", data_of (bar));

	GList *baz = g_list_insert_before (foo, bar, (char*)"baz");
	ASSERT_EQ (foo, baz);
	ASSERT_STREQ ("baz", (const char *) g_list_nth_data (foo, 1));

	g_list_free (foo);
}

TEST (list, copy)
{
	GList *list = g_list_prepend (NULL, (char*)"a");
	list = g_list_append (list, (char*)"aa");
	list = g_list_append (list, (char*)"aaa");
	list = g_list_append (list, (char*)"aaaa");

	guint length = g_list_length (list);
	GList *copy = g_list_copy (list);

	for (guint i = 0; i < length; i++)
		ASSERT_STREQ (data_of (g_list_nth (list, i)), data_of (g_list_nth (copy, i)))
			<< "at index " << i;

	g_list_free (list);
	g_list_free (copy);
}

TEST (list, reverse)
{
	GList *list = g_list_prepend (NULL, (char*)"a");
	list = g_list_append (list, (char*)"aa");
	list = g_list_append (list, (char*)"aaa");
	list = g_list_append (list, (char*)"aaaa");

	guint length = g_list_length (list);
	GList *reverse = g_list_reverse (g_list_copy (list));

	ASSERT_EQ (length, g_list_length (reverse));

	for (guint i = 0; i < length; i++) {
		guint j = length - i - 1;
		ASSERT_STREQ (data_of (g_list_nth (list, i)), data_of (g_list_nth (reverse, j)))
			<< "at index " << i;
	}

	g_list_free (list);
	g_list_free (reverse);
}

TEST (list, remove)
{
	char *one = (char*)"one";
	GList *list = g_list_prepend (NULL, (char*)"three");
	list = g_list_prepend (list, (char*)"two");
	list = g_list_prepend (list, one);

	list = g_list_remove (list, one);

	ASSERT_EQ (2u, g_list_length (list));
	ASSERT_STREQ ("two", data_of (list));

	g_list_free (list);
}

TEST (list, remove_link)
{
	GList *foo = g_list_prepend (NULL, (char*)"a");
	GList *bar = g_list_prepend (NULL, (char*)"b");
	GList *baz = g_list_prepend (NULL, (char*)"c");
	GList *list = foo;

	foo = g_list_concat (foo, bar);
	foo = g_list_concat (foo, baz);

	list = g_list_remove_link (list, bar);

	ASSERT_EQ (2u, g_list_length (list));
	ASSERT_EQ (nullptr, bar->next);

	g_list_free (list);
	g_list_free (bar);
}

TEST (list, sort)
{
	GList *list = NULL;

	for (int i = 0; i < N_ELEMS; ++i)
		list = g_list_prepend (list, GINT_TO_POINTER (i));
	list = g_list_sort (list, compare_ints);
	ASSERT_TRUE (is_sorted (list, N_ELEMS)) << "decreasing list";
	g_list_free (list);

	list = NULL;
	for (int i = 0; i < N_ELEMS; ++i)
		list = g_list_prepend (list, GINT_TO_POINTER (-i));
	list = g_list_sort (list, compare_ints);
	ASSERT_TRUE (is_sorted (list, N_ELEMS)) << "increasing list";
	g_list_free (list);

	list = g_list_prepend (NULL, GINT_TO_POINTER (0));
	for (int i = 1; i < N_ELEMS; ++i) {
		list = g_list_prepend (list, GINT_TO_POINTER (i));
		list = g_list_prepend (list, GINT_TO_POINTER (-i));
	}
	list = g_list_sort (list, compare_ints);
	ASSERT_TRUE (is_sorted (list, 2 * N_ELEMS - 1)) << "alternating list";
	g_list_free (list);

	list = NULL;
	int mul = 1;
	for (int i = 1; i < N_ELEMS; ++i) {
		mul = -mul;
		for (int j = 0; j < i; ++j)
			list = g_list_prepend (list, GINT_TO_POINTER (mul * j));
	}
	list = g_list_sort (list, compare_ints);
	ASSERT_TRUE (is_sorted (list, (N_ELEMS * N_ELEMS - N_ELEMS) / 2)) << "wavering list";
	g_list_free (list);
}

TEST (list, find_custom)
{
	char *foo = (char*)"foo";
	char *bar = (char*)"bar";
	char *baz = (char*)"baz";

	GList *list = NULL;
	list = g_list_prepend (list, baz);
	list = g_list_prepend (list, bar);
	list = g_list_prepend (list, foo);

	ASSERT_NE (nullptr, g_list_find_custom (list, baz, compare_strings));

	g_list_free (list);
}

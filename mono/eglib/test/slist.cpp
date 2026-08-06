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
data_of (GSList *node)
{
	return (const char *) node->data;
}

bool
is_sorted (GSList *list, int len)
{
	int prev = GPOINTER_TO_INT (list->data);
	len--;
	for (list = list->next; list; list = list->next) {
		int curr = GPOINTER_TO_INT (list->data);
		if (prev > curr)
			return false;
		prev = curr;

		if (len == 0)
			return false;
		len--;
	}
	return len == 0;
}

const int N_ELEMS = 100;

}

TEST (slist, nth)
{
	char *foo = (char*)"foo";
	char *bar = (char*)"bar";
	char *baz = (char*)"baz";

	GSList *list = g_slist_prepend (NULL, baz);
	list = g_slist_prepend (list, bar);
	list = g_slist_prepend (list, foo);

	ASSERT_EQ (foo, g_slist_nth (list, 0)->data);
	ASSERT_EQ (bar, g_slist_nth (list, 1)->data);
	ASSERT_EQ (baz, g_slist_nth (list, 2)->data);
	ASSERT_EQ (nullptr, g_slist_nth (list, 3));

	g_slist_free (list);
}

TEST (slist, index)
{
	char *foo = (char*)"foo";
	char *bar = (char*)"bar";
	char *baz = (char*)"baz";

	GSList *list = g_slist_prepend (NULL, baz);
	list = g_slist_prepend (list, bar);
	list = g_slist_prepend (list, foo);

	ASSERT_EQ (0, g_slist_index (list, foo));
	ASSERT_EQ (1, g_slist_index (list, bar));
	ASSERT_EQ (2, g_slist_index (list, baz));

	g_slist_free (list);
}

TEST (slist, append)
{
	GSList *list = g_slist_append (NULL, (char*)"first");
	ASSERT_EQ (1u, g_slist_length (list)) << "append(null,...) failed";

	GSList *foo = g_slist_append (list, (char*)"second");
	ASSERT_EQ (list, foo) << "changed list head on non-empty";
	ASSERT_EQ (2u, g_slist_length (list)) << "Append failed";

	g_slist_free (list);
}

TEST (slist, concat)
{
	GSList *foo = g_slist_prepend (NULL, (char*)"foo");
	GSList *bar = g_slist_prepend (NULL, (char*)"bar");
	GSList *list = g_slist_concat (foo, bar);

	ASSERT_EQ (2u, g_slist_length (list));

	g_slist_free (list);
}

TEST (slist, find)
{
	GSList *list = g_slist_prepend (NULL, (char*)"three");
	list = g_slist_prepend (list, (char*)"two");
	list = g_slist_prepend (list, (char*)"one");

	char *data = (char*)"four";
	list = g_slist_append (list, data);

	ASSERT_EQ (data, g_slist_find (list, data)->data);

	g_slist_free (list);
}

TEST (slist, find_custom)
{
	char *foo = (char*)"foo";
	char *bar = (char*)"bar";
	char *baz = (char*)"baz";

	GSList *list = NULL;
	list = g_slist_prepend (list, baz);
	list = g_slist_prepend (list, bar);
	list = g_slist_prepend (list, foo);

	ASSERT_NE (nullptr, g_slist_find_custom (list, baz, compare_strings));

	g_slist_free (list);
}

TEST (slist, remove)
{
	char *one = (char*)"one";
	GSList *list = g_slist_prepend (NULL, (char*)"three");
	list = g_slist_prepend (list, (char*)"two");
	list = g_slist_prepend (list, one);

	list = g_slist_remove (list, one);

	ASSERT_EQ (2u, g_slist_length (list));
	ASSERT_STREQ ("two", data_of (list));

	g_slist_free (list);
}

TEST (slist, remove_link)
{
	GSList *foo = g_slist_prepend (NULL, (char*)"a");
	GSList *bar = g_slist_prepend (NULL, (char*)"b");
	GSList *baz = g_slist_prepend (NULL, (char*)"c");
	GSList *list = foo;

	foo = g_slist_concat (foo, bar);
	foo = g_slist_concat (foo, baz);

	list = g_slist_remove_link (list, bar);

	ASSERT_EQ (2u, g_slist_length (list));
	ASSERT_EQ (nullptr, bar->next);

	g_slist_free (list);
	g_slist_free (bar);
}

TEST (slist, insert_sorted)
{
	GSList *list = g_slist_prepend (NULL, (char*)"a");
	list = g_slist_append (list, (char*)"aaa");

	/* insert at the middle */
	list = g_slist_insert_sorted (list, (char*)"aa", compare_by_length);
	ASSERT_STREQ ("aa", data_of (list->next));

	/* insert at the beginning */
	list = g_slist_insert_sorted (list, (char*)"", compare_by_length);
	ASSERT_STREQ ("", data_of (list));

	/* insert at the end */
	list = g_slist_insert_sorted (list, (char*)"aaaa", compare_by_length);
	ASSERT_STREQ ("aaaa", data_of (g_slist_last (list)));

	g_slist_free (list);
}

TEST (slist, insert_before)
{
	GSList *foo = g_slist_prepend (NULL, (char*)"foo");
	foo = g_slist_insert_before (foo, NULL, (char*)"bar");

	GSList *bar = g_slist_last (foo);
	ASSERT_STREQ ("bar", data_of (bar));

	GSList *baz = g_slist_insert_before (foo, bar, (char*)"baz");
	ASSERT_EQ (foo, baz);
	ASSERT_STREQ ("baz", data_of (foo->next));

	g_slist_free (foo);
}

TEST (slist, sort)
{
	GSList *list = NULL;

	for (int i = 0; i < N_ELEMS; ++i)
		list = g_slist_prepend (list, GINT_TO_POINTER (i));
	list = g_slist_sort (list, compare_ints);
	ASSERT_TRUE (is_sorted (list, N_ELEMS)) << "decreasing list";
	g_slist_free (list);

	list = NULL;
	for (int i = 0; i < N_ELEMS; ++i)
		list = g_slist_prepend (list, GINT_TO_POINTER (-i));
	list = g_slist_sort (list, compare_ints);
	ASSERT_TRUE (is_sorted (list, N_ELEMS)) << "increasing list";
	g_slist_free (list);

	list = g_slist_prepend (NULL, GINT_TO_POINTER (0));
	for (int i = 1; i < N_ELEMS; ++i) {
		list = g_slist_prepend (list, GINT_TO_POINTER (-i));
		list = g_slist_prepend (list, GINT_TO_POINTER (i));
	}
	list = g_slist_sort (list, compare_ints);
	ASSERT_TRUE (is_sorted (list, 2 * N_ELEMS - 1)) << "alternating list";
	g_slist_free (list);

	list = NULL;
	int mul = 1;
	for (int i = 1; i < N_ELEMS; ++i) {
		mul = -mul;
		for (int j = 0; j < i; ++j)
			list = g_slist_prepend (list, GINT_TO_POINTER (mul * j));
	}
	list = g_slist_sort (list, compare_ints);
	ASSERT_TRUE (is_sorted (list, (N_ELEMS * N_ELEMS - N_ELEMS) / 2)) << "wavering list";
	g_slist_free (list);
}

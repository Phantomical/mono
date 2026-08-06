#include <glib.h>
#include <gtest/gtest.h>

namespace {

/*
 * `sentinel` is how the callback tells whether foreach handed it back the
 * user_data pointer it was registered with, rather than something else.
 */
struct ForeachState {
	int count;
	int sentinel;
	bool wrong_user_data;
};

void
foreach_counter (gpointer key, gpointer value, gpointer user_data)
{
	ForeachState *state = (ForeachState *) user_data;
	state->count++;
	if (state->sentinel != 'a')
		state->wrong_user_data = true;
}

void
counter (gpointer key, gpointer value, gpointer user_data)
{
	int *count = (int *) user_data;
	(*count)++;
}

}

TEST (hashtable, t1)
{
	GHashTable *t = g_hash_table_new (g_str_hash, g_str_equal);
	ForeachState state = { 0, 'a', false };

	g_hash_table_insert (t, (char*)"hello", (char*)"world");
	g_hash_table_insert (t, (char*)"my", (char*)"god");

	g_hash_table_foreach (t, foreach_counter, &state);
	ASSERT_EQ (2, state.count) << "did not find all keys";
	ASSERT_FALSE (state.wrong_user_data) << "failed to pass the user-data to foreach";

	ASSERT_TRUE (g_hash_table_remove (t, (char*)"my")) << "did not find known key";
	ASSERT_EQ (1u, g_hash_table_size (t));

	g_hash_table_insert (t, (char*)"hello", (char*)"moon");
	ASSERT_STREQ ("moon", (const char *) g_hash_table_lookup (t, (char*)"hello"))
		<< "did not replace world with moon";

	ASSERT_TRUE (g_hash_table_remove (t, (char*)"hello")) << "did not find known key";
	ASSERT_EQ (0u, g_hash_table_size (t));

	g_hash_table_destroy (t);
}

TEST (hashtable, grow)
{
	GHashTable *hash = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
	int count = 0;

	for (int i = 0; i < 1000; i++)
		g_hash_table_insert (hash, g_strdup_printf ("%d", i), g_strdup_printf ("x-%d", i));

	for (int i = 0; i < 1000; i++) {
		char key [30];
		char expected [30];

		sprintf (key, "%d", i);
		sprintf (expected, "x-%d", i);

		ASSERT_STREQ (expected, (const char *) g_hash_table_lookup (hash, key))
			<< "failed to look up key " << i;
	}

	ASSERT_EQ (1000u, g_hash_table_size (hash));

	/* Now do the manual count, lets not trust the internals */
	g_hash_table_foreach (hash, counter, &count);
	ASSERT_EQ (1000, count);

	g_hash_table_destroy (hash);
}

TEST (hashtable, default_hash)
{
	GHashTable *hash = g_hash_table_new (NULL, NULL);

	ASSERT_NE (nullptr, hash) << "g_hash_table_new should return a valid hash";

	g_hash_table_destroy (hash);
}

TEST (hashtable, null_lookup)
{
	GHashTable *hash = g_hash_table_new (NULL, NULL);
	gpointer ok, ov;

	g_hash_table_insert (hash, NULL, GINT_TO_POINTER (1));
	g_hash_table_insert (hash, GINT_TO_POINTER (1), GINT_TO_POINTER (2));

	ASSERT_TRUE (g_hash_table_lookup_extended (hash, NULL, &ok, &ov))
		<< "Did not find the NULL";
	ASSERT_EQ (nullptr, ok) << "Incorrect key found";
	ASSERT_EQ (GINT_TO_POINTER (1), ov);

	ASSERT_TRUE (g_hash_table_lookup_extended (hash, GINT_TO_POINTER (1), &ok, &ov))
		<< "Did not find the 1";
	ASSERT_EQ (GINT_TO_POINTER (1), ok) << "Incorrect key found";
	ASSERT_EQ (GINT_TO_POINTER (2), ov);

	g_hash_table_destroy (hash);
}

TEST (hashtable, iter)
{
	GHashTable *hash = g_hash_table_new_full (g_direct_hash, g_direct_equal, NULL, NULL);
	GHashTableIter iter;
	gpointer key, value;
	int sum = 0;

	for (int i = 0; i < 1000; i++) {
		sum += i;
		g_hash_table_insert (hash, GUINT_TO_POINTER (i), GUINT_TO_POINTER (i));
	}

	int keys_sum = 0, values_sum = 0;
	g_hash_table_iter_init (&iter, hash);
	while (g_hash_table_iter_next (&iter, &key, &value)) {
		ASSERT_EQ (key, value);
		keys_sum += GPOINTER_TO_UINT (key);
		values_sum += GPOINTER_TO_UINT (value);
	}
	ASSERT_EQ (sum, keys_sum) << "Did not find all key-value pairs";
	ASSERT_EQ (sum, values_sum) << "Did not find all key-value pairs";

	g_hash_table_destroy (hash);
}

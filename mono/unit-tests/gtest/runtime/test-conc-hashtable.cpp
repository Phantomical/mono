/*
 * test-conc-hashtable.cpp: Unit test for the concurrent hashtable.
 *
 * Copyright (C) 2014 Xamarin Inc
 *
 * Licensed under the MIT license. See LICENSE file in the project root for full license information.
 */

#include "config.h"

#include "utils/mono-threads.h"
#include "utils/mono-conc-hashtable.h"
#include "utils/checked-build.h"
#include "metadata/w32handle.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <pthread.h>

#include <gtest/gtest.h>

namespace {

MonoConcurrentHashTable *hash;
mono_mutex_t global_mutex;
int running = 1;

void
monotest_thread_state_init (MonoThreadUnwindState *ctx)
{
}

#define monotest_setup_async_callback          NULL
#define monotest_thread_state_init_from_sigctx NULL
#define monotest_thread_state_init_from_handle NULL

/*
 * The table wants a thread-info subsystem under it, and that can only be stood
 * up once in a process -- so it happens per suite rather than per test, and the
 * tests each bring their own table.
 */
class ConcHashTable : public ::testing::Test {
public:
	static void SetUpTestSuite ()
	{
		static const MonoThreadInfoRuntimeCallbacks ticallbacks = {
			MONO_THREAD_INFO_RUNTIME_CALLBACKS (MONO_INIT_CALLBACK, monotest)
		};

		/*
		 * Standing the subsystem up a second time hands out small ids that no
		 * longer match their slots, and the hazard-pointer table asserts on it.
		 * gtest reaches here again under --gtest_repeat, so the guard is not
		 * theoretical.
		 */
		static bool started = false;
		if (started)
			return;
		started = true;

		CHECKED_MONO_INIT ();
		mono_thread_info_init (sizeof (MonoThreadInfo));
		mono_thread_info_runtime_init (&ticallbacks);
#ifndef HOST_WIN32
		mono_w32handle_init ();
#endif

		mono_thread_info_attach ();
	}

protected:
	void SetUp () override
	{
		mono_os_mutex_init (&global_mutex);
		hash = mono_conc_hashtable_new (NULL, NULL);
	}

	void TearDown () override
	{
		mono_conc_hashtable_destroy (hash);
		mono_os_mutex_destroy (&global_mutex);
		hash = NULL;
	}

	static void insert_locked (gpointer key, gpointer value)
	{
		mono_os_mutex_lock (&global_mutex);
		mono_conc_hashtable_insert (hash, key, value);
		mono_os_mutex_unlock (&global_mutex);
	}
};

void*
pw_sr_thread (void *arg)
{
	int i, idx = 1000 * GPOINTER_TO_INT (arg);
	mono_thread_info_attach ();

	for (i = 0; i < 1000; ++i) {
		mono_os_mutex_lock (&global_mutex);
		mono_conc_hashtable_insert (hash, GINT_TO_POINTER (i + idx), GINT_TO_POINTER (i + 1));
		mono_os_mutex_unlock (&global_mutex);
	}
	return NULL;
}

/* Spins until every key it was given a range for reads back the value it expects. */
void*
pr_sw_thread (void *arg)
{
	int i = 0, idx = 100 * GPOINTER_TO_INT (arg);
	mono_thread_info_attach ();

	while (i < 100) {
		gpointer res = mono_conc_hashtable_lookup (hash, GINT_TO_POINTER (i + idx + 1));
		if (!res)
			continue;
		if (res != GINT_TO_POINTER ((i + idx) * 2 + 1))
			return GINT_TO_POINTER (i);
		++i;
	}
	return NULL;
}

/*
 * i is not incremented as long as running is set, which guarantees a full pass
 * over the keys after the writer threads have finished.
 */
void*
pw_pr_r_thread (void *arg)
{
	int key, val, i;
	mono_thread_info_attach ();

	for (i = 0; i < 2; i += 1 - running) {
		for (key = 1; key < 3 * 1000 + 1; key++) {
			val = GPOINTER_TO_INT (mono_conc_hashtable_lookup (hash, GINT_TO_POINTER (key)));

			if (!val)
				continue;
			if (key != val)
				return GINT_TO_POINTER (key);
		}
	}
	return NULL;
}

void*
pw_pr_w_add_thread (void *arg)
{
	int i, idx = 1000 * GPOINTER_TO_INT (arg);

	mono_thread_info_attach ();

	for (i = idx; i < idx + 1000; i++) {
		mono_os_mutex_lock (&global_mutex);
		mono_conc_hashtable_insert (hash, GINT_TO_POINTER (i + 1), GINT_TO_POINTER (i + 1));
		mono_os_mutex_unlock (&global_mutex);
	}
	return NULL;
}

void*
pw_pr_w_del_thread (void *arg)
{
	int i, idx = 1000 * GPOINTER_TO_INT (arg);

	mono_thread_info_attach ();

	for (i = idx; i < idx + 1000; i++) {
		mono_os_mutex_lock (&global_mutex);
		mono_conc_hashtable_remove (hash, GINT_TO_POINTER (i + 1));
		mono_os_mutex_unlock (&global_mutex);
	}
	return NULL;
}

} // namespace

TEST_F (ConcHashTable, SingleWriterSingleReader)
{
	insert_locked (GUINT_TO_POINTER (10), GUINT_TO_POINTER (20));
	insert_locked (GUINT_TO_POINTER (30), GUINT_TO_POINTER (40));
	insert_locked (GUINT_TO_POINTER (50), GUINT_TO_POINTER (60));
	insert_locked (GUINT_TO_POINTER (2), GUINT_TO_POINTER (3));

	EXPECT_EQ (GUINT_TO_POINTER (40), mono_conc_hashtable_lookup (hash, GUINT_TO_POINTER (30)));
	EXPECT_EQ (GUINT_TO_POINTER (20), mono_conc_hashtable_lookup (hash, GUINT_TO_POINTER (10)));
	EXPECT_EQ (GUINT_TO_POINTER (3), mono_conc_hashtable_lookup (hash, GUINT_TO_POINTER (2)));
	EXPECT_EQ (GUINT_TO_POINTER (60), mono_conc_hashtable_lookup (hash, GUINT_TO_POINTER (50)));
}

TEST_F (ConcHashTable, ParallelWriterSingleReader)
{
	pthread_t a, b, c;

	pthread_create (&a, NULL, pw_sr_thread, GINT_TO_POINTER (1));
	pthread_create (&b, NULL, pw_sr_thread, GINT_TO_POINTER (2));
	pthread_create (&c, NULL, pw_sr_thread, GINT_TO_POINTER (3));

	pthread_join (a, NULL);
	pthread_join (b, NULL);
	pthread_join (c, NULL);

	for (int i = 0; i < 1000; ++i) {
		for (int j = 1; j < 4; ++j) {
			ASSERT_EQ (GINT_TO_POINTER (i + 1),
				   mono_conc_hashtable_lookup (hash, GINT_TO_POINTER (j * 1000 + i)))
				<< "writer " << j << " key " << (j * 1000 + i);
		}
	}
}

TEST_F (ConcHashTable, SingleWriterParallelReader)
{
	pthread_t a, b, c;
	gpointer ra, rb, rc;

	pthread_create (&a, NULL, pr_sw_thread, GINT_TO_POINTER (0));
	pthread_create (&b, NULL, pr_sw_thread, GINT_TO_POINTER (1));
	pthread_create (&c, NULL, pr_sw_thread, GINT_TO_POINTER (2));

	for (int i = 0; i < 100; ++i) {
		insert_locked (GINT_TO_POINTER (i +   0 + 1), GINT_TO_POINTER ((i +   0) * 2 + 1));
		insert_locked (GINT_TO_POINTER (i + 100 + 1), GINT_TO_POINTER ((i + 100) * 2 + 1));
		insert_locked (GINT_TO_POINTER (i + 200 + 1), GINT_TO_POINTER ((i + 200) * 2 + 1));
	}

	pthread_join (a, &ra);
	pthread_join (b, &rb);
	pthread_join (c, &rc);

	/* A reader hands back the key it read a stale value for, or NULL. */
	EXPECT_EQ (0, GPOINTER_TO_INT (ra));
	EXPECT_EQ (0, GPOINTER_TO_INT (rb));
	EXPECT_EQ (0, GPOINTER_TO_INT (rc));
}

TEST_F (ConcHashTable, ParallelWriterParallelReader)
{
	pthread_t wa, wb, wc, ra, rb, rc;
	gpointer a, b, c;

	srand (time (NULL));

	/* Pass 0 fills the table while the readers run over it, pass 1 empties it. */
	for (int i = 0; i < 2; i++) {
		SCOPED_TRACE (i == 0 ? "adding" : "removing");
		running = 1;

		pthread_create (&ra, NULL, pw_pr_r_thread, NULL);
		pthread_create (&rb, NULL, pw_pr_r_thread, NULL);
		pthread_create (&rc, NULL, pw_pr_r_thread, NULL);

		void *(*writer) (void *) = i == 0 ? pw_pr_w_add_thread : pw_pr_w_del_thread;
		pthread_create (&wa, NULL, writer, GINT_TO_POINTER (0));
		pthread_create (&wb, NULL, writer, GINT_TO_POINTER (1));
		pthread_create (&wc, NULL, writer, GINT_TO_POINTER (2));

		pthread_join (wa, NULL);
		pthread_join (wb, NULL);
		pthread_join (wc, NULL);

		running = 0;

		pthread_join (ra, &a);
		pthread_join (rb, &b);
		pthread_join (rc, &c);

		/* A reader hands back the key whose value did not match it, or NULL. */
		EXPECT_EQ (0, GPOINTER_TO_INT (a));
		EXPECT_EQ (0, GPOINTER_TO_INT (b));
		EXPECT_EQ (0, GPOINTER_TO_INT (c));
	}
}

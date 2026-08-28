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

#include <thread>

#include <gtest/gtest.h>

namespace {

/*
 * A worker thread and the answer it hands back.  The bodies below are written
 * as the pthread entry points they were, and std::thread drops what its
 * callable returns, so the result comes back through a member instead.
 */
struct Worker {
	std::thread thread;
	void *result = nullptr;

	Worker (void *(*body) (void *), void *arg)
		: thread ([this, body, arg] { result = body (arg); })
	{
	}

	void *join ()
	{
		thread.join ();
		return result;
	}
};

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
	Worker a (pw_sr_thread, GINT_TO_POINTER (1));
	Worker b (pw_sr_thread, GINT_TO_POINTER (2));
	Worker c (pw_sr_thread, GINT_TO_POINTER (3));

	a.join ();
	b.join ();
	c.join ();

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
	Worker a (pr_sw_thread, GINT_TO_POINTER (0));
	Worker b (pr_sw_thread, GINT_TO_POINTER (1));
	Worker c (pr_sw_thread, GINT_TO_POINTER (2));
	gpointer ra, rb, rc;

	for (int i = 0; i < 100; ++i) {
		insert_locked (GINT_TO_POINTER (i +   0 + 1), GINT_TO_POINTER ((i +   0) * 2 + 1));
		insert_locked (GINT_TO_POINTER (i + 100 + 1), GINT_TO_POINTER ((i + 100) * 2 + 1));
		insert_locked (GINT_TO_POINTER (i + 200 + 1), GINT_TO_POINTER ((i + 200) * 2 + 1));
	}

	ra = a.join ();
	rb = b.join ();
	rc = c.join ();

	/* A reader hands back the key it read a stale value for, or NULL. */
	EXPECT_EQ (0, GPOINTER_TO_INT (ra));
	EXPECT_EQ (0, GPOINTER_TO_INT (rb));
	EXPECT_EQ (0, GPOINTER_TO_INT (rc));
}

TEST_F (ConcHashTable, ParallelWriterParallelReader)
{
	gpointer a, b, c;

	srand (time (NULL));

	/* Pass 0 fills the table while the readers run over it, pass 1 empties it. */
	for (int i = 0; i < 2; i++) {
		SCOPED_TRACE (i == 0 ? "adding" : "removing");
		running = 1;

		Worker ra (pw_pr_r_thread, NULL);
		Worker rb (pw_pr_r_thread, NULL);
		Worker rc (pw_pr_r_thread, NULL);

		void *(*writer) (void *) = i == 0 ? pw_pr_w_add_thread : pw_pr_w_del_thread;
		Worker wa (writer, GINT_TO_POINTER (0));
		Worker wb (writer, GINT_TO_POINTER (1));
		Worker wc (writer, GINT_TO_POINTER (2));

		wa.join ();
		wb.join ();
		wc.join ();

		running = 0;

		a = ra.join ();
		b = rb.join ();
		c = rc.join ();

		/* A reader hands back the key whose value did not match it, or NULL. */
		EXPECT_EQ (0, GPOINTER_TO_INT (a));
		EXPECT_EQ (0, GPOINTER_TO_INT (b));
		EXPECT_EQ (0, GPOINTER_TO_INT (c));
	}
}

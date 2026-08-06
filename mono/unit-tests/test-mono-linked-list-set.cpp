#include <assert.h>
#include <pthread.h>

#include <config.h>
#include <mono/metadata/metadata.h>
#include <mono/utils/mono-threads.h>
#include <mono/utils/hazard-pointer.h>
#include <mono/utils/mono-linked-list-set.h>
#include <mono/utils/atomic.h>

#include <gtest/gtest.h>

namespace {

MonoLinkedListSet lls;

enum {
	STATE_OUT,
	STATE_BUSY,
	STATE_IN
};

#define N 23
#define NUM_ITERS 1000000
#define NUM_THREADS 8

typedef struct {
	MonoLinkedListSetNode node;
	int state;
} node_t;

typedef struct {
	int skip;
	int num_adds;
	int num_removes;
	pthread_t thread;
} thread_data_t;

node_t nodes [N];

void
mono_hazard_pointer_clear_all (MonoThreadHazardPointers *hp, int retain)
{
	if (retain != 0)
		mono_hazard_pointer_clear (hp, 0);
	if (retain != 1)
		mono_hazard_pointer_clear (hp, 1);
	if (retain != 2)
		mono_hazard_pointer_clear (hp, 2);
}

void
free_node (void *n)
{
	node_t *node = (node_t *)n;
	assert (node->state == STATE_BUSY);
	node->state = STATE_OUT;
}

/*
 * The checks in here run on worker threads, where gtest's assertions are not
 * usable, so they stay plain assert (): a failure aborts the process and ctest
 * attributes it to this test.
 */
void*
worker (void *arg)
{
	thread_data_t *thread_data = (thread_data_t *)arg;
	MonoThreadHazardPointers *hp;
	int skip = thread_data->skip;
	int i, j;
	gboolean result;

	mono_thread_info_register_small_id ();

	hp = mono_hazard_pointer_get ();

	i = 0;
	for (j = 0; j < NUM_ITERS; ++j) {
		switch (nodes [i].state) {
		case STATE_BUSY:
			mono_thread_hazardous_try_free_some ();
			break;
		case STATE_OUT:
			if (mono_atomic_cas_i32 (&nodes [i].state, STATE_BUSY, STATE_OUT) == STATE_OUT) {
				result = mono_lls_find (&lls, hp, i);
				assert (!result);
				mono_hazard_pointer_clear_all (hp, -1);

				result = mono_lls_insert (&lls, hp, &nodes [i].node);
				mono_hazard_pointer_clear_all (hp, -1);

				assert (nodes [i].state == STATE_BUSY);
				nodes [i].state = STATE_IN;

				++thread_data->num_adds;
			}
			break;
		case STATE_IN:
			if (mono_atomic_cas_i32 (&nodes [i].state, STATE_BUSY, STATE_IN) == STATE_IN) {
				result = mono_lls_find (&lls, hp, i);
				assert (result);
				assert (mono_hazard_pointer_get_val (hp, 1) == &nodes [i].node);
				mono_hazard_pointer_clear_all (hp, -1);

				result = mono_lls_remove (&lls, hp, &nodes [i].node);
				mono_hazard_pointer_clear_all (hp, -1);

				++thread_data->num_removes;
			}
			break;
		default:
			assert (FALSE);
		}

		i += skip;
		if (i >= N)
			i -= N;
	}

	return NULL;
}

} // namespace

TEST (MonoLinkedListSet, ConcurrentInsertFindRemove)
{
	int primes [] = { 1, 2, 3, 5, 7, 11, 13, 17 };
	thread_data_t thread_data [NUM_THREADS];
	int i;

	mono_metadata_init ();

	mono_thread_info_init (0);

	mono_lls_init (&lls, free_node);

	for (i = 0; i < N; ++i) {
		nodes [i].node.key = i;
		nodes [i].state = STATE_OUT;
	}

	for (i = 0; i < NUM_THREADS; ++i) {
		thread_data [i].num_adds = thread_data [i].num_removes = 0;
		thread_data [i].skip = primes [i];
		ASSERT_EQ (0, pthread_create (&thread_data [i].thread, NULL, worker, &thread_data [i]));
	}

	for (i = 0; i < NUM_THREADS; ++i) {
		ASSERT_EQ (0, pthread_join (thread_data [i].thread, NULL));
		printf ("thread %d  adds %d  removes %d\n", i, thread_data [i].num_adds, thread_data [i].num_removes);
	}
}

#include <glib.h>
#include <gtest/gtest.h>

namespace {

const char *
data_of (GList *node)
{
	return (const char *) node->data;
}

}

TEST (queue, push)
{
	GQueue *queue = g_queue_new ();

	g_queue_push_head (queue, (char*)"foo");
	g_queue_push_head (queue, (char*)"bar");
	g_queue_push_head (queue, (char*)"baz");

	ASSERT_EQ (3u, queue->length) << "push failed";

	ASSERT_EQ (nullptr, queue->head->prev) << "HEAD: prev is wrong";
	ASSERT_STREQ ("baz", data_of (queue->head));
	ASSERT_STREQ ("bar", data_of (queue->head->next));
	ASSERT_STREQ ("foo", data_of (queue->head->next->next));
	ASSERT_EQ (nullptr, queue->head->next->next->next) << "HEAD: End is wrong";

	ASSERT_EQ (nullptr, queue->tail->next) << "TAIL: next is wrong";
	ASSERT_STREQ ("foo", data_of (queue->tail));
	ASSERT_STREQ ("bar", data_of (queue->tail->prev));
	ASSERT_STREQ ("baz", data_of (queue->tail->prev->prev));
	ASSERT_EQ (nullptr, queue->tail->prev->prev->prev) << "TAIL: End is wrong";

	g_queue_free (queue);
}

TEST (queue, push_tail)
{
	GQueue *queue = g_queue_new ();

	g_queue_push_tail (queue, (char*)"baz");
	g_queue_push_tail (queue, (char*)"bar");
	g_queue_push_tail (queue, (char*)"foo");

	ASSERT_EQ (3u, queue->length) << "push failed";

	ASSERT_EQ (nullptr, queue->head->prev) << "HEAD: prev is wrong";
	ASSERT_STREQ ("baz", data_of (queue->head));
	ASSERT_STREQ ("bar", data_of (queue->head->next));
	ASSERT_STREQ ("foo", data_of (queue->head->next->next));
	ASSERT_EQ (nullptr, queue->head->next->next->next) << "HEAD: End is wrong";

	ASSERT_EQ (nullptr, queue->tail->next) << "TAIL: next is wrong";
	ASSERT_STREQ ("foo", data_of (queue->tail));
	ASSERT_STREQ ("bar", data_of (queue->tail->prev));
	ASSERT_STREQ ("baz", data_of (queue->tail->prev->prev));
	ASSERT_EQ (nullptr, queue->tail->prev->prev->prev) << "TAIL: End is wrong";

	g_queue_free (queue);
}

TEST (queue, pop)
{
	GQueue *queue = g_queue_new ();

	g_queue_push_head (queue, (char*)"foo");
	g_queue_push_head (queue, (char*)"bar");
	g_queue_push_head (queue, (char*)"baz");

	ASSERT_STREQ ("baz", (const char *) g_queue_pop_head (queue));
	ASSERT_STREQ ("bar", (const char *) g_queue_pop_head (queue));
	ASSERT_STREQ ("foo", (const char *) g_queue_pop_head (queue));

	ASSERT_TRUE (g_queue_is_empty (queue));
	ASSERT_EQ (0u, queue->length);

	g_queue_push_head (queue, (char*)"foo");
	g_queue_push_head (queue, (char*)"bar");
	g_queue_push_head (queue, (char*)"baz");

	g_queue_pop_head (queue);

	ASSERT_EQ (nullptr, queue->head->prev) << "HEAD: prev is wrong";
	ASSERT_STREQ ("bar", data_of (queue->head));
	ASSERT_STREQ ("foo", data_of (queue->head->next));
	ASSERT_EQ (nullptr, queue->head->next->next) << "HEAD: End is wrong";

	ASSERT_EQ (nullptr, queue->tail->next) << "TAIL: next is wrong";
	ASSERT_STREQ ("foo", data_of (queue->tail));
	ASSERT_STREQ ("bar", data_of (queue->tail->prev));
	ASSERT_EQ (nullptr, queue->tail->prev->prev) << "TAIL: End is wrong";

	g_queue_free (queue);
}

TEST (queue, new_)
{
	GQueue *queue = g_queue_new ();

	ASSERT_EQ (0u, queue->length);
	ASSERT_EQ (nullptr, queue->head);
	ASSERT_EQ (nullptr, queue->tail);

	g_queue_free (queue);
}

TEST (queue, is_empty)
{
	GQueue *queue = g_queue_new ();

	ASSERT_TRUE (g_queue_is_empty (queue)) << "new queue should be empty";

	g_queue_push_head (queue, (char*)"foo");
	ASSERT_FALSE (g_queue_is_empty (queue));

	g_queue_free (queue);
}

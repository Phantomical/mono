/*
 * Tests for the queue background compiles run on.
 *
 * Two things have to hold. Teardown has to be able to prove work is finished:
 * closing a channel drops what is queued for it and waits for what is running,
 * so whatever the work was holding can then be destroyed. And nothing may ever
 * wait for a compile to produce an answer - the last test here holds mono's
 * loader lock while the worker blocks on it, which is the deadlock the whole
 * design is arranged to avoid, and it stays live because the thread that queued
 * the work never waits for it.
 */

#include "config.h"

#include <glib.h>

#include <mono/metadata/appdomain.h>
#include <mono/metadata/class.h>
#include <mono/metadata/class-internals.h>
#include <mono/metadata/object-internals.h>
#include <mono/utils/mono-error-internals.h>

/* mono-tls.h puts PIC back in scope, and it breaks some LLVM headers. */
#ifdef PIC
#undef PIC
#endif

#include "compile-queue.hpp"
#include "compile-worker.hpp"
#include "harness.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace mono {
namespace test {
namespace {

using namespace std::chrono_literals;

/// A gate a piece of work can be parked on, so that a test can decide when the
/// worker is busy and when it is free again.
class Gate {
public:
	void wait ()
	{
		std::unique_lock<std::mutex> lock (mutex_);

		cv_.wait (lock, [this] { return open_; });
	}

	void open ()
	{
		{
			std::lock_guard<std::mutex> lock (mutex_);

			open_ = true;
		}
		cv_.notify_all ();
	}

private:
	std::mutex mutex_;
	std::condition_variable cv_;
	bool open_ = false;
};

/// Spin until PREDICATE holds, or give up after a second and fail the test.
template <typename Predicate>
static void
wait_for (Predicate predicate, const char *what)
{
	auto deadline = std::chrono::steady_clock::now () + 1s;

	while (!predicate ()) {
		ASSERT_LT (std::chrono::steady_clock::now (), deadline) << what;
		std::this_thread::sleep_for (1ms);
	}
}

TEST (CompileQueue, RunsWorkSomewhereElse)
{
	CompileQueue queue;
	CompileQueue::Channel channel (&queue);

	std::atomic<bool> ran {false};
	std::thread::id where;

	ASSERT_TRUE (channel.enqueue (nullptr, [&] {
		where = std::this_thread::get_id ();
		ran.store (true);
	}));

	queue.drain ();
	EXPECT_TRUE (ran.load ());
	EXPECT_NE (where, std::this_thread::get_id ());
	EXPECT_EQ (queue.completed (), 1u);
}

/*
 * The property free_domain () rests on: once close () returns, nothing the
 * channel's work was holding is still in use, so the state it compiled into can
 * be destroyed.
 */
TEST (CompileQueue, ClosingAChannelWaitsForWorkInFlight)
{
	CompileQueue queue;
	CompileQueue::Channel channel (&queue);

	Gate gate;
	std::atomic<bool> entered {false};
	std::atomic<bool> finished {false};

	ASSERT_TRUE (channel.enqueue (nullptr, [&] {
		entered.store (true);
		gate.wait ();
		finished.store (true);
	}));

	wait_for ([&] { return entered.load (); }, "the worker never took the work");
	EXPECT_FALSE (finished.load ());

	std::thread opener ([&] {
		std::this_thread::sleep_for (50ms);
		gate.open ();
	});

	channel.close ();
	EXPECT_TRUE (finished.load ()) << "close () returned while work was running";
	opener.join ();
}

TEST (CompileQueue, ClosingAChannelDropsWhatIsQueued)
{
	CompileQueue queue;
	CompileQueue::Channel channel (&queue);

	Gate gate;
	std::atomic<int> ran {0};
	std::atomic<bool> entered {false};

	ASSERT_TRUE (channel.enqueue (nullptr, [&] {
		entered.store (true);
		gate.wait ();
	}));
	wait_for ([&] { return entered.load (); }, "the worker never took the work");

	for (int i = 0; i < 4; i++)
		ASSERT_TRUE (channel.enqueue (nullptr, [&] { ran++; }));

	gate.open ();
	channel.close ();
	EXPECT_EQ (ran.load (), 0) << "a queued compile ran after its channel closed";
}

TEST (CompileQueue, AClosedChannelTakesNoMoreWork)
{
	CompileQueue queue;
	CompileQueue::Channel channel (&queue);

	channel.close ();

	std::atomic<bool> ran {false};

	EXPECT_FALSE (channel.enqueue (nullptr, [&] { ran.store (true); }));
	queue.drain ();
	EXPECT_FALSE (ran.load ());
}

/*
 * One domain unloading must not wait behind another domain's compile. The
 * worker here is parked in a channel that is not the one being closed, and
 * closing has to return anyway.
 */
TEST (CompileQueue, ClosingOneChannelIgnoresAnother)
{
	CompileQueue queue;
	CompileQueue::Channel busy (&queue);
	CompileQueue::Channel idle (&queue);

	Gate gate;
	std::atomic<bool> entered {false};

	ASSERT_TRUE (busy.enqueue (nullptr, [&] {
		entered.store (true);
		gate.wait ();
	}));
	wait_for ([&] { return entered.load (); }, "the worker never took the work");

	idle.close ();

	gate.open ();
	busy.close ();
}

/*
 * What free_method () needs. A dynamic method's MonoMethod goes straight back
 * to the allocator, so a compile still holding one has to be finished with -
 * and a later compile under the same address is a different method and has to
 * be taken.
 */
TEST (CompileQueue, DroppingATagWaitsForItAndLeavesTheRestAlone)
{
	CompileQueue queue;
	CompileQueue::Channel channel (&queue);

	int doomed = 0;
	int other = 0;

	Gate gate;
	std::atomic<bool> entered {false};
	std::atomic<bool> finished {false};
	std::atomic<int> others {0};

	ASSERT_TRUE (channel.enqueue (&doomed, [&] {
		entered.store (true);
		gate.wait ();
		finished.store (true);
	}));
	wait_for ([&] { return entered.load (); }, "the worker never took the work");

	ASSERT_TRUE (channel.enqueue (&doomed, [&] { FAIL () << "dropped work ran"; }));
	ASSERT_TRUE (channel.enqueue (&other, [&] { others++; }));

	std::thread opener ([&] {
		std::this_thread::sleep_for (50ms);
		gate.open ();
	});

	queue.drop (&doomed);
	EXPECT_TRUE (finished.load ()) << "drop () returned while the work was running";
	opener.join ();

	/* The channel is still open, and the tag can be used again. */
	ASSERT_TRUE (channel.enqueue (&doomed, [&] { others++; }));
	queue.drain ();
	EXPECT_EQ (others.load (), 2);
}

TEST (CompileQueue, AStoppedQueueTakesNothing)
{
	CompileQueue queue;
	CompileQueue::Channel channel (&queue);

	queue.stop ();

	std::atomic<bool> ran {false};

	EXPECT_FALSE (channel.enqueue (nullptr, [&] { ran.store (true); }));
	EXPECT_FALSE (ran.load ());
}

/*
 * The other side of the same rule. Every drain here waits for the worker, so
 * the worker reaching one waits for itself - and it would do so silently, on a
 * thread nothing is watching, with whoever eventually notices somewhere else
 * entirely. So it dies instead, and it has to die in a release build too: this
 * is the configuration everything is tested and shipped in, and an assert ()
 * would be compiled out of it.
 *
 * The queue is built inside the statement so that the fork happens before there
 * is a worker thread to fork away from.
 */
TEST (CompileQueueDeathTest, DrainingFromTheWorkerDiesRatherThanHanging)
{
	GTEST_FLAG_SET (death_test_style, "threadsafe");

	EXPECT_DEATH (
		{
			CompileQueue queue;
			CompileQueue::Channel channel (&queue);

			channel.enqueue (nullptr, [&] { channel.close (); });
			queue.drain ();
		},
		"closing a channel from the compile worker waits for itself");
}

TEST (CompileQueueDeathTest, StoppingFromTheWorkerDiesRatherThanHanging)
{
	GTEST_FLAG_SET (death_test_style, "threadsafe");

	EXPECT_DEATH (
		{
			CompileQueue queue;
			CompileQueue::Channel channel (&queue);

			channel.enqueue (nullptr, [&] { queue.stop (); });
			queue.drain ();
		},
		"stopping the queue from the compile worker waits for itself");
}

/// A worker whose start () does what the runtime's does once shutdown has
/// begun: never come back.
class ParkedWorker : public CompileQueue::Worker {
public:
	Gate release;
	std::atomic<bool> entered {false};
	std::atomic<bool> done {false};

	bool start () override
	{
		entered.store (true);
		release.wait ();
		return true;
	}

	void stop () override { done.store (true); }
};

/*
 * mono_thread_internal_attach () parks the caller for the life of the process
 * when the runtime is shutting down, and that call is inside Worker::start ().
 * A worker still in there has taken no work, so stop () has nothing to wait for
 * and must not.
 */
TEST (CompileQueue, StoppingDoesNotWaitForAWorkerStillStarting)
{
	auto worker = std::make_unique<ParkedWorker> ();
	ParkedWorker *parked = worker.get ();
	CompileQueue queue (std::move (worker));

	{
		CompileQueue::Channel channel (&queue);

		ASSERT_TRUE (channel.enqueue (nullptr, [] {}));
		wait_for ([&] { return parked->entered.load (); },
		          "the worker never reached its start hook");
	}

	/* The point: this returns rather than joining a thread that is not coming. */
	queue.stop ();

	/* Let it out and see it off, so that nothing outlives the queue. */
	parked->release.open ();
	wait_for ([&] { return parked->done.load (); }, "the worker never finished");
}

/// A worker that refuses the thread it is given.
class RefusingWorker : public CompileQueue::Worker {
public:
	std::atomic<bool> asked {false};

	bool start () override
	{
		asked.store (true);
		return false;
	}
};

TEST (CompileQueue, AWorkerThatRefusesItsThreadRunsNothing)
{
	auto worker = std::make_unique<RefusingWorker> ();
	RefusingWorker *refusing = worker.get ();

	CompileQueue queue (std::move (worker));
	CompileQueue::Channel channel (&queue);

	std::atomic<bool> ran {false};

	ASSERT_TRUE (channel.enqueue (nullptr, [&] { ran.store (true); }));
	wait_for ([&] { return refusing->asked.load (); }, "the worker never started");

	queue.stop ();
	EXPECT_FALSE (ran.load ());
	EXPECT_EQ (queue.completed (), 0u);
}

/// A CompileWorker the fixture can see the hooks of.
class CountingWorker : public CompileWorker {
public:
	std::atomic<int> starts {0};
	std::atomic<int> idles {0};

	bool start () override
	{
		bool ok = CompileWorker::start ();
		starts++;
		return ok;
	}

	void idle (llvm::function_ref<void ()> wake) override
	{
		idles++;
		CompileWorker::idle (wake);
	}
};

/*
 * The one this whole design turns on.
 *
 * Backend::resolve () lays classes out, which takes mono's loader lock. That
 * lock is recursive per thread, so a compile on the thread already holding it
 * is recursion; on a worker it is a different thread and it blocks. The cycle
 * that would close it needs somebody waiting on the compile from the other
 * side, and there is nobody: enqueue () returns immediately and the thread
 * holding the lock carries on to release it.
 *
 * If this ever hangs, that waiter has been added back.
 */
TEST (CompileQueue, TheLoaderLockIsWaitedForByTheWorkerAndNobodyElse)
{
	MONO_SKIP_WITHOUT_CORPUS ();
	init_runtime ();

	MonoImage *image = load_image ("objects");
	ERROR_DECL (lookup);
	MonoClass *klass = mono_class_from_name_checked (image, "", "Counter", lookup);

	ASSERT_NE (klass, nullptr) << mono_error_get_message (lookup);
	mono_error_cleanup (lookup);

	auto worker = std::make_unique<CountingWorker> ();
	CountingWorker *hooks = worker.get ();
	CompileQueue queue (std::move (worker));
	CompileQueue::Channel channel (&queue);

	std::atomic<bool> entered {false};
	std::atomic<bool> laid_out {false};

	mono_loader_lock ();

	ASSERT_TRUE (channel.enqueue (klass, [&] {
		entered.store (true);

		ERROR_DECL (error);
		MonoVTable *vtable =
			mono_class_vtable_checked (mono_get_root_domain (), klass, error);

		EXPECT_NE (vtable, nullptr);
		mono_error_cleanup (error);
		laid_out.store (true);
	}));

	wait_for ([&] { return entered.load (); }, "the worker never took the work");

	/*
	 * A class this fresh has no vtable, so laying it out goes through
	 * mono_class_create_runtime_vtable () and stops on the lock held here. A
	 * failure on this line means it no longer does, and the rest of the test
	 * is then proving nothing.
	 */
	std::this_thread::sleep_for (250ms);
	EXPECT_FALSE (laid_out.load ())
		<< "laying a class out did not want the loader lock";

	/* The point: this thread got here rather than waiting on the compile. */
	mono_loader_unlock ();

	queue.drain ();
	EXPECT_TRUE (laid_out.load ());

	EXPECT_EQ (hooks->starts.load (), 1);
	EXPECT_GE (hooks->idles.load (), 1);

	/* Detach the worker from the runtime while it is still up. */
	queue.stop ();
}

} // namespace
} // namespace test
} // namespace mono

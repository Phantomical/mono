#include "compile-queue.hpp"

#include <llvm/Support/ErrorHandling.h>
#include <llvm/ADT/Twine.h>

#include <algorithm>
#include <utility>

namespace mono {

void
CompileQueue::self_wait (const char *what)
{
	llvm::report_fatal_error (llvm::Twine (what)
	                          + " from the compile worker waits for itself");
}

CompileQueue::CompileQueue (WorkerFactory factory, unsigned workers)
	: factory_ (std::move (factory)), limit_ (std::max (workers, 1u))
{
}

CompileQueue::CompileQueue (std::unique_ptr<Worker> worker)
	: CompileQueue ([held = std::move (worker)] () mutable { return std::move (held); },
	                1)
{
}

CompileQueue::~CompileQueue ()
{
	stop ();
}

bool
CompileQueue::enqueue (Channel &channel, void *tag, Work work)
{
	std::lock_guard<std::mutex> lock (mutex_);

	if (!channel.open_ || stopping_)
		return false;

	pending_.push_back (Item { &channel, tag, next_id_++, std::move (work) });
	ensure_worker ();
	ready_.notify_one ();
	return true;
}

void
CompileQueue::close (Channel &channel)
{
	std::unique_lock<std::mutex> lock (mutex_);

	not_from_the_worker ("closing a channel");

	/*
	 * Closed before the queue is swept, so that a thread racing to enqueue
	 * either got in before the sweep - and is dropped by it - or is refused.
	 * There is no third outcome where an item lands behind the sweep.
	 */
	channel.open_ = false;

	Channel *closing = &channel;

	pending_.erase (std::remove_if (pending_.begin (), pending_.end (),
	                                [closing] (const Item &item) {
		                                return item.channel == closing;
	                                }),
	                pending_.end ());

	retired_.wait (lock, [this, closing] {
		return none_running (
			[closing] (const Ticket &ticket) { return ticket.channel == closing; });
	});
}

void
CompileQueue::drop (void *tag)
{
	std::unique_lock<std::mutex> lock (mutex_);

	not_from_the_worker ("dropping a tag");

	pending_.erase (std::remove_if (pending_.begin (), pending_.end (),
	                                [tag] (const Item &item) {
		                                return item.tag == tag;
	                                }),
	                pending_.end ());

	retired_.wait (lock, [this, tag] {
		return none_running (
			[tag] (const Ticket &ticket) { return ticket.tag == tag; });
	});
}

void
CompileQueue::drain ()
{
	std::unique_lock<std::mutex> lock (mutex_);

	not_from_the_worker ("draining the queue");

	retired_.wait (lock,
	               [this] { return pending_.empty () && running_.empty (); });
}

void
CompileQueue::stop ()
{
	std::vector<std::thread> joining;

	{
		std::lock_guard<std::mutex> lock (mutex_);

		stopping_ = true;
		pending_.clear ();
		ready_.notify_all ();

		std::thread::id self = std::this_thread::get_id ();

		/*
		 * Only the std::thread comes out. Thread's own entry stays where it
		 * is: a detached worker still names its own by index once it is out
		 * of Worker::start (), and stopping_ is already set above, so
		 * nothing grows threads_ behind it.
		 */
		for (Thread &worker : threads_) {
			/* Already joined or detached by an earlier stop (). Both this and
			 * the destructor call one, so the second finds these. */
			if (!worker.thread.joinable ())
				continue;

			/*
			 * A worker without started set is still inside Worker::start ():
			 * it has taken no work and can take none, so there is nothing to
			 * wait for even when it is the caller. One that reached the loop
			 * and calls this is a compile tearing down the runtime under
			 * itself.
			 */
			if (!worker.started) {
				worker.thread.detach ();
				continue;
			}

			if (worker.thread.get_id () == self)
				self_wait ("stopping the queue");

			joining.push_back (std::move (worker.thread));
		}
	}

	for (std::thread &thread : joining)
		thread.join ();
}

uint64_t
CompileQueue::completed () const
{
	std::lock_guard<std::mutex> lock (mutex_);

	return completed_;
}

unsigned
CompileQueue::workers () const
{
	std::lock_guard<std::mutex> lock (mutex_);

	return (unsigned) threads_.size ();
}

void
CompileQueue::ensure_worker ()
{
	if (threads_.size () >= limit_)
		return;

	/*
	 * Everything parked is going to take an item, so a thread is worth adding
	 * only for what is queued behind them. That grows the pool one thread per
	 * enqueue, and only under work the pool is not keeping up with. A program
	 * compiling a method at a time never gets past the first.
	 */
	if (pending_.size () <= idle_)
		return;

	size_t index = threads_.size ();

	threads_.push_back (Thread {});
	threads_[index].worker = factory_ ? factory_ () : nullptr;
	threads_[index].thread = std::thread ([this, index] { run (index); });
}

void
CompileQueue::run (size_t index)
{
	Worker *worker;

	{
		std::unique_lock<std::mutex> lock (mutex_);

		/* Nothing to attach for, and stop () is no longer waiting for us. */
		if (stopping_)
			return;

		worker = threads_[index].worker.get ();
	}

	if (worker != nullptr && !worker->start ())
		return;

	std::unique_lock<std::mutex> lock (mutex_);

	threads_[index].started = true;

	auto wait = [&] {
		ready_.wait (lock, [this] { return stopping_ || !pending_.empty (); });
	};

	for (;;) {
		idle_++;
		if (worker != nullptr)
			worker->idle (wait);
		else
			wait ();
		idle_--;

		/*
		 * Stopping wins over what is left queued. Whoever asked for the stop
		 * is on their way out and the work was for them. A queued compile
		 * that never runs costs nothing, since nothing is waiting for one.
		 */
		if (stopping_)
			break;

		Item item = std::move (pending_.front ());
		pending_.pop_front ();
		running_.push_back (Ticket { item.channel, item.tag, item.id });

		/*
		 * No lock across the work itself: it takes the backend's own locks,
		 * the loader lock and whatever else a compile needs, and a drain
		 * running concurrently has to be able to see this ticket while it
		 * does.
		 */
		lock.unlock ();
		item.work ();
		lock.lock ();

		uint64_t id = item.id;

		running_.erase (std::remove_if (running_.begin (), running_.end (),
		                                [id] (const Ticket &ticket) {
			                                return ticket.id == id;
		                                }),
		                running_.end ());
		completed_++;
		retired_.notify_all ();
	}

	lock.unlock ();

	if (worker != nullptr)
		worker->stop ();
}

} // namespace mono

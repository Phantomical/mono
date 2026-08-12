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

CompileQueue::CompileQueue (std::unique_ptr<Worker> worker)
	: worker_ (std::move (worker))
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

	ensure_worker ();
	pending_.push_back (Item { &channel, tag, next_id_++, std::move (work) });
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
	std::thread worker;

	{
		std::lock_guard<std::mutex> lock (mutex_);

		stopping_ = true;
		pending_.clear ();
		ready_.notify_all ();

		/*
		 * A worker without started_ set is still inside Worker::start (): it has
		 * taken no work and can take none, so there is nothing to wait for even
		 * when it is the caller. One that reached the loop and calls this is a
		 * compile tearing down the runtime under itself.
		 */
		if (started_) {
			not_from_the_worker ("stopping the queue");
			worker = std::move (thread_);
		} else if (thread_.joinable ()) {
			thread_.detach ();
		}
	}

	if (worker.joinable ())
		worker.join ();
}

uint64_t
CompileQueue::completed () const
{
	std::lock_guard<std::mutex> lock (mutex_);

	return completed_;
}

void
CompileQueue::ensure_worker ()
{
	if (thread_.joinable ())
		return;

	thread_ = std::thread ([this] { run (); });
}

void
CompileQueue::run ()
{
	{
		std::unique_lock<std::mutex> lock (mutex_);

		/* Nothing to attach for, and stop () is no longer waiting for us. */
		if (stopping_)
			return;
	}

	if (worker_ != nullptr && !worker_->start ())
		return;

	std::unique_lock<std::mutex> lock (mutex_);

	started_ = true;

	for (;;) {
		if (worker_ != nullptr)
			worker_->idle ([&] {
				ready_.wait (lock, [this] {
					return stopping_ || !pending_.empty ();
				});
			});
		else
			ready_.wait (lock,
			             [this] { return stopping_ || !pending_.empty (); });

		/*
		 * Stopping wins over what is left queued. Whoever asked for the stop
		 * is on their way out and the work was for them; a queued compile
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

	if (worker_ != nullptr)
		worker_->stop ();
}

} // namespace mono

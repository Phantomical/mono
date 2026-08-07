/**
 * \file
 * \brief Compiles that run on a thread of their own.
 *
 * Deliberately knows nothing about mono - no metadata, no MonoMethod, no
 * runtime headers - so the ordering it has to get right can be driven straight
 * from a unit test. The runtime integration layers on top, through Worker.
 */

#ifndef MONO_LLVM_COMPILE_QUEUE_HPP
#define MONO_LLVM_COMPILE_QUEUE_HPP

#include <llvm/ADT/FunctionExtras.h>
#include <llvm/ADT/STLFunctionalExtras.h>

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace mono {

/// A queue of compiles nobody waits for, and the thread that runs them.
///
/// The rule this whole machine exists to keep is that **no thread may ever wait
/// on a background compile**. A compile reaches mono_class_vtable_checked (),
/// which takes mono's loader lock; that lock is recursive per thread, so a
/// compile running on the thread that already holds it is recursion rather than
/// deadlock. Move the compile to a worker and thread identity changes: a
/// managed thread holding the loader lock and waiting for the worker deadlocks
/// against the worker waiting for the lock. The cycle needs a waiter, so there
/// is none - enqueue () hands the work over and returns, and nothing here
/// offers a way to ask what became of it. The natural "compile this one
/// synchronously, it is urgent" optimisation would put the waiter back.
///
/// The two waits that do exist - Channel::close () and drop () - run the other
/// way round: they are how a thread tearing something down proves the work
/// using it has finished, before the thing it was using is destroyed. Both must
/// be called with no lock the work could want, the loader lock included.
class CompileQueue {
public:
	using Work = llvm::unique_function<void ()>;

	/// The worker thread's life as the thing that owns the queue sees it.
	///
	/// The queue knows nothing about mono, so this is where the runtime
	/// attaches the thread to the GC and lets go of it again while it is idle.
	/// The default does neither, which is all the queue's own tests need.
	class Worker {
	public:
		virtual ~Worker () = default;

		/// On the worker thread, before it takes any work.
		virtual void start () {}
		/// On the worker thread, once it has taken its last.
		virtual void stop () {}

		/// Wait, by calling WAKE, until there is something to do.
		///
		/// The worker spends its whole idle life in here, which is why it is a
		/// hook at all: a thread parked in a condition variable reaches no
		/// safepoint, so a runtime that suspends threads cooperatively has to
		/// hand this one over for the duration or its next GC waits forever.
		virtual void idle (llvm::function_ref<void ()> wake) { wake (); }
	};

	/// One enqueuer's share of the queue, and the handle its teardown drains
	/// through.
	///
	/// A channel belongs to the thing the work is for - a domain's compilation
	/// state - and closing it is what makes destroying that thing safe: after
	/// close () nothing queued for it will run, nothing running for it still
	/// is, and nothing new is taken. That is the whole ownership story, and it
	/// is why a queued item can hold a raw pointer to the state it compiles
	/// into: the state cannot be destroyed until its channel has been closed,
	/// and the channel cannot be closed while work is in flight.
	class Channel {
	public:
		explicit Channel (CompileQueue *queue) : queue_ (queue) {}
		~Channel () { close (); }

		Channel (const Channel &) = delete;
		Channel &operator= (const Channel &) = delete;

		/// Run WORK on the worker thread. TAG says what the work is for, so
		/// that drop () can find it again; it is compared and never
		/// dereferenced.
		///
		/// Returns false when the channel is closed or the queue has been
		/// stopped, and WORK is then dropped without ever running. Nothing
		/// retries - a compile that is refused simply does not happen, which
		/// is the safe direction for every caller here.
		bool enqueue (void *tag, Work work)
		{
			return queue_->enqueue (*this, tag, std::move (work));
		}

		/// Refuse further work, drop what is queued for this channel, and wait
		/// for what is already running for it. Idempotent.
		void close () { queue_->close (*this); }

	private:
		friend class CompileQueue;

		CompileQueue *queue_;
		/// Guarded by the queue's mutex.
		bool open_ = true;
	};

	/// A queue whose worker runs WORKER's hooks around every compile. The
	/// thread itself is not started until something is enqueued, so a process
	/// that never compiles in the background never has one.
	explicit CompileQueue (std::unique_ptr<Worker> worker = nullptr);
	~CompileQueue ();

	CompileQueue (const CompileQueue &) = delete;
	CompileQueue &operator= (const CompileQueue &) = delete;

	/// Drop everything queued under TAG, whichever channel it came from, and
	/// wait for anything running under it.
	///
	/// Unlike closing a channel this does not refuse TAG in future: the tags
	/// here are freed and reused - a dynamic method's MonoMethod goes straight
	/// back to the allocator - so a later enqueue under the same address is a
	/// different piece of work and has to be taken.
	void drop (void *tag);

	/// Wait for the queue to empty and for the worker to be idle. For tests
	/// and for shutdown; ordinary teardown drains through a channel.
	void drain ();

	/// Stop the worker. Anything queued is dropped, anything running finishes,
	/// and nothing is taken afterwards.
	void stop ();

	/// How many pieces of work have run to completion.
	uint64_t completed () const;

private:
	/// One queued compile.
	struct Item {
		Channel *channel;
		void *tag;
		uint64_t id;
		Work work;
	};

	/// A piece of work the worker has taken and not yet finished. What close ()
	/// and drop () wait on.
	struct Ticket {
		Channel *channel;
		void *tag;
		uint64_t id;
	};

	bool enqueue (Channel &channel, void *tag, Work work);
	void close (Channel &channel);

	/// Die if this is the worker thread, naming WHAT it was asked to wait for.
	///
	/// Every wait below is a wait for the worker, so the worker reaching one
	/// waits for itself. It is a hang rather than a crash, on a thread nothing
	/// is watching, and the caller that eventually notices is somewhere else
	/// entirely - so this is worth a loud death even in a release build.
	/// Called with MUTEX_ held.
	[[noreturn]] static void self_wait (const char *what);
	void not_from_the_worker (const char *what) const
	{
		if (std::this_thread::get_id () == thread_.get_id ())
			self_wait (what);
	}

	/// The worker thread's whole life.
	void run ();

	/// Start the worker if it is not running yet. Called with MUTEX_ held; the
	/// new thread's first act is to take that same lock, so it waits for the
	/// caller to let go.
	void ensure_worker ();

	/// Whether any ticket satisfies PREDICATE. Called with MUTEX_ held.
	template <typename Predicate>
	bool none_running (Predicate predicate) const
	{
		for (const Ticket &ticket : running_)
			if (predicate (ticket))
				return false;
		return true;
	}

	mutable std::mutex mutex_;
	/// Signalled when work arrives or the queue is stopping.
	std::condition_variable ready_;
	/// Signalled when a piece of work finishes, which is what the two drains
	/// and drain () itself are waiting for.
	std::condition_variable retired_;

	std::deque<Item> pending_;
	std::vector<Ticket> running_;

	std::unique_ptr<Worker> worker_;
	std::thread thread_;
	bool stopping_ = false;

	uint64_t next_id_ = 0;
	uint64_t completed_ = 0;
};

} // namespace mono

#endif

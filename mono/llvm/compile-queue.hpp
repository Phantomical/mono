/**
 * \file
 * \brief Compiles that run on threads of their own.
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

/// A queue of compiles nobody waits for, and the threads that run them.
///
/// The rule this whole machine exists to keep is that **no thread may ever wait
/// on a background compile**. A compile reaches mono_class_vtable_checked (),
/// which takes mono's loader lock. That lock is recursive per thread, so a
/// compile running on the thread that already holds it is recursion rather than
/// deadlock. Move the compile to a worker and thread identity changes: a
/// managed thread holding the loader lock and waiting for a worker deadlocks
/// against that worker waiting for the lock. The cycle needs a waiter, so the
/// queue has none - enqueue () hands the work over and returns, and the queue
/// offers no way to ask what became of it. The natural "compile this one
/// synchronously, it is urgent" optimisation would put the waiter back.
///
/// The two waits that do exist - Channel::close () and drop () - run the other
/// way round. They are how a thread tearing something down proves the work
/// using it has finished, before the thing it was using is destroyed. Call both
/// with no lock the work can want, the loader lock included.
///
/// Several workers take from the one queue, so work is unordered: two items run
/// at the same time whether or not they came from the same channel, and an item
/// queued later can finish first. A caller may rely on what the waits above give
/// it - that a closed channel and a dropped tag have nothing left running.
class CompileQueue {
public:
	using Work = llvm::unique_function<void ()>;

	/// Hooks the queue's owner installs around the worker thread's life.
	///
	/// The queue knows nothing about mono, so this is where the runtime
	/// attaches the thread to the GC and lets go of it again while it is idle.
	/// The default does neither, which is all the queue's own tests need.
	///
	/// Each worker thread gets one of these to itself, so an implementation
	/// keeps its thread's state in it and needs no locking of its own. What it
	/// shares with the other workers it has to guard.
	class Worker {
	public:
		virtual ~Worker () = default;

		/// On the worker thread, before it takes any work. Answering false
		/// means this thread can run none, and it exits without taking the
		/// queue's lock or calling stop ().
		virtual bool start () { return true; }
		/// On the worker thread, once it has taken its last.
		virtual void stop () {}

		/// Wait, by calling wake (), until there is something to do.
		///
		/// The worker spends its whole idle life in here, which is why it is a
		/// hook at all. A thread parked in a condition variable reaches no
		/// safepoint, so a runtime that suspends threads cooperatively has to
		/// hand this one over for the duration, or its next GC waits forever.
		virtual void idle (llvm::function_ref<void ()> wake) { wake (); }
	};

	/// One enqueuer's share of the queue, and the handle its teardown drains
	/// through.
	///
	/// A channel belongs to the thing the work is for - a domain's compilation
	/// state - and closing it is what makes destroying that thing safe. After
	/// close () nothing queued for it will run, nothing running for it still
	/// is, and nothing new is taken. That is the whole ownership story, and it
	/// is why a queued item can hold a raw pointer to the state it compiles
	/// into. The state cannot be destroyed until its channel has been closed,
	/// and the channel cannot be closed while work is in flight.
	class Channel {
	public:
		explicit Channel (CompileQueue *queue) : queue_ (queue) {}
		~Channel () { close (); }

		Channel (const Channel &) = delete;
		Channel &operator= (const Channel &) = delete;

		/// Run work on a worker thread, tagged with tag so drop () can find
		/// it again. The tag is only compared, never dereferenced.
		///
		/// Returns false when the channel is closed or the queue has been
		/// stopped, and work is then dropped without ever running. Nothing
		/// retries - a compile that is refused does not happen, which is the
		/// safe direction for every caller here.
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

	/// Builds the Worker a new thread runs its compiles under. Called with the
	/// queue's lock held, on whichever thread asked for the work that wants
	/// another thread, and may return null for a thread with no hooks.
	using WorkerFactory = llvm::unique_function<std::unique_ptr<Worker> ()>;

	/// Builds a queue of at most workers threads, each running the hooks of a
	/// Worker that factory built for it.
	///
	/// Threads start one at a time and only while work is waiting that no parked
	/// thread is going to take, so a process that never compiles in the
	/// background has none and one that compiles a method at a time keeps one.
	CompileQueue (WorkerFactory factory, unsigned workers);

	/// Builds a queue of a single thread, running worker's hooks.
	explicit CompileQueue (std::unique_ptr<Worker> worker = nullptr);
	~CompileQueue ();

	CompileQueue (const CompileQueue &) = delete;
	CompileQueue &operator= (const CompileQueue &) = delete;

	/// Drop everything queued under tag, whichever channel it came from, and
	/// wait for anything running under it.
	///
	/// Unlike closing a channel, this does not refuse tag in future. Tags are
	/// freed and reused - a dynamic method's MonoMethod goes straight back to
	/// the allocator - so a later enqueue under the same address is a
	/// different piece of work and has to be taken.
	void drop (void *tag);

	/// Wait for the queue to empty and for every worker to be idle. For tests
	/// and for shutdown; ordinary teardown drains through a channel.
	void drain ();

	/// Stop the workers. Anything queued is dropped, anything running finishes,
	/// and nothing is taken afterwards.
	///
	/// A worker that has not got through Worker::start () yet is left where it
	/// is rather than waited for: it has taken no work, and start () is
	/// entitled never to return.
	void stop ();

	/// How many pieces of work have run to completion.
	uint64_t completed () const;

	/// How many worker threads have been started. For tests and for reporting;
	/// it counts the threads that exist rather than the ones doing anything.
	unsigned workers () const;

private:
	/// One queued compile.
	struct Item {
		Channel *channel;
		void *tag;
		uint64_t id;
		Work work;
	};

	/// A piece of work a worker has taken and not yet finished. What close ()
	/// and drop () wait on.
	struct Ticket {
		Channel *channel;
		void *tag;
		uint64_t id;
	};

	/// One worker thread and the hooks it runs under.
	///
	/// An entry lives until the queue does, so a thread can name its own by an
	/// index that stays good however many others start after it. stop () takes
	/// the std::thread out to join it and leaves the rest where it is.
	struct Thread {
		std::unique_ptr<Worker> worker;
		std::thread thread;
		/// Whether this one got through Worker::start () and reached the loop.
		/// Until it has, it is not a thread stop () may wait for.
		bool started = false;
	};

	bool enqueue (Channel &channel, void *tag, Work work);
	void close (Channel &channel);

	/// Die if this is a worker thread, naming what it was asked to wait for.
	///
	/// Every wait below is a wait for the workers, so a worker reaching one
	/// waits for itself. It is a hang rather than a crash, on a thread nothing
	/// is watching, and the caller that eventually notices is somewhere else
	/// entirely - so this is worth a loud death even in a release build.
	/// Called with the queue's mutex held.
	[[noreturn]] static void self_wait (const char *what);
	void not_from_the_worker (const char *what) const
	{
		std::thread::id self = std::this_thread::get_id ();

		for (const Thread &worker : threads_)
			if (worker.thread.get_id () == self)
				self_wait (what);
	}

	/// A worker thread's whole life. index names its own entry in threads_.
	void run (size_t index);

	/// Start one more worker if there is work no parked one will take and the
	/// limit allows it. Called with the queue's mutex held. The new thread's
	/// first act is to take that same lock, so it waits for the caller to let
	/// go.
	void ensure_worker ();

	/// Whether no running ticket satisfies predicate. Called with the
	/// queue's mutex held.
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

	WorkerFactory factory_;
	unsigned limit_;
	std::vector<Thread> threads_;
	/// How many workers are parked waiting for something to do. Compared
	/// against what is queued to decide whether another thread would have
	/// anything to run.
	size_t idle_ = 0;
	bool stopping_ = false;

	uint64_t next_id_ = 0;
	uint64_t completed_ = 0;
};

} // namespace mono

#endif

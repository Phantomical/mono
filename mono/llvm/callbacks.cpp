/**
 * \file
 * \brief Reserving and reclaiming compiler re-entry trampolines.
 */

#include "callbacks.hpp"

#include "arch/arch.hpp"

#include <llvm/ExecutionEngine/Orc/IndirectionUtils.h>

#include <utility>

using namespace llvm;
using namespace llvm::orc;

namespace mono {

Expected<std::unique_ptr<LazyCallbacks>>
LazyCallbacks::create (void *on_error)
{
	std::unique_ptr<LazyCallbacks> self (new LazyCallbacks (on_error));
	LazyCallbacks *raw = self.get ();

	auto pool = LocalTrampolinePool<arch::LazyEntryABI>::Create (
		[raw] (ExecutorAddr trampoline,
		       TrampolinePool::NotifyLandingResolvedFunction resolved) {
			resolved (ExecutorAddr::fromPtr (raw->fire (trampoline)));
		});
	if (!pool)
		return pool.takeError ();

	self->pool_ = std::move (*pool);
	return std::move (self);
}

LazyCallbacks::~LazyCallbacks () = default;

Expected<void *>
LazyCallbacks::reserve (LazyCompile compile)
{
	Expected<ExecutorAddr> trampoline = pool_->getTrampoline ();

	if (!trampoline)
		return trampoline.takeError ();

	auto callback = std::make_shared<Callback> ();
	callback->compile = std::move (compile);

	std::lock_guard<std::mutex> lock (mutex_);
	callbacks_[*trampoline] = std::move (callback);
	return trampoline->toPtr<void *> ();
}

void
LazyCallbacks::release (void *trampoline)
{
	if (trampoline == nullptr)
		return;

	ExecutorAddr addr = ExecutorAddr::fromPtr (trampoline);
	std::lock_guard<std::mutex> lock (mutex_);

	if (callbacks_.erase (addr))
		pool_->releaseTrampoline (addr);
}

void *
LazyCallbacks::fire (ExecutorAddr trampoline)
{
	std::shared_ptr<Callback> callback;
	{
		std::lock_guard<std::mutex> lock (mutex_);
		callback = callbacks_.lookup (trampoline);
	}

	if (callback == nullptr)
		return on_error_;

	if (void *landed = callback->landing.load (std::memory_order_acquire))
		return landed;

	/*
	 * No lock of this backend's is held across the compile. That is the rule
	 * here rather than a tuning choice, and two things need it.
	 *
	 * The compile re-enters the backend and publishes the callees it names, so
	 * it reserves trampolines of its own and meets the map's lock again.
	 *
	 * A compile also takes the runtime's locks: the domain lock, and the loader
	 * lock for a corlib class that an emitted null check names. A thread that
	 * already holds one of those can enter a lazy stub and arrive here. A
	 * backend lock held across compile () closes that cycle, and the process
	 * stops. The order is therefore loader and domain outside, this map's lock
	 * and the engine's lock inside, and neither inner lock crosses a compile.
	 *
	 * The cost is that threads which arrive together each compile the method.
	 * They agree on one answer below, and a body that loses is superseded
	 * rather than freed - which is what the engine already does for a method it
	 * compiles a second time.
	 */
	void *landed = callback->compile ();
	void *first = nullptr;

	if (callback->landing.compare_exchange_strong (first, landed,
	                                               std::memory_order_release,
	                                               std::memory_order_acquire))
		return landed;

	return first;
}

} // namespace mono

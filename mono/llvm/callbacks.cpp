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

	/*
	 * Outside the map's lock: the compile re-enters the backend and publishes
	 * the callees it names, so it reserves trampolines of its own while this
	 * one is still running.
	 */
	std::lock_guard<std::mutex> lock (callback->mutex);

	if (callback->landing == nullptr) {
		callback->landing = callback->compile ();
		/* Nothing will run it again; it is holding a translated module. */
		callback->compile = LazyCompile ();
	}

	return callback->landing;
}

} // namespace mono

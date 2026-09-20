/**
 * \file
 * \brief Reserving and reclaiming compiler re-entry trampolines.
 */

#include "callbacks.hpp"

#include "arch/arch.hpp"

#include "mono/utils/mono-hwcap.h"

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

	/*
	 * The resolver passes the frame it spilled the call on rather than the
	 * trampoline ORC would pass, so the first argument is not the trampoline
	 * id its name says. fire () reads the trampoline back out of the frame.
	 */
	auto pool = LocalTrampolinePool<arch::LazyEntryABI>::Create (
		[raw] (ExecutorAddr frame,
		       TrampolinePool::NotifyLandingResolvedFunction resolved) {
			resolved (ExecutorAddr::fromPtr (
				raw->fire (frame.toPtr<const arch::LazyEntryFrame *> ())));
		});
	if (!pool)
		return pool.takeError ();

	self->pool_ = std::move (*pool);
	self->resolver_ = arch::take_published_resolver ();

	/* The AVX resolver executes VMOVUPS before entering managed code. */
	if (mono_hwcap_x86_has_avx) {
		auto avx_pool = LocalTrampolinePool<arch::LazyEntryAvxABI>::Create (
			[raw] (ExecutorAddr frame,
			       TrampolinePool::NotifyLandingResolvedFunction resolved) {
				resolved (ExecutorAddr::fromPtr (
					raw->fire (frame.toPtr<const arch::LazyEntryFrame *> ())));
			});
		if (!avx_pool)
			return avx_pool.takeError ();

		self->pool_avx_ = std::move (*avx_pool);
		self->resolver_avx_ = arch::take_published_resolver ();
	}

	return std::move (self);
}

/*
 * Unregister each resolver while its pool still owns the backing mapping.
 * Otherwise, the host unwinder can retain stale unwind data after ORC releases
 * the mapping and reuses its address.
 */
LazyCallbacks::~LazyCallbacks ()
{
	arch::unregister_resolver_unwind_info (resolver_);
	arch::unregister_resolver_unwind_info (resolver_avx_);
}

void
LazyCallbacks::foreach_resolver (void (*visit) (const void *, uint32_t, const char *, void *),
                                 void *user_data) const
{
	if (resolver_)
		visit (resolver_.toPtr<const void *> (), arch::LazyEntryABI::ResolverCodeSize,
		      arch::LazyEntryABI::DisplayName, user_data);
	if (resolver_avx_)
		visit (resolver_avx_.toPtr<const void *> (), arch::LazyEntryAvxABI::ResolverCodeSize,
		      arch::LazyEntryAvxABI::DisplayName, user_data);
}

TrampolinePool &
LazyCallbacks::pool_for (ResolverKind kind)
{
	if (kind == ResolverKind::Avx && pool_avx_)
		return *pool_avx_;
	return *pool_;
}

Expected<void *>
LazyCallbacks::reserve (LazyCompile compile, ResolverKind kind)
{
	Expected<ExecutorAddr> trampoline = pool_for (kind).getTrampoline ();

	if (!trampoline)
		return trampoline.takeError ();

	auto callback = std::make_shared<Callback> ();
	callback->compile = std::move (compile);
	callback->kind = kind;

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
	ResolverKind kind;

	{
		std::lock_guard<std::mutex> lock (mutex_);
		auto it = callbacks_.find (addr);

		if (it == callbacks_.end ())
			return;

		kind = it->second->kind;
		callbacks_.erase (it);
	}

	pool_for (kind).releaseTrampoline (addr);
}

void
LazyCallbacks::rearm (void *trampoline)
{
	if (trampoline == nullptr)
		return;

	std::shared_ptr<Callback> callback;
	{
		std::lock_guard<std::mutex> lock (mutex_);
		callback = callbacks_.lookup (ExecutorAddr::fromPtr (trampoline));
	}

	if (callback == nullptr)
		return;

	std::lock_guard<std::mutex> held (callback->latch);

	callback->epoch.fetch_add (1, std::memory_order_relaxed);
	callback->landing.store (nullptr, std::memory_order_release);
}

void *
LazyCallbacks::fire (const arch::LazyEntryFrame *frame)
{
	ExecutorAddr trampoline = ExecutorAddr::fromPtr (arch::trampoline_of (frame));
	std::shared_ptr<Callback> callback;
	{
		std::lock_guard<std::mutex> lock (mutex_);
		callback = callbacks_.lookup (trampoline);
	}

	if (callback == nullptr)
		return on_error_;

	for (;;) {
		if (void *landed = callback->landing.load (std::memory_order_acquire))
			return landed;

		uint32_t at = callback->epoch.load (std::memory_order_acquire);

		/*
		 * No lock of this backend's is held across the compile. That is the
		 * rule here rather than a tuning choice, and two things need it.
		 *
		 * The compile re-enters the backend and publishes the callees it
		 * names, so it reserves trampolines of its own and meets the map's
		 * lock again.
		 *
		 * A compile also takes the runtime's locks: the domain lock, and the
		 * loader lock for a corlib class that an emitted null check names. A
		 * thread that already holds one of those can enter a lazy stub and
		 * arrive here. A backend lock held across compile () closes that
		 * cycle, and the process stops. The order is therefore loader and
		 * domain outside, this map's lock and the engine's lock inside, and
		 * neither inner lock crosses a compile.
		 *
		 * The cost is that threads which arrive together each compile the
		 * method. They agree on one answer below, and a body that loses is
		 * superseded rather than freed - which is what the engine already does
		 * for a method it compiles a second time.
		 */
		void *landed = callback->compile ();
		void *first = nullptr;

		std::lock_guard<std::mutex> held (callback->latch);

		// A re-arm while this compiled took down the code it built. Compile
		// again rather than continue into it.
		if (callback->epoch.load (std::memory_order_relaxed) != at)
			continue;

		if (callback->landing.compare_exchange_strong (first, landed,
		                                               std::memory_order_release,
		                                               std::memory_order_acquire))
			return landed;

		return first;
	}
}

} // namespace mono

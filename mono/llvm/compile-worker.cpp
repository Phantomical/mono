#include "compile-worker.hpp"

#include "runtime/options.hpp"

#include "config.h"

#include <glib.h>

#include <llvm/Support/raw_ostream.h>

#include "mono/metadata/appdomain.h"
#include "mono/metadata/object-internals.h"
#include "mono/metadata/threads.h"
#include "mono/metadata/threads-types.h"
#include "mono/utils/mono-threads-api.h"

namespace mono {

bool
CompileWorker::start ()
{
	/*
	 * Attaching once shutdown has begun does not fail, it parks the thread for
	 * the life of the process - so ask first rather than find out. Nothing is
	 * lost by refusing: a compile queued this late is for a runtime that is on
	 * its way out, and nothing waits for one.
	 */
	if (mono_runtime_is_shutting_down ())
		return false;

	mono_thread_internal_attach (mono_get_root_domain ());
	attached_ = true;

	MonoInternalThread *internal = mono_thread_internal_current ();

	mono_thread_set_name_constant_ignore_error (internal, "LLVM compiler",
	                                            MonoSetThreadNameFlag_Permanent);

	/*
	 * Background so that shutdown does not wait for a thread that only ever
	 * exits when it is told to, and unmanaged so that nothing tries to abort
	 * or suspend it on its own account - Environment.Exit () suspends every
	 * other thread, and a compile stopped halfway leaves the linker holding a
	 * half-linked object.
	 *
	 * The attach above publishes the thread, so shutdown can already have
	 * taken a list of threads to wait for with this one on it. Only
	 * mono_thread_set_state () fires the notification that tells shutdown to
	 * build that list again. Assigning the bit instead leaves shutdown
	 * waiting on a thread that exits once the queue stops, which is after
	 * that wait. Flags first, so the rebuilt list sees both.
	 */
	internal->flags |= MONO_THREAD_FLAG_DONT_MANAGE;
	mono_thread_set_state (internal, ThreadState_Background);

	/* The queue adds threads under load and retires them when they go quiet.
	 * So how many threads a run has is not the setting, and no other output
	 * reports it. */
	if (is_jit_trace_enabled ())
		MONO_LOCK (jit_trace_mutex ())
		{
			llvm::errs () << "[llvm-jit] compile worker attached\n";
		}

	return true;
}

void
CompileWorker::stop ()
{
	if (!attached_)
		return;

	/*
	 * Ask for the thread object again rather than keep the one the attach
	 * returned. That object is managed, and this thread spends most of its
	 * life in GC Safe mode, so a collection can move it out from under a
	 * pointer held here. The stale read gives a garbage internal thread and
	 * the detach faults on it.
	 */
	mono_thread_internal_detach (mono_thread_current ());
	attached_ = false;
}

void
CompileWorker::abandon ()
{
	if (!attached_)
		return;

	/*
	 * The queue does not wait for this thread, and mono_thread_current ()
	 * reads the domain's special static fields, which mono_domain_free () can
	 * already have cleared. An exit instead of a park runs mono's own
	 * thread-exit cleanup out of a TLS destructor, which takes the GC lock and
	 * is no safer. An attach to a runtime that is shutting down parks a thread
	 * the same way, one frame inside start ().
	 */
	for (;;)
		mono_thread_info_sleep (10000, nullptr);
}

void
CompileWorker::idle (llvm::function_ref<void ()> wake)
{
	MONO_ENTER_GC_SAFE;
	wake ();
	MONO_EXIT_GC_SAFE;
}

} // namespace mono

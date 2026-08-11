#include "compile-worker.hpp"

#include "config.h"

#include <glib.h>

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
	 */
	internal->state |= ThreadState_Background;
	internal->flags |= MONO_THREAD_FLAG_DONT_MANAGE;

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
CompileWorker::idle (llvm::function_ref<void ()> wake)
{
	MONO_ENTER_GC_SAFE;
	wake ();
	MONO_EXIT_GC_SAFE;
}

} // namespace mono

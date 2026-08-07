#include "compile-worker.hpp"

#include "config.h"

#include <glib.h>

#include "mono/metadata/object-internals.h"
#include "mono/metadata/threads.h"
#include "mono/metadata/threads-types.h"
#include "mono/utils/mono-threads-api.h"

namespace mono {

void
CompileWorker::start ()
{
	thread_ = mono_thread_internal_attach (mono_get_root_domain ());

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
}

void
CompileWorker::stop ()
{
	if (thread_ == nullptr)
		return;

	mono_thread_internal_detach (thread_);
	thread_ = nullptr;
}

void
CompileWorker::idle (llvm::function_ref<void ()> wake)
{
	MONO_ENTER_GC_SAFE;
	wake ();
	MONO_EXIT_GC_SAFE;
}

} // namespace mono

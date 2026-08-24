#ifndef MONO_LLVM_COMPILE_WORKER_HPP
#define MONO_LLVM_COMPILE_WORKER_HPP

#include "compile-queue.hpp"

namespace mono {

/// A compile queue's worker as the runtime needs it. It attaches the thread to
/// the runtime, hands it back to the GC while it has nothing to do, and detaches
/// it again when the queue retires the thread.
///
/// A compile allocates - mono_ldstr_checked () interns a managed string, laying
/// a class out allocates its statics - so the thread has to be one the collector
/// knows about. It stays in GC Unsafe mode while it compiles, exactly as a
/// managed thread compiling synchronously does, and drops to GC Safe while it
/// waits. Under a cooperative or hybrid suspend policy a thread parked in a
/// condition variable reaches no safepoint, so a collection that tries to
/// suspend it there can never start. A preemptive policy is the default here,
/// and it transitions nothing and signals the parked thread instead.
///
/// It is attached to the root domain and never to the domain it compiles for,
/// which is what keeps it off mono_threads_abort_appdomain_threads ()'s list
/// when that domain unloads. Waiting for its work is the compile queue's
/// business, not the thread abort machinery's.
class CompileWorker : public CompileQueue::Worker {
public:
	bool start () override;
	void stop () override;
	void idle (llvm::function_ref<void ()> wake) override;

private:
	bool attached_ = false;
};

} // namespace mono

#endif

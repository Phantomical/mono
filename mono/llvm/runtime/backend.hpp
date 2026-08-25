#ifndef MONO_LLV_RUNTIME_BACKEND_HPP
#define MONO_LLV_RUNTIME_BACKEND_HPP

#include "compile-queue.hpp"
#include "compile-worker.hpp"
#include "domain-method.hpp"
#include "options.hpp"
#include "method-symbols.hpp"
#include "translate.hpp"
#include <llvm/ADT/ArrayRef.h>
#include <llvm/ADT/DenseMap.h>
#include <llvm/ADT/DenseSet.h>
#include <llvm/ADT/StringRef.h>
#include <llvm/Support/Error.h>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

namespace llvm {
class Module;
}

typedef struct _MonoDomain MonoDomain;
typedef struct _MonoJitInfo MonoJitInfo;

namespace mono {

class MonoBackend {
private:
	static MonoBackend *instance;

	/// Registers the teardown that runs at exit, installing it only on the
	/// first call.
	///
	/// Call this only from a thread that has just compiled a method.
	static void register_exit_teardown ();

	using Compiled = mono::Compiled;
	struct DomainState;
	struct MethodState;

public:
	static llvm::Expected<MonoBackend *> get ();

	/// Gives \p dm its stubs, standing up the engine and the domain's state if
	/// this is the first thing to ask for either.
	static llvm::Error attach (MonoDomainMethod &dm);

	static llvm::Error attach_interop (MonoDomainMethod &dm);

	static void stop_compilation (MonoDomain *domain);

	static void release_domain (MonoDomain *domain);

	/// Calling this while a thread is still executing the method can free the
	/// memory that thread is running in.
	static void release_method (MonoMethod *method);

	static void stop_compilation ();

	static void *body_of (MonoDomain *domain, MonoMethod *method);

	static bool request_promotion (MonoMethod *method, MonoDomain *domain, MonoTier tier);

	static bool promote_now (MonoMethod *method, MonoDomain *domain, MonoTier tier);

	/// Makes the next call through \p trampoline run its compile again, on the
	/// terms LazyCallbacks::rearm () states.
	static void rearm_trampoline (MonoDomain *domain, void *trampoline);

	/// Returns the profile counters \p dm's tier-1 body counts into.
	///
	/// std::nullopt covers a method the backend never compiled and one
	/// compiled with the instrumentation off. A method that was instrumented
	/// but has not run yet returns its counters, and they read zero - that
	/// lets a caller tell "no profile" from "never executed".
	///
	/// Returns a copy, because a recompile can replace what the record holds.
	/// The counter array the copy points at stays readable for as long as the
	/// domain does.
	static std::optional<ProfileCounters> profile_of (MonoDomainMethod &dm);

	static void foreach_body (MonoDomain *domain, MonoMethod *method,
	                          void (*visit) (MonoJitInfo *, void *), void *user_data);

	static void *unbox_entry_of (MonoMethod *method);

	/// Returns the body \p method runs in the calling thread's domain,
	/// compiling it if the domain has none.
	///
	/// A dispatcher stub calls this, so its address is registered as a JIT
	/// symbol and as the mono_llvm_jit_body_for_current_domain icall.
	static void *body_for_current_domain (MonoMethod *method);

public:
	llvm::Expected<void *> compile (MonoMethod *method, MonoDomain *domain);

	/// Returns the address that stands for \p method, publishing the method if
	/// the domain has not seen it, and without compiling a body for it.
	///
	/// The address for a method native code enters is its C entry, and that one
	/// is compiled here rather than on a later call: an address handed to
	/// native code is called without the runtime being asked again.
	llvm::Expected<void *> stub_for (MonoMethod *method, MonoDomain *domain);

	/// The address that stands for dm's method: the entry native code is
	/// handed, or the thunk in front of the body. Call it with no lock on dm
	/// held.
	static llvm::Expected<void *> published_entry (MonoDomainMethod &dm);

private:
	MonoBackend () = default;
	MonoBackend (const MonoBackend &) = delete;
	MonoBackend &operator= (const MonoBackend &) = delete;

	~MonoBackend ();

	llvm::Expected<DomainState *> state ();
	llvm::Expected<DomainState *> state (MonoDomain *domain);

	static llvm::Expected<MonoDomainMethod *> publish (DomainState &domain,
	                                                   MonoMethod *method);

	llvm::Error attach_entry (DomainState &domain, MonoDomainMethod &dm);

	/// Decides which engine runs \p dm and returns the address that engine is
	/// entered at. This is what the method's thunk is published pointing at.
	///
	/// It takes no decision that has to wait for another thread. A method it
	/// sends to a compiled tier gets the compile entry below, so every thread
	/// that blocks for a body blocks there.
	void *policy_entry (DomainState &domain, MonoDomainMethod &dm);

	/// Compiles \p dm on the calling thread and returns where the body landed.
	///
	/// The one place a thread waits for a method's code. A method that cannot
	/// be compiled gets a body that raises, so this always returns somewhere
	/// the caller can be sent.
	void *compile_entry (DomainState &domain, MonoDomainMethod &dm);

	/// Points \p dm's entry at the interpreter and returns it.
	///
	/// Fails when the interpreter does not run the method, which is the
	/// caller's signal to compile it. A failure leaves the entry untouched.
	llvm::Expected<Compiled> tier0_entry (DomainState &domain, MonoDomainMethod &dm);

	llvm::Expected<void *> dispatcher (DomainState &domain, MonoDomainMethod &dm);

	/// Returns where \p dm's body has ended up, compiling the method if it
	/// has not been compiled yet. This is what the stub in front of it is
	/// redirected to on the first call through it.
	///
	/// With allow_tier0 false, this returns a compiled body. The interpreter
	/// is not offered the method, and any interpreter entry it already has
	/// is compiled over. Pass false if you cannot set the register the
	/// interpreter reads its method from.
	///
	/// Compiles the method again when one it folded in was replaced while it
	/// compiled.
	llvm::Expected<void *> entry_point (DomainState &domain, MonoDomainMethod &dm,
	                                    bool allow_tier0 = true);

	/// Points \p dm's entry at the body \p shared compiles to, compiling that
	/// body if this domain has not yet.
	///
	/// Fails with a SharingRefusal when the shared body cannot be built, and
	/// with that body's own failure when it fails verification. \p dm is left
	/// untouched either way, which is the caller's signal to compile it
	/// against its own instantiation. A refusal decides for the life of the
	/// domain which body this instantiation runs, so a detour on the shared
	/// form misses it from then on.
	///
	/// Threads that want one shared body at once build it once, the losers
	/// waiting for the winner. That wait is bounded, so a loser can end up
	/// building the body beside the winner; see claim_shared_body ().
	llvm::Expected<Compiled> enter_shared_body (DomainState &domain, MonoDomainMethod &dm,
	                                            MonoMethod *shared, MonoTier tier);

	/// Whether what enter_shared_body () returned is that method's own answer.
	///
	/// A SharingRefusal is an answer about the shared form rather than about the
	/// method, so it is the one outcome a caller has to take somewhere else.
	/// Every other outcome, a failure included, stands for the method itself.
	///
	/// Marks \p result checked either way, so a caller may take its error.
	static bool answered_by_sharing (llvm::Expected<Compiled> &result);

	/// What a thread that asked to build a shared body found.
	enum class SharedClaim {
		/// This thread holds the claim and must build the body.
		held,
		/// The thread that held the claim released it. The record shows
		/// what it left, which can be nothing.
		done,
		/// Another thread has held the claim for longer than a compile is
		/// given. This thread builds the body beside it.
		expired,
	};

	/// Takes the claim to build \p owner's shared body.
	///
	/// A claim this returns as held must be given back with
	/// release_shared_body (). A thread that finds the claim taken waits for
	/// it and comes back with the claim, with done, or with expired.
	///
	/// The wait is bounded because a thread that arrives here can hold a
	/// runtime lock the holder's compile goes on to want. Such a pair makes
	/// progress by the waiter giving up rather than by either of them noticing.
	SharedClaim claim_shared_body (MonoDomainMethod *owner);

	/// Gives back the claim \p owner is built under and wakes what waits for
	/// it.
	void release_shared_body (MonoDomainMethod *owner);

	/// Returns the stub \p dm's entry is published as when its shared body
	/// has no receiver to read a context out of. It writes this
	/// instantiation's context and enters \p target.
	///
	/// Carved once per record and kept, so \p target has to be an address that
	/// stays right - the shared method's thunk, not the body behind it.
	llvm::Expected<void *> context_stub (DomainState &domain, MonoDomainMethod &dm,
	                                     void *target);

	/// Gives \p dm a body at \p tier and points its stub at it, whether or
	/// not it already has one.
	///
	/// With allow_tier0 the interpreter is offered the method first.
	/// Promotion passes false, which is what makes it a compile rather than
	/// a second trip through the tier the method is already running at. A
	/// method whose entry has been taken off a compiled body is compiled
	/// whatever this says: its tier-0 call counter is spent.
	///
	/// With for_sharing, \p dm is the record of a shared method and a failure
	/// comes back as a SharingRefusal rather than as a body that raises it.
	llvm::Expected<Compiled> compile_body (DomainState &domain, MonoDomainMethod &dm,
	                                       bool allow_tier0, MonoTier tier,
	                                       bool for_sharing = false);

	/// Compiles several methods together, the way compile_body () compiles
	/// one, sharing a single compile between them.
	///
	/// Every method in \p dms is compiled at the one tier given. The results
	/// line up with \p dms. A member that a shared body can serve leaves the
	/// batch, and its result comes from that body instead. The interpreter is
	/// offered none of them - compile_body () decides that before it gets
	/// here, and promotion has already settled it.
	std::vector<llvm::Expected<Compiled>>
	compile_bodies (DomainState &domain, llvm::ArrayRef<MonoDomainMethod *> dms,
	                MonoTier tier, bool for_sharing = false);

	/// Returns this engine's own state for \p dm, which it attached when the
	/// record was built.
	static MethodState &engine_state (MonoDomainMethod &dm);

	void release_domain_impl (MonoDomain *domain);
	void release_method_impl (MonoMethod *method);

private:
	std::mutex mutex_;

	/**
	 * Ahead of domains_, because each domain holds a channel into this and
	 * closing one - which is what destroying it does - reaches back in here.
	 *
	 * With a CompileWorker each, which is what attaches a worker thread to the
	 * GC. A thread that allocates or reads metadata without being attached is a
	 * thread the collector does not know to suspend.
	 */
	CompileQueue queue_ { [] { return std::make_unique<CompileWorker> (); },
		              compile_worker_count (),
		              compile_worker_idle_timeout () };
	llvm::DenseMap<MonoDomain *, std::unique_ptr<DomainState>> domains_;

	/*
	 * The shared bodies a thread is compiling right now. Several instantiations
	 * share one form, so two threads compiling two of them reach the same
	 * record - and the second must not compile it again while the first is
	 * still publishing it. Guarded by mutex_, and held only around the set
	 * itself rather than across the compile.
	 */
	llvm::DenseSet<MonoDomainMethod *> sharing_;
	/// Signalled when a record leaves sharing_. It covers every record, so a
	/// waiter reads the set again to find out whether the wake was its own.
	std::condition_variable shared_claims_;
};

} // namespace mono

#endif

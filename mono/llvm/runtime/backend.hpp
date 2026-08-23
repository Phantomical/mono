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

public:
	llvm::Expected<void *> compile (MonoMethod *method, MonoDomain *domain);

	/// A method native code enters has an address of its own, handed back by
	/// compile () instead: this function is never asked for one.
	llvm::Expected<void *> stub_for (MonoMethod *method, MonoDomain *domain);

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

	llvm::Expected<Compiled> interp_entries (DomainState &domain, MonoDomainMethod &dm);

	llvm::Expected<void *> dispatcher (DomainState &domain, MonoDomainMethod &dm);

	static void *body_for_current_domain (MonoMethod *method);

	/// Returns where \p dm's body has ended up, compiling the method if it
	/// has not been compiled yet. This is what the stub in front of it is
	/// redirected to on the first call through it.
	///
	/// With allow_tier0 false, this returns a compiled body. The interpreter
	/// is not offered the method, and any interpreter entry it already has
	/// is compiled over. Pass false if you cannot set the register the
	/// interpreter reads its method from.
	llvm::Expected<void *> entry_point (DomainState &domain, MonoDomainMethod &dm,
	                                    bool allow_tier0 = true);

	/// Points \p dm's entry at the body \p shared compiles to, compiling that
	/// body if this domain has not yet.
	///
	/// Fails with a SharingRefusal unless the shared body fails verification,
	/// which comes back as that failure instead. The SharingRefusal cases:
	/// another thread already holds the claim to compile the shared body,
	/// this thread's own compile of it fails, or that compile succeeds and
	/// the record still shows no body right after. \p dm is then left
	/// untouched on a SharingRefusal, which is the caller's signal to compile
	/// it against its own instantiation.
	llvm::Expected<Compiled> enter_shared_body (DomainState &domain, MonoDomainMethod &dm,
	                                            MonoMethod *shared, MonoTier tier);

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
	/// a second trip through the tier the method is already running at.
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
		              compile_worker_count () };
	llvm::DenseMap<MonoDomain *, std::unique_ptr<DomainState>> domains_;

	/*
	 * The shared bodies a thread is compiling right now. Several instantiations
	 * share one form, so two threads compiling two of them reach the same
	 * record - and the second must not compile it again while the first is
	 * still publishing it. Guarded by mutex_, and held only around the set
	 * itself rather than across the compile.
	 */
	llvm::DenseSet<MonoDomainMethod *> sharing_;
};

} // namespace mono

#endif

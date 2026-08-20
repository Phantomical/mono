#ifndef MONO_LLV_RUNTIME_BACKEND_HPP
#define MONO_LLV_RUNTIME_BACKEND_HPP

#include "compile-queue.hpp"
#include "compile-worker.hpp"
#include "domain-method.hpp"
#include "method-symbols.hpp"
#include "translate.hpp"
#include <llvm/ADT/ArrayRef.h>
#include <llvm/ADT/DenseMap.h>
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

	using Compiled = mono::Compiled;
	struct DomainState;
	struct MethodState;

public:
	static llvm::Expected<MonoBackend *> get ();

	/// Gives \p dm its stubs, standing up the engine and the domain's state if
	/// this is the first thing to ask for either.
	static llvm::Error attach (MonoDomainMethod &dm);

	/// Gives \p dm the C-convention entry native code enters it through, or
	/// leaves it with none when nothing native enters the method.
	static llvm::Error attach_interop (MonoDomainMethod &dm);

	/// Stop all compilation for a specific domain. Blocks until any in-progress
	/// work is completed.
	static void stop_compilation (MonoDomain *domain);

	/// Release all backend data associated with a MonoDomain.
	static void release_domain (MonoDomain *domain);

	/// Release everything associated with a MonoMethod.
	///
	/// Calling this while the method is still in use will lead to UB.
	static void release_method (MonoMethod *method);

	/// Stop all background compilation. Blocks until any in-progress work is
	/// completed.
	///
	/// Note that this will prevent any compilations from running again.
	static void stop_compilation ();

	/// Where METHOD's body starts in DOMAIN, or null when this engine has not
	/// compiled it there.
	static void *body_of (MonoDomain *domain, MonoMethod *method);

	/// Ask for METHOD to be compiled in DOMAIN at TIER, replacing the tier
	/// running it.
	///
	/// Returns as soon as the work is queued, never once it is done. Answering
	/// false means the work was refused and nothing retries it: a domain on its
	/// way out takes nothing new, and neither does an engine that does not exist
	/// yet. A caller that counts calls towards a promotion has to count again.
	static bool request_promotion (MonoMethod *method, MonoDomain *domain, MonoTier tier);

	/// Compile METHOD at TIER in DOMAIN on the calling thread, and point its
	/// entry at the result before returning.
	///
	/// Answers false when the compile was refused or failed, and the method is
	/// then left at whatever tier already ran it.
	static bool promote_now (MonoMethod *method, MonoDomain *domain, MonoTier tier);

	/// The profile counters \p dm's tier-1 body counts into.
	///
	/// Absent covers a method the backend never compiled and one compiled with
	/// the instrumentation off. A method that was instrumented and has not run
	/// yet answers its counters, which read zero - so a caller that has to tell
	/// "no profile" from "never executed" gets the two apart.
	///
	/// Answers a copy, because a recompile can replace what the record holds.
	/// The counter array the copy points at stays readable for as long as the
	/// domain does.
	static std::optional<ProfileCounters> profile_of (MonoDomainMethod &dm);

	/// Call VISIT with the jit info of each live body this engine compiled
	/// METHOD into in DOMAIN, oldest first.
	static void foreach_body (MonoDomain *domain, MonoMethod *method,
	                          void (*visit) (MonoJitInfo *, void *), void *user_data);

	/// Where to enter METHOD when the receiver is still boxed, or null when this
	/// engine generated no such entry for it.
	static void *unbox_entry_of (MonoMethod *method);

public:
	/// Compile a method within a domain. Returns an address which you can use
	/// to call the method, or an error otherwise.
	llvm::Expected<void *> compile (MonoMethod *method, MonoDomain *domain);

	/// The address METHOD is called at in DOMAIN, without compiling it.
	///
	/// The body behind the stub is compiled by the first call that arrives.
	/// A method native code enters has an address of its own that compile ()
	/// hands back instead; nothing that asks here holds such a method.
	llvm::Expected<void *> stub_for (MonoMethod *method, MonoDomain *domain);

private:
	MonoBackend () = default;
	MonoBackend (const MonoBackend &) = delete;
	MonoBackend &operator= (const MonoBackend &) = delete;

	~MonoBackend ();

	/// Get the DomainState for a given domain, or the current one otherwise.
	llvm::Expected<DomainState *> state ();
	llvm::Expected<DomainState *> state (MonoDomain *domain);

	/// The record for \p method in \p domain, publishing its stubs if this is
	/// the first time anything has asked for it.
	static llvm::Expected<MonoDomainMethod *> publish (DomainState &domain,
	                                                   MonoMethod *method);

	/// Carves \p dm's stub and gives it the state behind it.
	llvm::Error attach_entry (DomainState &domain, MonoDomainMethod &dm);

	/// Point \p dm's stub at the interpreter.
	llvm::Expected<Compiled> interp_entries (DomainState &domain, MonoDomainMethod &dm);

	/// The per-call dispatcher \p dm's body stub binds to when its first
	/// caller arrived from another domain.
	llvm::Expected<void *> dispatcher (DomainState &domain, MonoDomainMethod &dm);

	/// The runtime helper behind a dispatcher: the current domain's body for
	/// METHOD, compiling it now if this domain has not yet.
	static void *body_for_current_domain (MonoMethod *method);

	/// Where \p dm's body has ended up, compiling the method if it has not been
	/// compiled yet. This is what the stub in front of it is redirected to on
	/// the first call through it.
	///
	/// With allow_tier0 false the answer is a compiled body: the interpreter is
	/// not offered the method, and an interpreter entry the method already has
	/// is compiled over. A caller that cannot set the register the interpreter's
	/// entry reads its method out of must pass false.
	llvm::Expected<void *> entry_point (DomainState &domain, MonoDomainMethod &dm,
	                                    bool allow_tier0 = true);

	/// Point \p dm's entry at the body \p shared compiles to, compiling that
	/// body if this domain has not yet.
	///
	/// Fails with a SharingRefusal when the translator will not share the
	/// method, which leaves \p dm untouched and is the caller's signal to
	/// compile it against its own instantiation.
	llvm::Expected<Compiled> enter_shared_body (DomainState &domain, MonoDomainMethod &dm,
	                                            MonoMethod *shared, MonoTier tier);

	/// The stub \p dm's entry is published as when its shared body has no
	/// receiver to read a context out of: it writes this instantiation's
	/// context and enters \p target.
	///
	/// Carved once per record and kept, so \p target has to be an address that
	/// stays right - the shared method's thunk, not the body behind it.
	llvm::Expected<void *> context_stub (DomainState &domain, MonoDomainMethod &dm,
	                                     void *target);

	/// Give METHOD a body at TIER and point its stub at it, whether or not it
	/// already has one.
	///
	/// With allow_tier0 the interpreter is offered the method first; promotion
	/// passes false, which is what makes it a compile rather than a second trip
	/// through the tier the method is already running at.
	///
	/// With for_sharing, \p dm is the record of a shared method and a failure
	/// comes back as a SharingRefusal rather than as a body that raises it.
	llvm::Expected<Compiled> compile_body (DomainState &domain, MonoDomainMethod &dm,
	                                       bool allow_tier0, MonoTier tier,
	                                       bool for_sharing = false);

	/// The same for several methods at once, sharing one compile between them.
	///
	/// The results line up with dms. Neither the interpreter nor a shared body
	/// is offered any of them - compile_body () decides both before it gets
	/// here, and promotion has already settled them.
	std::vector<llvm::Expected<Compiled>>
	compile_bodies (DomainState &domain, llvm::ArrayRef<MonoDomainMethod *> dms,
	                MonoTier tier, bool for_sharing = false);

	/// This engine's own state for \p dm, which it attached when the record was
	/// built.
	static MethodState &engine_state (MonoDomainMethod &dm);

	void release_domain_impl (MonoDomain *domain);
	void release_method_impl (MonoMethod *method);

private:
	std::mutex mutex_;

	/*
	 * Ahead of domains_, because each domain holds a channel into this and
	 * closing one - which is what destroying it does - reaches back in here.
	 */
	/*
	 * With a CompileWorker, which is what attaches the worker thread to the GC.
	 * A thread that allocates or reads metadata without being attached is a
	 * thread the collector does not know to suspend.
	 */
	CompileQueue queue_ { std::make_unique<CompileWorker> () };
	llvm::DenseMap<MonoDomain *, std::unique_ptr<DomainState>> domains_;
};

} // namespace mono

#endif

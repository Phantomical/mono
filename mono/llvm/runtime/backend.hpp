#ifndef MONO_LLV_RUNTIME_BACKEND_HPP
#define MONO_LLV_RUNTIME_BACKEND_HPP

#include "compile-queue.hpp"
#include "compile-worker.hpp"
#include "domain-method.hpp"
#include "method-symbols.hpp"
#include "translate.hpp"
#include <llvm/ADT/DenseMap.h>
#include <llvm/ADT/StringRef.h>
#include <llvm/Support/Error.h>
#include <memory>
#include <mutex>

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

	/// Gives \p dm the entry a call off a value type's vtable arrives at, or
	/// leaves it with none when the method has no such entry.
	static llvm::Error attach_unbox (MonoDomainMethod &dm);

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

	/// Ask for METHOD to be compiled in DOMAIN, replacing the tier running it.
	///
	/// Returns as soon as the work is queued, never once it is done. Answering
	/// false means the work was refused and nothing retries it: a domain on its
	/// way out takes nothing new, and neither does an engine that does not exist
	/// yet. A caller that counts calls towards a promotion has to count again.
	static bool request_promotion (MonoMethod *method, MonoDomain *domain);

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

	/// Give every declaration in M that names a method the symbol this backend
	/// publishes that method's entry under, publishing the method if it has not
	/// been already.
	llvm::Error bind_externals (DomainState &domain, llvm::Module &m);

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
	llvm::Expected<void *> entry_point (DomainState &domain, MonoDomainMethod &dm);

	/// Give METHOD a body and point its stub at it, whether or not it already
	/// has one.
	///
	/// With allow_tier0 the interpreter is offered the method first; promotion
	/// passes false, which is what makes it a compile rather than a second trip
	/// through the tier the method is already running at.
	llvm::Expected<Compiled> compile_body (DomainState &domain, MonoDomainMethod &dm,
	                                       bool allow_tier0);

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

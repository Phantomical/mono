#ifndef MONO_LLV_RUNTIME_BACKEND_HPP
#define MONO_LLV_RUNTIME_BACKEND_HPP

#include "compile-queue.hpp"
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
	struct Publication;
	struct DomainState;
	struct MethodState;

public:
	static llvm::Expected<MonoBackend *> get ();

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

private:
	MonoBackend () = default;
	MonoBackend (const MonoBackend &) = delete;
	MonoBackend &operator= (const MonoBackend &) = delete;

	~MonoBackend ();

	/// Get the DomainState for a given domain, or the current one otherwise.
	llvm::Expected<DomainState *> state ();
	llvm::Expected<DomainState *> state (MonoDomain *domain);

	/// The state for METHOD in DOMAIN, publishing its stubs if this is the first
	/// time anything has asked for it.
	llvm::Expected<MethodState *> publish (DomainState &domain, MonoMethod *method);

	/// Give every declaration in M that names a method the symbol this backend
	/// publishes that method's entry under, publishing the method if it has not
	/// been already.
	llvm::Error bind_externals (DomainState &domain, llvm::Module &m);

	/// Where ENTRY of METHOD has ended up, compiling the method if it has not
	/// been compiled yet. This is what the stub in front of that entry is
	/// redirected to on the first call through it.
	llvm::Expected<void *> entry_point (DomainState &domain, MethodState &method,
	                                    Entry entry);

	void release_domain_impl (MonoDomain *domain);
	void release_method_impl (MonoMethod *method);

private:
	std::mutex mutex_;

	/*
	 * Ahead of domains_, because each domain holds a channel into this and
	 * closing one - which is what destroying it does - reaches back in here.
	 */
	CompileQueue queue_;
	llvm::DenseMap<MonoDomain *, std::unique_ptr<DomainState>> domains_;
};

} // namespace mono

#endif

#include "backend.hpp"
#include "callbacks.hpp"
#include "codemem.hpp"
#include "compile-queue.hpp"
#include "jit.hpp"
#include "method-to-llvm.hpp"
#include "engine.hpp"
#include "naming.hpp"
#include "options.hpp"
#include <llvm/ADT/StringRef.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/Module.h>
#include <memory>
#include <mutex>
#include <unistd.h>
#include "stubs.hpp"
#include "util/lock.hpp"
#include "builtins.hpp"
#include "translate.hpp"
#include <optional>
#include "mono/metadata/appdomain.h"

namespace mono {

namespace {

/*
 * Where a stub lands when the compile behind it failed. The trampoline has
 * already put the call's arguments back and jumped here, so this is running as
 * the method the caller asked for: there is no value it could return and no
 * caller that would know what to do with one.
 */
[[noreturn]] void
lazy_compile_failed ()
{
	/*
	 * Printed and left by hand rather than through report_fatal_error, which
	 * ends in exit() when it is told not to produce a crash diagnostic. exit()
	 * runs the static destructors of every C++ library loaded into the
	 * process, LLVM's own among them, while the threads that are still
	 * compiling are using what those destructors free.
	 */
	static const char msg[] = "LLVM ERROR: a method failed to compile on first call\n";
	[[maybe_unused]] ssize_t written = write (2, msg, sizeof (msg) - 1);
	fflush (nullptr);
	_exit (1);
}

} // namespace

MonoBackend *MonoBackend::instance = nullptr;

llvm::Expected<MonoBackend *>
MonoBackend::get ()
{
	static std::once_flag once;

	std::call_once (once, [] {
		claim_engine (EngineKind::backend);
		instance = new MonoBackend ();

		atexit ([] {
			auto backend = instance;
			instance = nullptr;
			delete backend;
		});
	});

	return instance;
}

/// What publishing a method handed the rest of the runtime: the
/// address it was handed, and the trampoline jit-info record each of the
/// method's stubs was registered under. A record is only held here when it has
/// to be taken out again by hand - see register_stub_jinfo ().
struct MonoBackend::Publication {
	void *stub;
	MonoJitInfo *entry_jinfo;
	MonoJitInfo *body_jinfo;
	MonoJitInfo *unbox_jinfo;
	/// Whether the method was given an unboxing stub as well.
	bool unboxed;
};

/// What compiling a method put somewhere it has to be taken back out of.
/// Only tracked for dynamic methods; nothing else is ever freed.
struct Owned {
	std::vector<llvm::orc::JITDylib *> dylibs;
	std::vector<MonoJitInfo *> jinfos;
};

/// Everything one method owns in one domain.
///
/// Per-method state lives here and nowhere else: a method reached through a
/// different door, or running at a different tier, is still one entry in one
/// map, which is what keeps freeing it a matter of dropping this struct rather
/// than of remembering every table it was written into.
struct MonoBackend::MethodState {
	MonoMethod *method;

	/// `<printed name>@<pointer>`. Every symbol this method owns is this plus a
	/// suffix, and this is the only place the name is built - a declaration
	/// naming the method arrives with the MonoMethod on it and is renamed to
	/// what is published here.
	std::string symbol;

	/// The method itself: what generated code calls, what a vtable slot holds,
	/// and what the runtime is handed. Every method has this one.
	Stub thunk;

	/// The C-convention entry, which forwards to the one above. Only a wrapper
	/// generated for native code to enter - publishes_interop_entry () - has one,
	/// and for that method it is the address the runtime is handed instead.
	Stub c_thunk;

	/// The thunk a call off a value type's vtable comes in through. Only a
	/// method publishes_unbox_entry () selects has one.
	Stub unbox_thunk;

	/// The re-entry trampoline each stub was pointed at when it was published,
	/// held so it can be given back once nothing can reach the stub.
	void *thunk_tramp = nullptr;
	void *c_thunk_tramp = nullptr;
	void *unbox_tramp = nullptr;

	/// Where this method's code ended up, once something has asked for it.
	/// Guarded by the engine's lock.
	std::optional<Compiled> code;

	MethodState (MonoMethod *method, std::string symbol)
	    : method (method), symbol (std::move (symbol))
	{
	}

	std::string name (Entry entry) const { return symbol + stub_suffix (entry).str (); }

	Stub &stub (Entry entry)
	{
		switch (entry) {
		case Entry::body:
			return thunk;
		case Entry::interop:
			return c_thunk;
		case Entry::unbox:
			return unbox_thunk;
		}
		return thunk;
	}

	void *&trampoline (Entry entry)
	{
		switch (entry) {
		case Entry::body:
			return thunk_tramp;
		case Entry::interop:
			return c_thunk_tramp;
		case Entry::unbox:
			return unbox_tramp;
		}
		return thunk_tramp;
	}

	/// The entries this method is published under, in the order they are
	/// carved.
	llvm::SmallVector<Entry, 3> entries () const
	{
		llvm::SmallVector<Entry, 3> all{Entry::body};

		if (publishes_interop_entry (method))
			all.push_back (Entry::interop);
		if (publishes_unbox_entry (method))
			all.push_back (Entry::unbox);
		return all;
	}
};

struct MonoBackend::DomainState {
	/// The actual domain we are tracking.
	MonoDomain *domain;

	/// Memory slabs that we allocate the actual functions out of.
	std::shared_ptr<CodeSlabs> slabs;

	/// The MonoJit that is used to compile methods.
	std::unique_ptr<MonoJit> jit;

	/// Allocator for thunks.
	std::unique_ptr<StubTable> stub_table;

	/// The re-entry trampolines a published-but-uncompiled stub points at.
	std::unique_ptr<LazyCallbacks> callbacks;

	CompileQueue::Channel queue;

	/*
	 * Last, so every MethodState is destroyed before the table its stubs were
	 * carved from and the linker they were defined in.
	 */
	llvm::DenseMap<MonoMethod *, std::unique_ptr<MethodState>> methods;

	DomainState (MonoDomain *domain, CompileQueue &queue) : domain (domain), queue (&queue) {}

	static llvm::Expected<std::unique_ptr<DomainState>> create (MonoDomain *domain,
	                                                            CompileQueue &queue)
	{
		auto state = std::make_unique<DomainState> (domain, queue);
		state->slabs = std::make_shared<CodeSlabs> ();
		state->stub_table = std::make_unique<StubTable> (state->slabs.get ());

		auto jit = MonoJit::create (state->slabs);
		if (!jit)
			return jit.takeError ();
		state->jit = std::move (*jit);

		auto callbacks = LazyCallbacks::create ((void *) &lazy_compile_failed);
		if (!callbacks)
			return callbacks.takeError ();
		state->callbacks = std::move (*callbacks);

		auto builtins = MonoBuiltin::get_platform_builtins (state->jit->triple ());
		for (const auto &b : builtins) {
			if (auto err = state->jit->register_symbol (b.name, b.address))
				return std::move (err);
		}

		if (is_jit_trace_enabled ()) {
			llvm::errs () << llvm::format (
				"[llvm-jit] %zu runtime builtins registered\n", builtins.size ());
		}

		return state;
	}

	/// Undefine everything METHOD was published as and hand its blocks and
	/// trampolines back.
	void retire (MethodState &method);
};

MonoBackend::~MonoBackend ()
{
	queue_.stop ();
}

/*
 * A failure here means the method being freed is still reachable - a stub the
 * linker will not give up, a block the table does not know about. There is
 * nothing to fall back to and continuing hands the next method a stub something
 * is still calling, so say what broke and stop.
 */
static void
must (llvm::Error err)
{
	if (!err)
		return;

	llvm::logAllUnhandledErrors (std::move (err), llvm::errs (), "mono: ");
	llvm::report_fatal_error ("mono: could not release a method's stubs",
	                          /*GenCrashDiag=*/false);
}

void
MonoBackend::DomainState::retire (MethodState &method)
{
	llvm::SmallVector<Entry, 3> entries = method.entries ();
	llvm::SmallVector<std::string, 3> names;

	for (Entry entry : entries)
		names.push_back (method.name (entry));

	/*
	 * By name first, then by address: nothing can find the stub through the
	 * linker any more, and only then is the block free to be carved again.
	 */
	must (jit->undefine_stubs (names));

	for (Entry entry : entries)
		callbacks->release (method.trampoline (entry));

	stub_table->remove_all (names);
}

llvm::Expected<MonoBackend::MethodState *>
MonoBackend::publish (DomainState &domain, MonoMethod *method)
{
	std::lock_guard<std::mutex> lock (mutex_);

	auto it = domain.methods.find (method);
	if (it != domain.methods.end ())
		return it->second.get ();

	auto state = std::make_unique<MethodState> (method,
	                                            stub_symbol (method, Entry::body));

	llvm::SmallVector<Entry, 3> entries = state->entries ();
	llvm::SmallVector<std::pair<llvm::StringRef, void *>, 3> defs;
	llvm::SmallVector<std::string, 3> names;
	MethodState *raw = state.get ();

	/* The definitions below point into these, so they must not move. */
	names.reserve (entries.size ());

	/*
	 * Nothing published so far is reachable, so a failure part-way has to put
	 * the pieces back rather than leave a name claimed for a method that is not
	 * in the map - the next thread to ask for the method would collide with it.
	 */
	auto unwind = [&] {
		for (const std::string &name : names)
			domain.stub_table->remove (name);
		for (Entry entry : entries)
			domain.callbacks->release (state->trampoline (entry));
	};

	for (Entry entry : entries) {
		llvm::Expected<void *> trampoline = domain.callbacks->reserve (
			[this, &domain, raw, entry] () -> void * {
				llvm::Expected<void *> code = entry_point (domain, *raw, entry);

				if (!code) {
					llvm::logAllUnhandledErrors (code.takeError (),
					                             llvm::errs (), "mono: ");
					return (void *) &lazy_compile_failed;
				}

				raw->stub (entry).redirect (*code);
				return *code;
			});
		if (!trampoline) {
			unwind ();
			return trampoline.takeError ();
		}

		names.push_back (state->name (entry));

		llvm::Expected<Stub> stub = domain.stub_table->create (names.back ());
		if (!stub) {
			names.pop_back ();
			domain.callbacks->release (*trampoline);
			unwind ();
			return stub.takeError ();
		}

		stub->redirect (*trampoline);
		state->stub (entry) = *stub;
		state->trampoline (entry) = *trampoline;
		defs.emplace_back (names.back (), stub->code ());
	}

	/*
	 * Every stub reaches the linker as soon as it is carved, whether or not
	 * anything ever names it, so that releasing one is unconditional rather
	 * than a question of whether some module happened to ask for it.
	 *
	 * Under mutex_, which is safe because defining takes the session lock and
	 * gives it back: it materializes nothing, so nothing it runs can come back
	 * here for this lock.
	 */
	if (llvm::Error err = domain.jit->define_stubs (defs)) {
		unwind ();
		return std::move (err);
	}

	domain.methods[method] = std::move (state);
	return raw;
}

llvm::Error
MonoBackend::bind_externals (DomainState &domain, llvm::Module &m)
{
	return bind_method_symbols (
		m, [&] (MonoMethod *method, Entry entry) -> llvm::Expected<std::string> {
			llvm::Expected<MethodState *> state = publish (domain, method);

			if (!state)
				return state.takeError ();
			return (*state)->name (entry);
		});
}

llvm::Expected<void *>
MonoBackend::entry_point (DomainState &domain, MethodState &method, Entry entry)
{
	MONO_LOCK (mutex_)
	{
		if (method.code)
			return method.code->at (entry);
	}

	/*
	 * Materialize as the domain the code is for, not as whatever the calling
	 * thread happens to be running as: a stub fires under the thread's current
	 * domain, and AppDomain:InvokeInDomain switches that before calling. An
	 * address baked in from the wrong domain is a pointer into another domain's
	 * vtables and statics, live until that domain unloads under it.
	 */
	DomainScope entered (domain.domain);
	MonoJitInfo *published = nullptr;

	/* Named: function_ref does not own what it points at. */
	auto publish_callee = [&] (MonoMethod *callee) -> llvm::Error {
		return publish (domain, callee).takeError ();
	};
	auto stub_address = [&] (llvm::StringRef name) -> llvm::Expected<void *> {
		if (std::optional<Stub> stub = domain.stub_table->find (name))
			return stub->code ();

		return llvm::createStringError (llvm::inconvertibleErrorCode (),
		                                "no stub is published as %s",
		                                name.str ().c_str ());
	};
	/* Nothing is freed yet, so nothing has to be remembered to free it. */
	auto note = [] (const CompiledMethod &, MonoJitInfo *) {};
	/*
	 * No stand-in body to fall back to yet, so a metadata failure is raised as
	 * itself rather than deferred to the call that would have used it.
	 */
	auto keep = [] (llvm::Error failure) -> llvm::Expected<Compiled> {
		return std::move (failure);
	};

	TranslationTarget target { domain.jit.get (), domain.domain, publish_callee,
		                   stub_address, note, keep };

	llvm::Expected<Compiled> code =
		translate_and_compile (target, method.method, &published);

	if (!code)
		return code.takeError ();

	/*
	 * Every entry, not just the one asked for: the stubs are redirected together
	 * so that whichever door a caller came in through it lands on this compile.
	 */
	for (Entry each : method.entries ())
		if (void *address = code->at (each))
			method.stub (each).redirect (address);

	MONO_LOCK (mutex_)
	{
		method.code = *code;
	}

	return code->at (entry);
}

llvm::Expected<MonoBackend::DomainState *>
MonoBackend::state ()
{
	return state (mono_domain_get ());
}

llvm::Expected<MonoBackend::DomainState *>
MonoBackend::state (MonoDomain *domain)
{
	std::lock_guard<std::mutex> lock (mutex_);

	auto it = domains_.find (domain);
	if (it != domains_.end ())
		return it->second.get ();

	auto state = DomainState::create (domain, queue_);
	if (!state)
		return state.takeError ();

	auto ptr = state->get ();
	domains_[domain] = std::move (*state);
	return ptr;
}

void
MonoBackend::stop_compilation ()
{
	if (!instance)
		return;

	instance->queue_.stop ();
}

void
MonoBackend::stop_compilation (MonoDomain *domain)
{
	if (!instance)
		return;

	CompileQueue::Channel *channel = nullptr;
	MONO_LOCK (instance->mutex_)
	{
		auto it = instance->domains_.find (domain);
		if (it == instance->domains_.end ())
			return;

		channel = &it->second->queue;
	}

	channel->close ();
}

void
MonoBackend::release_domain (MonoDomain *domain)
{
	if (!instance)
		return;

	instance->release_domain_impl (domain);
}

void
MonoBackend::release_domain_impl (MonoDomain *domain)
{
	std::unique_ptr<DomainState> state;
	MONO_LOCK (mutex_)
	{
		auto it = domains_.find (domain);
		if (it == domains_.end ())
			return;

		state = std::move (it->second);
		domains_.erase (it);
	}

	// Draining takes a while so we want to be careful to only do it outside of
	// the mutex.
	state->queue.close ();
}

void
MonoBackend::release_method (MonoMethod *method)
{
	if (!instance)
		return;

	instance->release_method_impl (method);
}

void
MonoBackend::release_method_impl (MonoMethod *method)
{
	// No need to compile this anymore. Drop it if still in the queue and
	// block on it if compilation is still in progress.
	queue_.drop (method);

	/*
	 * Every domain, not just the one that compiled it: a body reached across a
	 * domain boundary is published in the calling domain's linker too.
	 */
	llvm::SmallVector<std::pair<DomainState *, std::unique_ptr<MethodState>>, 2> going;

	MONO_LOCK (mutex_)
	{
		for (const auto &entry : domains_) {
			DomainState *state = entry.second.get ();

			auto it = state->methods.find (method);
			if (it == state->methods.end ())
				continue;

			going.emplace_back (state, std::move (it->second));
			state->methods.erase (it);
		}
	}

	// Undefining a stub waits on any link that is using it, so it happens with
	// the backend's own lock dropped.
	for (auto &[state, mstate] : going)
		state->retire (*mstate);
}

/*
 * The three reads below answer for a method this engine has compiled. Nothing it
 * publishes carries code yet - entry_point () still refuses - so for now they say
 * so rather than pretending a method has no body, which the runtime would take as
 * "not compiled here" and act on.
 */
void *
MonoBackend::body_of (MonoDomain *, MonoMethod *)
{
	return nullptr;
}

void
MonoBackend::foreach_body (MonoDomain *, MonoMethod *, void (*) (MonoJitInfo *, void *),
                           void *)
{
}

void *
MonoBackend::unbox_entry_of (MonoMethod *)
{
	return nullptr;
}

llvm::Expected<void *>
MonoBackend::compile (MonoMethod *method, MonoDomain *domain)
{
	llvm::Expected<DomainState *> state = this->state (domain);

	if (!state)
		return state.takeError ();

	llvm::Expected<MethodState *> published = publish (**state, method);

	if (!published)
		return published.takeError ();

	/* The C door where the method has one, and the method itself otherwise. */
	return (*published)
	        ->stub (publishes_interop_entry (method) ? Entry::interop : Entry::body)
	        .code ();
}

} // namespace mono

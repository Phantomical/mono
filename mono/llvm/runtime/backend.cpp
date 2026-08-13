#include "backend.hpp"
#include "callbacks.hpp"
#include "codemem.hpp"
#include "compile-queue.hpp"
#include "jit.hpp"
#include "method-to-llvm.hpp"
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
#include "dispatcher.hpp"
#include "interp.hpp"
#include "publish-events.hpp"
#include <vector>
#include "mini-runtime.h"
#include "stub-jinfo.hpp"
#include "thrower.hpp"
#include "verification.hpp"
#include "translate.hpp"
#include "arch/arch.hpp"
#include <optional>
#include "mono/metadata/appdomain.h"
#include "mono/metadata/domain-internals.h"

namespace mono {

namespace {

/// Holds a domain's lock for a scope.
class DomainLock {
public:
	explicit DomainLock (MonoDomain *domain) : domain_ (domain)
	{
		mono_domain_lock (domain_);
	}

	~DomainLock () { mono_domain_unlock (domain_); }

	DomainLock (const DomainLock &) = delete;
	DomainLock &operator= (const DomainLock &) = delete;

private:
	MonoDomain *domain_;
};

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

/// The process that built the backend, and the only one allowed to take it
/// apart again.
static pid_t owner_pid;

llvm::Expected<MonoBackend *>
MonoBackend::get ()
{
	static std::once_flag once;

	std::call_once (once, [] {
		instance = new MonoBackend ();
		owner_pid = getpid ();

		atexit ([] {
			/*
			 * Tearing the backend down closes every domain's compile channel,
			 * and closing one waits for the compiles already running on the
			 * worker thread to retire. fork () keeps only the thread that
			 * called it, so in a child that worker does not exist and a
			 * ticket left in flight at the fork never retires - the wait
			 * never ends. The crash handler forks exactly like this, so a
			 * runtime that faults while a background compile is running would
			 * otherwise leave a child parked here for good.
			 */
			if (getpid () != owner_pid)
				return;

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

	/// Whether that is the shared interpreter entry rather than a body of this
	/// method's own. Guarded by the engine's lock.
	bool interpreted = false;

	/// The jit-info record each stub was registered under, held only when it
	/// has to be taken out again by hand - see register_stub_jinfo ().
	MonoJitInfo *thunk_jinfo = nullptr;
	MonoJitInfo *c_thunk_jinfo = nullptr;
	MonoJitInfo *unbox_jinfo = nullptr;

	/// What this method's compiles put somewhere it has to be taken back out
	/// of. Only filled for a dynamic method; nothing else is ever freed.
	Owned owned;

	/// The per-call dispatcher this method's body stub got instead of a direct
	/// binding, when its first caller arrived from another domain.
	void *dispatch = nullptr;

	/*
	 * Bodies this method had before the current one, in publication order. The
	 * stubs no longer name them, but a thread already running in one still is,
	 * so anything that has to cover every body a method is executing in - the
	 * debugger arming a breakpoint - has to see these too.
	 */
	std::vector<MonoJitInfo *> superseded;

	/*
	 * The entries this method was actually published under, recorded rather
	 * than recomputed. entries () reads the method's signature, and by the time
	 * one is freed its image may be on its way out - a shorter list at retire
	 * than at publish leaves names claimed and blocks uncarved, and the next
	 * method to land there inherits them.
	 */
	llvm::SmallVector<Entry, 3> published;

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

	MonoJitInfo *&jinfo (Entry entry)
	{
		switch (entry) {
		case Entry::body:
			return thunk_jinfo;
		case Entry::interop:
			return c_thunk_jinfo;
		case Entry::unbox:
			return unbox_jinfo;
		}
		return thunk_jinfo;
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

		/*
		 * Not in the builtins list: its address is a member of this class, which
		 * a list assembled without reference to the engine cannot name.
		 */
		if (auto err = state->jit->register_symbol (
			    "mono_llvm_jit_body_for_current_domain",
			    (void *) &MonoBackend::body_for_current_domain))
			return std::move (err);

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
	llvm::SmallVector<std::string, 3> names;

	for (Entry entry : method.published)
		names.push_back (method.name (entry));

	/*
	 * The records first. A block on the free list belongs to whichever method
	 * publishes next, and a lookup must never find a record covering memory a
	 * later compile has already been handed - a delegate built over that
	 * method's address would otherwise be bound to this one, by then freed
	 * metadata. Unreachable by name is not enough; the address is reachable too.
	 */
	for (Entry entry : method.published)
		if (MonoJitInfo *jinfo = method.jinfo (entry))
			mono_jit_info_table_remove (domain, jinfo);

	for (MonoJitInfo *jinfo : method.owned.jinfos)
		mono_jit_info_table_remove (domain, jinfo);

	/*
	 * By name first, then by address: nothing can find the stub through the
	 * linker any more, and only then is the block free to be carved again.
	 */
	must (jit->undefine_stubs (names));

	for (Entry entry : method.published)
		callbacks->release (method.trampoline (entry));

	stub_table->remove_all (names);

	if (!method.owned.dylibs.empty ())
		must (jit->remove_dylibs (method.owned.dylibs));
}

llvm::Expected<MonoBackend::MethodState *>
MonoBackend::publish (DomainState &domain, MonoMethod *method)
{
	/*
	 * The domain lock is the outer one of the two. A mutator can arrive here
	 * already holding it - mono_class_proxy_vtable compiles a remoting
	 * trampoline under it - and registering the stubs' jit info below takes it
	 * as well, so taking them the other way round on the compile worker
	 * deadlocks against a compile on a mutator thread.
	 */
	DomainLock domain_lock (domain.domain);
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
				/*
				 * The thread that fires a stub is not necessarily
				 * running as the domain that owns it, so binding here
				 * can weld one domain's copy into another's code. The
				 * body goes to a dispatcher instead; the C entry keeps
				 * a thunk of its own, because a caller arriving there
				 * speaks C and a dispatcher forwards in this engine's
				 * convention.
				 */
				if (entry == Entry::body
				    && !bindable (domain.domain, raw->method)) {
					llvm::Expected<void *> forward =
						dispatcher (domain, *raw);

					if (forward) {
						raw->stub (entry).redirect (*forward);
						return *forward;
					}
					llvm::logAllUnhandledErrors (forward.takeError (),
					                             llvm::errs (), "mono: ");
				}

				llvm::Expected<void *> code = entry_point (domain, *raw, entry);

				if (code) {
					raw->stub (entry).redirect (*code);
					return *code;
				}

				/*
				 * A stub is the end of the line for a failure: the
				 * trampoline behind it has already put the call's
				 * arguments back, and no caller is expecting a miss.
				 * So it becomes a body that raises, and costs the one
				 * method that could not be compiled rather than the
				 * process.
				 */
				auto note = [] (const CompiledMethod &, MonoJitInfo *) {};
				llvm::Expected<Compiled> raising =
					raise_on_call (*domain.jit, domain.domain,
				                       raw->method, code.takeError (), note);

				if (!raising) {
					llvm::logAllUnhandledErrors (raising.takeError (),
					                             llvm::errs (), "mono: ");
					return (void *) &lazy_compile_failed;
				}

				raw->stub (entry).redirect (raising->at (entry));
				return raising->at (entry);
			});
		if (!trampoline) {
			unwind ();
			return trampoline.takeError ();
		}

		names.push_back (state->name (entry));

		/*
		 * Body and unbox carry the method in the IMT register, which is also
		 * where a shared generic reads its runtime generic context. The C entry
		 * does not: native code arrives there having set no such register.
		 */
		llvm::Expected<Stub> stub =
			entry == Entry::interop
			        ? domain.stub_table->create (names.back ())
			        : domain.stub_table->create (names.back (), method);
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

	/*
	 * A stub is the only address the runtime ever sees for the method, so
	 * anything recovering a method from a code pointer - delegate creation off
	 * an ldftn most visibly - has to find it in the jit-info table. Registered
	 * the way mini registers trampolines: an entry carrying the method, in the
	 * domain whose linker holds the stub, so the two die together.
	 */
	for (Entry entry : entries)
		state->jinfo (entry) = register_stub_jinfo (domain.domain, method,
		                                            state->stub (entry).code (),
		                                            arch::stub_block_size,
		                                            state->name (entry));

	state->published.assign (entries.begin (), entries.end ());
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

/*
 * The receiver a value type's vtable slot arrives with is the boxed object, and
 * stepping it past the header is the whole of the difference - so this is the
 * runtime's own unboxing trampoline, over the shared entry thunk, which is where
 * mono_arch_get_unbox_trampoline's receiver-in-the-first-register assumption
 * holds.
 */
llvm::Expected<MonoBackend::Compiled>
MonoBackend::interp_entries (DomainState &domain, MethodState &method)
{
	llvm::Expected<const arch::InterpEntryPoint *> ready =
		interp_entry (domain.domain, method.method);

	if (!ready)
		return ready.takeError ();

	/* Before the redirects below, so no call arrives before it is counted. */
	mini_get_interp_callbacks ()->arm_tier_counter ((*ready)->imethod,
	                                                (gint32) tier1_threshold ());

	void *body = arch::interp_entry_thunk ();
	Compiled entries { body, body,
		           publishes_unbox_entry (method.method)
		                   ? mono_arch_get_unbox_trampoline (method.method, body)
		                   : nullptr };

	for (Entry each : method.published)
		if (void *address = entries.at (each))
			method.stub (each).redirect (address);

	MONO_LOCK (mutex_)
	{
		method.code = entries;
		method.interpreted = true;
	}

	if (is_jit_trace_enabled ()) {
		char *name = mono_method_full_name (method.method, TRUE);

		fprintf (stderr, "[llvm-jit] interpreting %s (for %s)\n", name,
		         domain.domain->friendly_name);
		g_free (name);
	}

	return entries;
}

llvm::Expected<void *>
MonoBackend::dispatcher (DomainState &domain, MethodState &method)
{
	MONO_LOCK (mutex_)
	{
		if (method.dispatch != nullptr)
			return method.dispatch;
	}

	auto note = [&] (const CompiledMethod &compiled, MonoJitInfo *jinfo) {
		if (!method.method->dynamic)
			return;

		MONO_LOCK (mutex_)
		{
			if (compiled.dylib != nullptr)
				method.owned.dylibs.push_back (compiled.dylib);
			if (jinfo != nullptr)
				method.owned.jinfos.push_back (jinfo);
		}
	};

	llvm::Expected<void *> built =
		build_dispatcher (*domain.jit, domain.domain, method.method, note);

	if (!built)
		return built;

	MONO_LOCK (mutex_)
	{
		method.dispatch = *built;
	}

	return *built;
}

/*
 * A dispatcher is the only caller, so the engine exists. Having no state for the
 * domain being called into is not something the program could be told about -
 * there is no linker to compile the telling.
 */
void *
MonoBackend::body_for_current_domain (MonoMethod *method)
{
	llvm::Expected<DomainState *> domain = instance->state ();

	if (!domain)
		llvm::report_fatal_error (llvm::Twine ("no domain state for a dispatched call: ")
		                                  + llvm::toString (domain.takeError ()),
		                          false);

	llvm::Expected<MethodState *> published = instance->publish (**domain, method);

	if (!published)
		llvm::report_fatal_error (llvm::Twine ("a dispatched method could not be published: ")
		                                  + llvm::toString (published.takeError ()),
		                          false);

	llvm::Expected<void *> body =
		instance->entry_point (**domain, **published, Entry::body);

	if (!body)
		llvm::report_fatal_error (llvm::Twine ("a dispatched method failed to compile: ")
		                                  + llvm::toString (body.takeError ()),
		                          false);

	return *body;
}

llvm::Expected<void *>
MonoBackend::entry_point (DomainState &domain, MethodState &method, Entry entry)
{
	MONO_LOCK (mutex_)
	{
		if (method.code && !recompiling (method.method))
			return method.code->at (entry);
	}

	llvm::Expected<Compiled> code = compile_body (domain, method, /*allow_tier0=*/true);

	if (!code)
		return code.takeError ();

	return code->at (entry);
}

llvm::Expected<MonoBackend::Compiled>
MonoBackend::compile_body (DomainState &domain, MethodState &method, bool allow_tier0)
{
	/*
	 * Materialize as the domain the code is for, not as whatever the calling
	 * thread happens to be running as: a stub fires under the thread's current
	 * domain, and AppDomain:InvokeInDomain switches that before calling. An
	 * address baked in from the wrong domain is a pointer into another domain's
	 * vtables and statics, live until that domain unloads under it.
	 */
	DomainScope entered (domain.domain);

	/*
	 * Before anything is translated, so a method gets the same verdict whichever
	 * tier ends up running it: a body the verifier rejects is one no tier may
	 * run, and one it accepts is one every tier may.
	 */
	if (llvm::Error invalid = verify_method (method.method))
		return std::move (invalid);

	/*
	 * The stubs are pointed at the interpreter before the method is
	 * transformed, and that order matters: transforming runs the class
	 * initializer, and this can be running inside a lazy stub's callback. A
	 * cctor that calls back into this very method would re-enter the trampoline
	 * being resolved and start its compile again below itself, with nothing to
	 * stop it. Publishing first means such a call lands on the entry instead.
	 */
	if (allow_tier0 && runs_at_tier0 (method.method)) {
		llvm::Expected<Compiled> entries = interp_entries (domain, method);

		if (!entries) {
			/*
			 * Neither the interpreter refusing the method nor this machine's
			 * entry being unable to carry the call is a failure: the method
			 * gets compiled like anything else.
			 */
			llvm::consumeError (entries.takeError ());
		} else {
			ERROR_DECL (transform_error);

			if (mini_get_interp_callbacks ()->transform_method (method.method,
			                                                    transform_error))
				return *entries;

			mono_error_cleanup (transform_error);
		}
	}

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
	/*
	 * Only a dynamic method is ever freed, and only its own compiles have to be
	 * taken back out again - everything else dies with the domain.
	 */
	auto note = [&] (const CompiledMethod &compiled, MonoJitInfo *jinfo) {
		if (!method.method->dynamic)
			return;

		MONO_LOCK (mutex_)
		{
			if (compiled.dylib != nullptr)
				method.owned.dylibs.push_back (compiled.dylib);
			if (jinfo != nullptr)
				method.owned.jinfos.push_back (jinfo);
		}
	};
	auto recover_failure = [&] (llvm::Error failure) -> llvm::Expected<Compiled> {
		return recover (*domain.jit, domain.domain, method.method,
		                std::move (failure), note);
	};

	TranslationTarget target { domain.jit.get (), domain.domain, publish_callee,
		                   stub_address, note, recover_failure };

	llvm::Expected<Compiled> code =
		translate_and_compile (target, method.method, &published);

	if (!code)
		return code.takeError ();

	/*
	 * Before the redirects below, which are what make the body reachable. A body
	 * that goes live carrying none of the breakpoints already set on the method
	 * is a breakpoint that stops being hit the moment the method is compiled
	 * again, with nothing said about it.
	 */
	if (published != nullptr)
		mini_install_pending_breakpoints (domain.domain,
		                                  jinfo_get_method (published), published);

	/*
	 * Every entry, not just the one asked for: the stubs are redirected together
	 * so that whichever door a caller came in through it lands on this compile.
	 */
	for (Entry each : method.published)
		if (void *address = code->at (each))
			method.stub (each).redirect (address);

	/*
	 * The body being replaced is not dead - a thread can still be running in it
	 * - so it moves to the superseded list rather than being dropped.
	 */
	MONO_LOCK (mutex_)
	{
		if (method.code && method.code->jinfo != nullptr
		    && method.code->jinfo != code->jinfo)
			method.superseded.push_back (method.code->jinfo);
		method.code = *code;
		method.interpreted = false;
	}

	/*
	 * Only now, and outside the lock: the debugger agent's handler for this
	 * parks the compiling thread and lets its own thread look the method up,
	 * through a door that takes the same lock.
	 */
	if (published != nullptr)
		raise_jit_done (jinfo_get_method (published), published);

	/*
	 * A method the interpreter is already running calls its callees by
	 * interpreting them, and has no other way of noticing that one of them has
	 * since been given code to call instead.
	 */
	if (mono_use_interpreter)
		mini_get_interp_callbacks ()->method_compiled (domain.domain, method.method);

	return *code;
}

/*
 * Runs on a mutator thread inside the interpreter, so it hands everything it
 * can to the worker. Nothing waits for the result and there is nobody to report
 * a failure to: the method stays at the tier it is at.
 */
void
MonoBackend::request_promotion (MonoMethod *method, MonoDomain *domain)
{
	if (!instance)
		return;

	llvm::Expected<DomainState *> state = instance->state (domain);

	if (!state) {
		llvm::consumeError (state.takeError ());
		return;
	}

	DomainState *owner = *state;
	/* Held rather than read again on the worker: at exit the engine is
	 * unhooked from instance before the destructor drains the queue. */
	MonoBackend *self = instance;

	owner->queue.enqueue (method, [self, owner, method] () {
		/* The interpreter reaches a callee without the backend being asked
		 * for it, so there may be no state for this method yet. */
		llvm::Expected<MethodState *> published = self->publish (*owner, method);

		if (!published) {
			llvm::logAllUnhandledErrors (published.takeError (), llvm::errs (),
			                             "mono: could not publish a promoted method: ");
			return;
		}

		llvm::Expected<Compiled> body =
			self->compile_body (*owner, **published, /*allow_tier0=*/false);

		if (!body)
			llvm::logAllUnhandledErrors (body.takeError (), llvm::errs (),
			                             "mono: could not promote a method: ");
	});
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

	/*
	 * After the drain, so that nothing is left running that could resolve an
	 * entry for this domain and put it back.
	 */
	forget_interp_entries (domain);
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
	 * After the queue is drained, so that a promotion still running cannot
	 * resolve an entry back into the map behind this.
	 */
	forget_interp_entry (method);

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

void *
MonoBackend::body_of (MonoDomain *domain, MonoMethod *method)
{
	if (!instance)
		return nullptr;

	MONO_LOCK (instance->mutex_)
	{
		auto state = instance->domains_.find (domain);

		if (state == instance->domains_.end ())
			return nullptr;

		auto it = state->second->methods.find (method);

		/*
		 * An interpreted method is answered for as if it had none. What it has
		 * is the entry thunk, which every interpreted method shares - so
		 * handing it back names no method in particular, and callers take it
		 * for a body: they look its jit info up by address, and the interpreter
		 * reads it as "this has native code now" and starts calling it through
		 * the native boundary instead of interpreting it.
		 */
		if (it == state->second->methods.end () || !it->second->code
		    || it->second->interpreted)
			return nullptr;
		return it->second->code->body;
	}

	return nullptr;
}

void
MonoBackend::foreach_body (MonoDomain *domain, MonoMethod *method,
                           void (*visit) (MonoJitInfo *, void *), void *user_data)
{
	if (!instance)
		return;

	/*
	 * Collected under the lock and visited outside it: what the debugger does
	 * with a body is look the method up again, through a door that takes this
	 * same lock.
	 */
	llvm::SmallVector<MonoJitInfo *, 2> bodies;

	MONO_LOCK (instance->mutex_)
	{
		auto state = instance->domains_.find (domain);

		if (state == instance->domains_.end ())
			return;

		auto it = state->second->methods.find (method);

		if (it == state->second->methods.end ())
			return;

		/* Oldest first, which is what a breakpoint walk expects. */
		bodies.assign (it->second->superseded.begin (),
		               it->second->superseded.end ());
		if (it->second->code && it->second->code->jinfo != nullptr)
			bodies.push_back (it->second->code->jinfo);
	}

	for (MonoJitInfo *jinfo : bodies)
		visit (jinfo, user_data);
}

void *
MonoBackend::unbox_entry_of (MonoMethod *method)
{
	if (!instance || !publishes_unbox_entry (method))
		return nullptr;

	llvm::Expected<DomainState *> domain = instance->state ();

	if (!domain) {
		llvm::consumeError (domain.takeError ());
		return nullptr;
	}

	llvm::Expected<MethodState *> published = instance->publish (**domain, method);

	if (!published) {
		llvm::consumeError (published.takeError ());
		return nullptr;
	}

	return (*published)->stub (Entry::unbox).code ();
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
	Entry door = publishes_interop_entry (method) ? Entry::interop : Entry::body;

	/*
	 * Compiled here rather than on the first call through the stub. Two reasons,
	 * and the second is not an optimisation: a refusal can come back through
	 * MonoError and be raised by the runtime, which knows how to throw from
	 * where it stands; and the caller may hand the address to native code that
	 * calls it from a thread mono has never seen, where there is no domain to
	 * compile against and no thread info to read one from.
	 */
	if (llvm::Error err = entry_point (**state, **published, door).takeError ())
		return std::move (err);

	return (*published)->stub (door).code ();
}

} // namespace mono

#include "backend.hpp"
#include "callbacks.hpp"
#include "jitlink-memory.hpp"
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
#include <mono/mini/thunk.hpp>
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
#include "mono/metadata/class-internals.h"
#include "mono/metadata/domain-internals.h"

namespace mono {

namespace {

/*
 * Where a thunk lands when the compile behind it failed. The trampoline has
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

/// What compiling a method put somewhere it has to be taken back out of when
/// the method is freed.
struct Owned {
	std::vector<llvm::orc::JITDylib *> dylibs;
	std::vector<MonoJitInfo *> jinfos;
};

/// What this engine keeps for one method in one domain, hung off that method's
/// MonoDomainMethod record. The record owns the thunk, the name, the tier and
/// the bodies; this is what the engine needs beside them.
struct MonoBackend::MethodState {
	/// What this method's compiles put somewhere it has to be taken back out
	/// of.
	Owned owned;

	/// The per-call dispatcher this method's thunk got instead of a direct
	/// binding, when its first caller arrived from another domain.
	void *dispatch = nullptr;
};

struct MonoBackend::DomainState {
	/// The actual domain we are tracking.
	MonoDomain *domain;

	/// The code memory we allocate the actual functions and thunks out of. It
	/// is declared before everything that carves out of it, so it is destroyed
	/// after them.
	CodeArena code;

	/// The MonoJit that is used to compile methods.
	std::unique_ptr<MonoJit> jit;

	/// The re-entry trampolines a published-but-uncompiled thunk points at.
	std::unique_ptr<LazyCallbacks> callbacks;

	CompileQueue::Channel queue;

	DomainState (MonoDomain *domain, CompileQueue &queue) : domain (domain), queue (&queue) {}

	static llvm::Expected<std::unique_ptr<DomainState>> create (MonoDomain *domain,
	                                                            CompileQueue &queue)
	{
		auto state = std::make_unique<DomainState> (domain, queue);

		auto jit = MonoJit::create (&state->code);
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

	/// Undefine everything \p dm was published as and hand its blocks and
	/// trampolines back.
	void retire (MonoDomainMethod &dm);
};

MonoBackend::~MonoBackend ()
{
	queue_.stop ();
}

/*
 * A failure here means the method being freed is still reachable - a thunk the
 * linker will not give up, a block the table does not know about. There is
 * nothing to fall back to and continuing hands the next method a thunk
 * something is still calling, so say what broke and stop.
 */
static void
must (llvm::Error err)
{
	if (!err)
		return;

	llvm::logAllUnhandledErrors (std::move (err), llvm::errs (), "mono: ");
	llvm::report_fatal_error ("mono: could not release a method's thunks",
	                          /*GenCrashDiag=*/false);
}

void
MonoBackend::DomainState::retire (MonoDomainMethod &dm)
{
	MethodState &engine = MonoBackend::engine_state (dm);

	/*
	 * The records first. Nothing publishes this method's thunk by name any more
	 * for a lookup to find it through, so the jit-info table is what a
	 * concurrent reference to this method resolves against. A lookup must never
	 * find an entry for a method whose thunk has already gone into its fatal
	 * trap.
	 */
	if (MonoJitInfo *jinfo = dm.jinfo)
		mono_jit_info_table_remove (domain, jinfo);

	for (MonoJitInfo *jinfo : engine.owned.jinfos)
		mono_jit_info_table_remove (domain, jinfo);

	/*
	 * take_thunk () is under the record's own lock, the same one thunk_address ()
	 * reads under - see its doc comment - so a concurrent thunk_address () always
	 * sees a fully-published thunk, never a torn read of the record. What follows
	 * is a single atomic store in Thunk::redirect (): any caller going through the
	 * address it returned lands on the live target or the trap, never in between.
	 */
	callbacks->release (dm.trampoline);
	dm.take_thunk ().quarantine ();

	if (!engine.owned.dylibs.empty ())
		must (jit->remove_dylibs (engine.owned.dylibs));
}

MonoBackend::MethodState &
MonoBackend::engine_state (MonoDomainMethod &dm)
{
	return *static_cast<MethodState *> (dm.engine_data.get ());
}

llvm::Expected<MonoDomainMethod *>
MonoBackend::publish (DomainState &domain, MonoMethod *method)
{
	return domain_method_get (domain.domain, method);
}

llvm::Error
MonoBackend::attach (MonoDomainMethod &dm)
{
	llvm::Expected<MonoBackend *> backend = get ();

	if (!backend)
		return backend.takeError ();
	if (*backend == nullptr)
		return llvm::createStringError (llvm::inconvertibleErrorCode (),
		                                "the engine has been taken apart");

	MonoBackend *self = *backend;
	llvm::Expected<DomainState *> state = self->state (dm.domain);

	if (!state)
		return state.takeError ();

	return self->attach_entry (**state, dm);
}

llvm::Error
MonoBackend::attach_entry (DomainState &domain, MonoDomainMethod &dm)
{
	MonoMethod *method = dm.method;
	auto engine = std::make_unique<MethodState> ();

	dm.engine_data = {engine.release (),
	                  [] (void *data) { delete static_cast<MethodState *> (data); }};

	/* Tier policy is this engine's, so the record is told rather than asked. */
	dm.tier_calls.store (tier0_calls (method), std::memory_order_relaxed);

	llvm::Expected<void *> trampoline = domain.callbacks->reserve (
		[this, &domain, &dm] () -> void * {
			/* The thunk follows, so the next call skips the trampoline. */
			auto answer = [&] (void *code) {
				dm.publish (MonoTier::none, code);
				return code;
			};

			/*
			 * The thread that fires a thunk is not necessarily running as
			 * the domain that owns it, so binding here can weld one
			 * domain's copy into another's code. It goes to a dispatcher
			 * instead, which picks the body out per call.
			 */
			if (!bindable (domain.domain, dm.method)) {
				llvm::Expected<void *> forward = dispatcher (domain, dm);

				if (forward)
					return answer (*forward);
				llvm::logAllUnhandledErrors (forward.takeError (),
				                             llvm::errs (), "mono: ");
			}

			llvm::Expected<void *> code = entry_point (domain, dm);

			if (code)
				return answer (*code);

			/*
			 * A thunk is the end of the line for a failure: the trampoline
			 * behind it has already put the call's arguments back, and no
			 * caller is expecting a miss. So it becomes a body that
			 * raises, and costs the one method that could not be compiled
			 * rather than the process.
			 */
			auto note = [] (const CompiledMethod &, MonoJitInfo *) {};
			llvm::Expected<Compiled> raising =
				raise_on_call (*domain.jit, domain.domain, dm.method, code.takeError (), note);

			if (!raising) {
				llvm::logAllUnhandledErrors (raising.takeError (),
				                             llvm::errs (), "mono: ");
				return (void *) &lazy_compile_failed;
			}

			return answer (raising->body);
		});
	if (!trampoline)
		return trampoline.takeError ();

	dm.name = stub_symbol (method);

	/*
	 * The thunk carries the method in the IMT register, which is also where a
	 * shared generic reads its runtime generic context. attach_entry () runs
	 * exactly once per record - domain_method_get ()'s get-or-create sees to
	 * that - so there is no existing thunk to collide with.
	 */
	llvm::Expected<Thunk> thunk = allocate_thunk (&domain.code, method);

	if (!thunk) {
		domain.callbacks->release (*trampoline);
		return thunk.takeError ();
	}

	thunk->redirect (*trampoline);
	dm.thunk = *thunk;
	dm.trampoline = *trampoline;

	/*
	 * A thunk is the only address the runtime ever sees for the method, so
	 * anything recovering a method from a code pointer - delegate creation off
	 * an ldftn most visibly - has to find it in the jit-info table. Registered
	 * the way mini registers trampolines: an entry carrying the method, in the
	 * domain whose linker holds the thunk, so the two die together.
	 *
	 * It covers the unbox prologue as well as the thunk. Both are the method,
	 * and a walk that catches a thread on either has the same frame to step
	 * off. So one record answers for the two.
	 */
	dm.jinfo = register_stub_jinfo (domain.domain, method, thunk->unbox_entry (),
	                                thunk_unbox_span, dm.name);
	return llvm::Error::success ();
}

llvm::Error
attach_method_entries (MonoDomainMethod &dm)
{
	return MonoBackend::attach (dm);
}

llvm::Error
attach_interop_entry (MonoDomainMethod &dm)
{
	return MonoBackend::attach_interop (dm);
}

/*
 * A caller arriving here speaks C, so the entry is real code rather than a
 * carved block: it takes the call apart into the values this engine's
 * convention wants. It calls the method's thunk rather than any one body, so it
 * is right at every tier and a promotion redirects it with everything else.
 */
llvm::Error
MonoBackend::attach_interop (MonoDomainMethod &dm)
{
	MonoMethod *method = dm.method;

	if (!publishes_interop_entry (method))
		return llvm::Error::success ();

	llvm::Expected<MonoBackend *> backend = get ();

	if (!backend)
		return backend.takeError ();
	if (*backend == nullptr)
		return llvm::createStringError (llvm::inconvertibleErrorCode (),
		                                "the engine has been taken apart");

	MonoBackend *self = *backend;
	llvm::Expected<DomainState *> domain = self->state (dm.domain);

	if (!domain)
		return domain.takeError ();

	MethodState &engine = engine_state (dm);
	auto note = [&] (const CompiledMethod &compiled, MonoJitInfo *jinfo) {
		MONO_LOCK (self->mutex_)
		{
			if (compiled.dylib != nullptr)
				engine.owned.dylibs.push_back (compiled.dylib);
			if (jinfo != nullptr)
				engine.owned.jinfos.push_back (jinfo);
		}
	};

	llvm::Expected<void *> code =
		compile_interop_entry (*(*domain)->jit, (*domain)->domain, method, dm.thunk.code (), note);

	if (!code)
		return code.takeError ();

	dm.set_interop_entry (*code);
	return llvm::Error::success ();
}

llvm::Expected<MonoBackend::Compiled>
MonoBackend::interp_entries (DomainState &domain, MonoDomainMethod &dm)
{
	MonoMethod *method = dm.method;
	llvm::Expected<arch::InterpEntryPoint> ready = interp_entry (dm);

	if (!ready)
		return ready.takeError ();

	void *body = arch::interp_entry_thunk ();
	Compiled entries { body };

	dm.publish (MonoTier::interp, body);
	dm.attach_body (MonoTier::interp, body, nullptr);

	if (is_jit_trace_enabled ()) {
		char *name = mono_method_full_name (method, TRUE);

		fprintf (stderr, "[llvm-jit] interpreting %s (for %s)\n", name,
		         domain.domain->friendly_name);
		g_free (name);
	}

	return entries;
}

llvm::Expected<void *>
MonoBackend::dispatcher (DomainState &domain, MonoDomainMethod &dm)
{
	MethodState &engine = engine_state (dm);

	MONO_LOCK (mutex_)
	{
		if (engine.dispatch != nullptr)
			return engine.dispatch;
	}

	auto note = [&] (const CompiledMethod &compiled, MonoJitInfo *jinfo) {
		if (!dm.method->dynamic)
			return;

		MONO_LOCK (mutex_)
		{
			if (compiled.dylib != nullptr)
				engine.owned.dylibs.push_back (compiled.dylib);
			if (jinfo != nullptr)
				engine.owned.jinfos.push_back (jinfo);
		}
	};

	llvm::Expected<void *> built = build_dispatcher (*domain.jit, domain.domain, dm.method, note);

	if (!built)
		return built;

	MONO_LOCK (mutex_)
	{
		engine.dispatch = *built;
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

	llvm::Expected<MonoDomainMethod *> published = publish (**domain, method);

	if (!published)
		llvm::report_fatal_error (llvm::Twine ("a dispatched method could not be published: ")
		                                  + llvm::toString (published.takeError ()),
		                          false);

	llvm::Expected<void *> body = instance->entry_point (**domain, **published);

	if (!body)
		llvm::report_fatal_error (llvm::Twine ("a dispatched method failed to compile: ")
		                                  + llvm::toString (body.takeError ()),
		                          false);

	return *body;
}

llvm::Expected<void *>
MonoBackend::entry_point (DomainState &domain, MonoDomainMethod &dm)
{
	if (std::optional<MonoMethodBody> ready = dm.body (); ready && !recompiling (dm.method))
		return ready->code;

	llvm::Expected<Compiled> code = compile_body (domain, dm, /*allow_tier0=*/true);

	if (!code)
		return code.takeError ();

	return code->body;
}

llvm::Expected<MonoBackend::Compiled>
MonoBackend::compile_body (DomainState &domain, MonoDomainMethod &dm, bool allow_tier0)
{
	MonoMethod *method = dm.method;
	MethodState &engine = engine_state (dm);

	/*
	 * Materialize as the domain the code is for, not as whatever the calling
	 * thread happens to be running as: a thunk fires under the thread's current
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
	if (llvm::Error invalid = verify_method (method))
		return std::move (invalid);

	/*
	 * The thunks are pointed at the interpreter before the method is
	 * transformed, and that order matters: transforming runs the class
	 * initializer, and this can be running inside a lazy thunk's callback. A
	 * cctor that calls back into this very method would re-enter the trampoline
	 * being resolved and start its compile again below itself, with nothing to
	 * stop it. Publishing first means such a call lands on the entry instead.
	 */
	if (allow_tier0 && runs_at_tier0 (method)) {
		llvm::Expected<Compiled> entries = interp_entries (domain, dm);

		if (!entries) {
			/*
			 * Neither the interpreter refusing the method nor this machine's
			 * entry being unable to carry the call is a failure: the method
			 * gets compiled like anything else.
			 */
			llvm::consumeError (entries.takeError ());
		} else {
			ERROR_DECL (transform_error);

			if (mini_get_interp_callbacks ()->transform_method (method,
			                                                    transform_error))
				return *entries;

			mono_error_cleanup (transform_error);
		}
	}

	MonoJitInfo *published = nullptr;

	/* Named: function_ref does not own what it points at. */
	auto publish_callee = [&] (MonoMethod *callee) -> llvm::Expected<MonoDomainMethod *> {
		return publish (domain, callee);
	};
	/*
	 * Only a dynamic method is ever freed, and only its own compiles have to be
	 * taken back out again - everything else dies with the domain.
	 */
	auto note = [&] (const CompiledMethod &compiled, MonoJitInfo *jinfo) {
		if (!method->dynamic)
			return;

		MONO_LOCK (mutex_)
		{
			if (compiled.dylib != nullptr)
				engine.owned.dylibs.push_back (compiled.dylib);
			if (jinfo != nullptr)
				engine.owned.jinfos.push_back (jinfo);
		}
	};
	auto recover_failure = [&] (llvm::Error failure) -> llvm::Expected<Compiled> {
		return recover (*domain.jit, domain.domain, method, std::move (failure), note);
	};

	TranslationTarget target { domain.jit.get (), domain.domain, publish_callee,
		                   note, recover_failure };

	llvm::Expected<Compiled> code =
		translate_and_compile (target, method, &published);

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

	dm.publish (MonoTier::tier1, code->body);
	dm.attach_body (MonoTier::tier1, code->body, code->jinfo);

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
		mini_get_interp_callbacks ()->method_compiled (domain.domain, method);

	return *code;
}

/*
 * Runs on a mutator thread inside the interpreter, so it hands everything it
 * can to the worker. Nothing waits for the result, and a compile that fails
 * once it is running leaves the method at the tier it is at.
 */
bool
MonoBackend::request_promotion (MonoMethod *method, MonoDomain *domain)
{
	llvm::Expected<MonoBackend *> backend = get ();

	if (!backend) {
		llvm::consumeError (backend.takeError ());
		return false;
	}

	/* Held rather than read again on the worker: at exit the engine is
	 * unhooked from instance before the destructor drains the queue. */
	MonoBackend *self = *backend;

	/* Null once the engine has been taken apart, which get () does not undo. */
	if (!self)
		return false;

	llvm::Expected<DomainState *> state = self->state (domain);

	if (!state) {
		llvm::consumeError (state.takeError ());
		return false;
	}

	DomainState *owner = *state;

	return owner->queue.enqueue (method, [self, owner, method] () {
		/* The interpreter reaches a callee without the backend being asked
		 * for it, so there may be no state for this method yet. */
		llvm::Expected<MonoDomainMethod *> published = publish (*owner, method);

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
	llvm::SmallVector<DomainState *, 2> states;
	llvm::SmallVector<std::pair<DomainState *, std::unique_ptr<MonoDomainMethod>>, 2> going;

	/*
	 * The domains under this lock and the records outside it. Taking a record
	 * takes its domain's table lock, and the order everywhere else is the table
	 * lock first.
	 */
	MONO_LOCK (mutex_)
	{
		for (const auto &entry : domains_)
			states.push_back (entry.second.get ());
	}

	for (DomainState *state : states)
		if (std::unique_ptr<MonoDomainMethod> dm =
			    domain_method_take (state->domain, method))
			going.emplace_back (state, std::move (dm));

	// Removing a dylib waits on any link that is still using symbols in it, so
	// retire () happens with the backend's own lock dropped.
	for (auto &[state, dm] : going)
		state->retire (*dm);
}

void *
MonoBackend::body_of (MonoDomain *domain, MonoMethod *method)
{
	if (!instance)
		return nullptr;

	MonoDomainMethod *dm = domain_method_find (domain, method);

	if (dm == nullptr)
		return nullptr;

	std::optional<MonoMethodBody> body = dm->body ();

	/*
	 * An interpreted method is answered for as if it had none. What it has is
	 * the entry thunk, which every interpreted method shares - so handing it
	 * back names no method in particular, and callers take it for a body: they
	 * look its jit info up by address, and the interpreter reads it as "this
	 * has native code now" and starts calling it through the native boundary
	 * instead of interpreting it.
	 */
	if (!body || body->tier == MonoTier::interp)
		return nullptr;
	return body->code;
}

void
MonoBackend::foreach_body (MonoDomain *domain, MonoMethod *method,
                           void (*visit) (MonoJitInfo *, void *), void *user_data)
{
	if (!instance)
		return;

	/*
	 * Collected under the record's lock and visited outside it: what the
	 * debugger does with a body is look the method up again, through a door
	 * that takes that same lock.
	 */
	llvm::SmallVector<MonoJitInfo *, 2> bodies;
	MonoDomainMethod *dm = domain_method_find (domain, method);

	if (dm == nullptr)
		return;

	/* Oldest first, which is what a breakpoint walk expects. */
	dm->foreach_body ([&] (const MonoMethodBody &body) {
		if (body.jinfo != nullptr)
			bodies.push_back (body.jinfo);
	});

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

	llvm::Expected<MonoDomainMethod *> published = publish (**domain, method);

	if (!published) {
		llvm::consumeError (published.takeError ());
		return nullptr;
	}

	return (*published)->unbox_entry ();
}

llvm::Expected<void *>
MonoBackend::compile (MonoMethod *method, MonoDomain *domain)
{
	llvm::Expected<DomainState *> state = this->state (domain);

	if (!state)
		return state.takeError ();

	llvm::Expected<MonoDomainMethod *> published = publish (**state, method);

	if (!published)
		return published.takeError ();

	/*
	 * Compiled here rather than on the first call through the thunk. Two reasons,
	 * and the second is not an optimisation: a refusal can come back through
	 * MonoError and be raised by the runtime, which knows how to throw from
	 * where it stands; and the caller may hand the address to native code that
	 * calls it from a thread mono has never seen, where there is no domain to
	 * compile against and no thread info to read one from.
	 */
	if (llvm::Error err = entry_point (**state, **published).takeError ())
		return std::move (err);

	/* The C door where the method has one, and the thunk itself otherwise. */
	if (publishes_interop_entry (method))
		return (*published)->interop_entry ();

	return (*published)->thunk.code ();
}

llvm::Expected<void *>
MonoBackend::stub_for (MonoMethod *method, MonoDomain *domain)
{
	llvm::Expected<DomainState *> state = this->state (domain);

	if (!state)
		return state.takeError ();

	llvm::Expected<MonoDomainMethod *> published = publish (**state, method);

	if (!published)
		return published.takeError ();

	return (*published)->thunk.code ();
}

} // namespace mono

#include "backend.hpp"
#include "callbacks.hpp"
#include "jitlink-memory.hpp"
#include "compile-queue.hpp"
#include "jit.hpp"
#include "method-to-llvm.hpp"
#include "naming.hpp"
#include "options.hpp"
#include "runtime-error.hpp"
#include <llvm/ADT/ScopeExit.h>
#include <llvm/ADT/StringRef.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/Module.h>
#include <memory>
#include <algorithm>
#include <map>
#include <mutex>
#include <unistd.h>
#include <mono/mini/thunk.hpp>
#include <llvm/Support/Memory.h>
#include "util/lock.hpp"
#include "builtins.hpp"
#include "dispatcher.hpp"
#include "interp.hpp"
#include "publish-events.hpp"
#include <vector>
#include "mini-runtime.h"
#include "stub-jinfo.hpp"
#include "thrower.hpp"
#include "timing.hpp"
#include "verification.hpp"
#include "translate.hpp"
#include "arch/arch.hpp"
#include <optional>
#include "mono/metadata/appdomain.h"
#include "mono/metadata/class-internals.h"
#include "mono/metadata/domain-internals.h"
#include "mono/metadata/marshal.h"
#include "mono/utils/mono-threads-api.h"
#include <chrono>
#include <condition_variable>

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

char StaleFold::ID = 0;

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
/// the bodies. This is what the engine needs beside them.
struct MonoBackend::MethodState {
	Owned owned;

	void *dispatch = nullptr;

	/// The stub that enters this instantiation's shared body carrying its
	/// context. Null for a method that is not answered by a shared body, and
	/// for one whose shared body reads the context out of its receiver.
	void *context = nullptr;

	std::optional<ProfileCounters> profile;
};

struct MonoBackend::DomainState {
	MonoDomain *domain;

	/// The code memory we allocate the actual functions and thunks out of. It
	/// is declared before everything that carves out of it, so it is destroyed
	/// after them.
	CodeArena code;

	std::unique_ptr<MonoJit> jit;

	std::unique_ptr<LazyCallbacks> callbacks;

	CompileQueue::Channel queue;

	/// The tier-1 promotions asked for and not yet taken. The worker takes a
	/// run of them at a time, so promotions asked for close together share one
	/// compile.
	std::mutex pending_mutex;
	std::vector<MonoMethod *> pending;

	std::vector<MonoMethod *> take_pending (uint32_t limit)
	{
		std::lock_guard<std::mutex> lock (pending_mutex);
		size_t count = std::min ((size_t) limit, pending.size ());
		std::vector<MonoMethod *> taken (pending.begin (), pending.begin () + count);

		pending.erase (pending.begin (), pending.begin () + count);
		return taken;
	}

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

		if (is_jit_trace_enabled ())
			MONO_LOCK (jit_trace_mutex ())
			{
				llvm::errs () << llvm::format (
					"[llvm-jit] %zu runtime builtins registered\n",
					builtins.size ());
			}

		return state;
	}

	void retire (MonoDomainMethod &dm);
};

/*
 * Registered from a thread that has just compiled, rather than when the backend
 * is built. LLVM builds its process-wide tables lazily inside a compile - the
 * MVT list behind SDNode::getValueTypeList (), and the file system every
 * PassBuilder holds. Exit runs handlers in reverse order of registration. A
 * handler registered before those tables exist therefore runs after they are
 * destroyed, and the wait below comes too late: by then the workers already
 * read freed memory.
 *
 * This orders the teardown against the tables a compile builds, which is less
 * than every static LLVM owns. mini_cleanup () calls
 * mono_llvm_jit_stop_compiling (), which stops the workers on a path that ends
 * properly. This handler is the net under a process that exits without one.
 *
 * A process that asks for the backend and never compiles - one that only frees
 * a method, or tears a domain down - therefore registers no handler. That is
 * correct, because it has no worker to wait for and no LLVM table to be ordered
 * against. Moving this back into MonoBackend::get () to cover it puts the
 * ordering back the wrong way round.
 */
void
MonoBackend::register_exit_teardown ()
{
	static std::once_flag once;

	std::call_once (once, [] {
		atexit ([] {
			/*
			 * fork () keeps only the thread that called it, so a child has
			 * none of the workers stop_compilation () joins, and that join
			 * never ends. Only the process that built the engine takes it
			 * apart.
			 */
			if (getpid () != owner_pid)
				return;

			/*
			 * The engine is not deleted, and only its compile queue stops.
			 * The mutators still run at this point, and a mutator compiles
			 * as well: the finalizer thread compiles the wrapper it invokes
			 * a finalizer through. A delete here would free mutex_ under
			 * such a thread, which takes it in MonoBackend::state ().
			 */
			stop_compilation ();
		});
	});
}

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
	callbacks->release (dm.compile_trampoline);
	dm.take_thunk ().quarantine ();

	if (!engine.owned.dylibs.empty ())
		must (jit->remove_dylibs (engine.owned.dylibs));
}

MonoBackend::MethodState &
MonoBackend::engine_state (MonoDomainMethod &dm)
{
	return *static_cast<MethodState *> (dm.engine_data.get ());
}

std::optional<ProfileCounters>
MonoBackend::profile_of (MonoDomainMethod &dm)
{
	// Both are null between publishing a record and compiling anything into it.
	if (instance == nullptr || dm.engine_data.get () == nullptr)
		return std::nullopt;

	MethodState &engine = engine_state (dm);

	MONO_LOCK (instance->mutex_) { return engine.profile; }

	return std::nullopt;
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

	/*
	 * Two trampolines rather than one, so that deciding and compiling are
	 * separate calls. The policy one below is what the thunk is published
	 * pointing at, and it answers without waiting for anything. A method it
	 * sends to a compiled tier is published at this one instead, so a thread
	 * that has to wait for a body waits at the top of the thunk it entered
	 * rather than somewhere down the compile.
	 */
	llvm::Expected<void *> compiling = domain.callbacks->reserve (
		[this, &domain, &dm] () -> void * { return compile_entry (domain, dm); });
	if (!compiling)
		return compiling.takeError ();

	dm.compile_trampoline = *compiling;

	llvm::Expected<void *> trampoline = domain.callbacks->reserve (
		[this, &domain, &dm] () -> void * { return policy_entry (domain, dm); });
	if (!trampoline) {
		domain.callbacks->release (*compiling);
		return trampoline.takeError ();
	}

	dm.name = stub_symbol (method);

	llvm::Expected<Thunk> thunk = Thunk::allocate (&domain.code, method);

	if (!thunk) {
		domain.callbacks->release (*trampoline);
		domain.callbacks->release (*compiling);
		return thunk.takeError ();
	}

	thunk->redirect (*trampoline);
	dm.thunk = *thunk;
	dm.trampoline = *trampoline;

	dm.jinfo = thunk->register_jinfo(dm.name, domain.domain, method);

	return llvm::Error::success ();
}

void *
MonoBackend::policy_entry (DomainState &domain, MonoDomainMethod &dm)
{
	/*
	 * A thread reaches this trampoline after the thunk has moved on when it
	 * read the thunk and the redirect landed behind it.
	 */
	std::optional<MonoMethodBody> ready = dm.body ();

	if (ready && !recompiling (dm.method))
		return ready->code;

	/*
	 * The thread that fires a thunk is not necessarily running as the domain
	 * that owns it, so binding here can weld one domain's copy into another's
	 * code. It goes to a dispatcher instead, which picks the body out per call.
	 */
	if (!bindable (domain.domain, dm.method)) {
		llvm::Expected<void *> forward = dispatcher (domain, dm);

		if (forward) {
			/* The thunk follows, so the next call skips the trampoline. */
			dm.publish (MonoTier::none, *forward);
			return *forward;
		}

		llvm::logAllUnhandledErrors (forward.takeError (), llvm::errs (), "mono: ");
	}

	/*
	 * The entry moves to the compile before the interpreter is offered the
	 * method, and that order is what makes the offer safe to take here.
	 * Transforming a method runs its class initializer, and a cctor that calls
	 * back into this very method would otherwise re-enter the trampoline being
	 * resolved and take this decision again below itself, with nothing to stop
	 * it. It reaches the compile instead.
	 */

	/* A tier above this one already owns the entry, a detour among them. It
	 * keeps it, and the caller goes through the thunk to reach it. */
	if (!dm.publish (MonoTier::none, dm.compile_trampoline))
		return dm.thunk.code ();

	llvm::Expected<Compiled> interpreted = tier0_entry (domain, dm);

	if (interpreted)
		return interpreted->body;

	llvm::consumeError (interpreted.takeError ());
	return dm.compile_trampoline;
}

void *
MonoBackend::compile_entry (DomainState &domain, MonoDomainMethod &dm)
{
	/*
	 * The entry can have moved on since the policy step published this
	 * trampoline: it publishes before it offers the method to the interpreter,
	 * so a thread that read the thunk in between arrives here for a method the
	 * interpreter now runs.
	 */
	std::optional<MonoMethodBody> ready = dm.body ();

	if (ready && !recompiling (dm.method))
		return ready->code;

	llvm::Expected<void *> code = entry_point (domain, dm, /*allow_tier0=*/false);

	if (code)
		return *code;

	/*
	 * A thunk is the end of the line for a failure: the trampoline behind it
	 * has already put the call's arguments back, and no caller is expecting a
	 * miss. So it becomes a body that raises, and costs the one method that
	 * could not be compiled rather than the process.
	 */
	auto note = [] (const CompiledMethod &, MonoJitInfo *) {};
	llvm::Expected<Compiled> raising =
		raise_on_call (*domain.jit, domain.domain, dm.method, code.takeError (), note);

	if (!raising) {
		llvm::logAllUnhandledErrors (raising.takeError (), llvm::errs (), "mono: ");
		return (void *) &lazy_compile_failed;
	}

	/* The thunk follows, so the next call skips the trampoline. */
	dm.publish (MonoTier::none, raising->body);
	return raising->body;
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

llvm::Expected<void *>
published_entry_of (MonoDomainMethod &dm)
{
	return MonoBackend::published_entry (dm);
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
MonoBackend::tier0_entry (DomainState &domain, MonoDomainMethod &dm)
{
	MonoMethod *method = dm.method;

	if (!runs_at_tier0 (method))
		return llvm::createStringError (llvm::inconvertibleErrorCode (),
		                                "tier 0 does not run this method");

	/*
	 * A method whose entry went back to this trampoline after a fold was
	 * replaced ran compiled before, so its tier-0 call counter is spent. Sent
	 * to the interpreter now, it would stay there.
	 */
	if (dm.past_tier0 ())
		return llvm::createStringError (llvm::inconvertibleErrorCode (),
		                                "the method has left tier 0 already");

	// Transforming the method runs its class initializer, which has to run as
	// the domain the code is for rather than as whatever the calling thread
	// happens to be running as.
	DomainScope entered (domain.domain);

	/*
	 * Before the interpreter is offered the method, so it gets the same verdict
	 * whichever tier ends up running it: a body the verifier rejects is one no
	 * tier may run, and one it accepts is one every tier may. The compile asks
	 * again, and a second answer costs nothing - the verifier records its
	 * verdict on the method.
	 */
	if (llvm::Error invalid = verify_method (method))
		return std::move (invalid);

	llvm::Expected<arch::InterpEntryPoint> ready = interp_entry (dm);

	if (!ready)
		return ready.takeError ();

	ERROR_DECL (transform_error);

	if (!mini_get_interp_callbacks ()->transform_method (method, transform_error)) {
		llvm::Error refused = llvm::createStringError (
			llvm::inconvertibleErrorCode (),
			"the interpreter could not transform the method: %s",
			mono_error_get_message (transform_error));

		mono_error_cleanup (transform_error);
		return std::move (refused);
	}

	void *body = arch::interp_entry_thunk ();

	/*
	 * A tier above this one can have taken the entry while the transform ran:
	 * a cctor it ran can call back into this very method and reach the compile
	 * entry, and a detour outranks every tier at any time. Whatever owns the
	 * entry keeps it, and the caller goes through the thunk to reach it.
	 */
	if (!dm.publish (MonoTier::interp, body))
		return Compiled { dm.thunk.code () };

	dm.attach_body (MonoTier::interp, body, nullptr);

	if (is_jit_trace_enabled ()) {
		char *name = mono_method_full_name (method, TRUE);

		MONO_LOCK (jit_trace_mutex ())
		{
			fprintf (stderr, "[llvm-jit] interpreting %s (for %s)\n", name,
			         domain.domain->friendly_name);
		}
		g_free (name);
	}

	return Compiled { body };
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

	/*
	 * The interpreter's entry reads its MonoMethod * out of the register the
	 * method's own thunk writes. A dispatcher does not go through that thunk: it
	 * gets here through a C call, which destroys the register, and it leaves
	 * with a musttail jump. LLVM holds a value in %r10 across both, because the
	 * nest attribute pins one there, but it has nothing that pins %r11 - the
	 * register allocator takes %r11 for stack-argument copies and for the tail
	 * jump itself. So a method a dispatcher answers for is compiled, whatever
	 * tier it runs at elsewhere.
	 */
	llvm::Expected<void *> body =
		instance->entry_point (**domain, **published, /*allow_tier0=*/false);

	if (!body)
		llvm::report_fatal_error (llvm::Twine ("a dispatched method failed to compile: ")
		                                  + llvm::toString (body.takeError ()),
		                          false);

	return *body;
}

llvm::Expected<void *>
MonoBackend::entry_point (DomainState &domain, MonoDomainMethod &dm, bool allow_tier0)
{
	for (;;) {
		std::optional<MonoMethodBody> ready = dm.body ();

		if (ready && !recompiling (dm.method)
		    && (allow_tier0 || ready->code != arch::interp_entry_thunk ()))
			return ready->code;

		llvm::Expected<Compiled> code =
			compile_body (domain, dm, allow_tier0, MonoTier::tier1);

		if (code)
			return code->body;

		/*
		 * Refused rather than not built. The replacement is installed by now,
		 * so the next translation folds nothing stale in and the loop ends.
		 */
		if (!code.errorIsA<StaleFold> ())
			return code.takeError ();

		llvm::consumeError (code.takeError ());
	}
}

/*
 * Reference sharing only. Every number a body burns - a field offset, an array
 * element size, a vtable slot index - is the same for every reference
 * instantiation, because a reference is one pointer whatever it points at. A
 * value type changes all of them, so an instantiation naming one gets a body of
 * its own.
 */
static MonoMethod *
shared_form (MonoMethod *method)
{
	if (!mono_class_generic_sharing_enabled (method->klass))
		return nullptr;

	// The shared method is itself open, and asking it for its own shared form
	// again is how this would recurse.
	if (mono_method_check_context_used (method) != 0)
		return nullptr;

	if (!mono_method_is_generic_sharable_full (method, FALSE, FALSE, FALSE))
		return nullptr;

	ERROR_DECL (share_error);
	MonoMethod *shared = mini_get_shared_method_full (method, SHARE_MODE_NONE, share_error);

	mono_error_cleanup (share_error);

	if (shared == nullptr || shared == method)
		return nullptr;

	return shared;
}

/*
 * One thread at a time per shared form. Two instantiations of one generic reach
 * the same record, and two threads that both find it unbuilt would both compile
 * it, which is work the second of them need not do.
 *
 * A wait for a claim cannot cycle, because no thread that holds one comes to
 * want a second. The holder compiles the shared method, which is open, and
 * shared_form () refuses an open method, so that nested compile takes no claim.
 *
 * It can still wait behind a runtime lock. A compile takes the loader lock, and
 * a mutator that arrives here can already hold it, so a waiter and a holder can
 * each be what the other is waiting for. That is what the bound below is for:
 * the waiter gives up, builds the body itself, and drops the lock the holder
 * wants on the way out. mini takes the same way out of the same shape, with the
 * same second - see wait_or_register_method_to_compile ().
 */
static constexpr std::chrono::milliseconds shared_body_wait { 1000 };

MonoBackend::SharedClaim
MonoBackend::claim_shared_body (MonoDomainMethod *owner)
{
	std::unique_lock<std::mutex> lock (mutex_);

	/* A deadline rather than a duration for each wait: the variable covers
	 * every record, so an unrelated release restarts a duration. */
	auto expires = std::chrono::steady_clock::now () + shared_body_wait;

	while (!sharing_.insert (owner).second) {
		std::cv_status waited;

		/*
		 * A thread parked here reaches no safepoint, so a collection that
		 * tries to suspend it waits for a compile on another thread. The
		 * wait reads no managed object, which is what the safe region asks
		 * of it.
		 */
		MONO_ENTER_GC_SAFE;
		waited = shared_claims_.wait_until (lock, expires);
		MONO_EXIT_GC_SAFE;

		if (sharing_.count (owner) == 0)
			return SharedClaim::done;

		if (waited == std::cv_status::timeout)
			return SharedClaim::expired;
	}

	return SharedClaim::held;
}

void
MonoBackend::release_shared_body (MonoDomainMethod *owner)
{
	MONO_LOCK (mutex_) { sharing_.erase (owner); }

	shared_claims_.notify_all ();
}

/*
 * The shared method gets a record of its own, so its body is compiled once and
 * carries the jit info a stack walk reads. This method's entry then names that
 * body and it keeps no body record for it: the frame belongs to the shared
 * method, not to this instantiation of it.
 */
llvm::Expected<MonoBackend::Compiled>
MonoBackend::enter_shared_body (DomainState &domain, MonoDomainMethod &dm,
                                MonoMethod *shared, MonoTier tier)
{
	llvm::Expected<MonoDomainMethod *> owner = publish (domain, shared);

	if (!owner)
		return owner.takeError ();

	std::optional<MonoMethodBody> ready = (*owner)->body ();

	while (!ready || ready->tier < tier) {
		SharedClaim claim = claim_shared_body (*owner);

		if (claim == SharedClaim::done) {
			// claim_shared_body () decides under mutex_, and a read of
			// the body takes the record's own lock, so the read is here.
			ready = (*owner)->body ();

			/* Back to the condition, which ends the loop when the body
			 * the holder built is at this compile's tier, and asks for
			 * the claim again when it is below it or when the holder
			 * built none. */
			continue;
		}

		bool holding = claim == SharedClaim::held;
		auto unclaim = llvm::make_scope_exit ([&] {
			if (holding)
				release_shared_body (*owner);
		});

		if (!holding && is_jit_trace_enabled ())
			MONO_LOCK (jit_trace_mutex ())
			{
				llvm::errs () << "[llvm-jit] building " << (*owner)->name
					      << " beside another thread's compile of it\n";
			}

		/* Again, now that we hold the claim: the thread that had it may have
		 * published the body between our first read and the claim. */
		ready = (*owner)->body ();

		if (ready && ready->tier >= tier)
			break;

		llvm::Expected<Compiled> built =
			compile_body (domain, **owner, /*allow_tier0=*/false, tier,
		                      /*for_sharing=*/true);

		if (!built)
			return built.takeError ();

		ready = (*owner)->body ();

		if (!ready)
			return llvm::make_error<SharingRefusal> (
				"the shared body was not published");

		break;
	}

	void *entry = ready->code;

	/*
	 * A shared body with no receiver is entered with the context in a register,
	 * and this instantiation's own entry is the only place that can be written.
	 * The stub runs into the shared method's thunk rather than into the body
	 * behind it, so a later compile of the shared method reaches this
	 * instantiation through the redirect every other caller goes through.
	 */
	if (mono_method_needs_static_rgctx_invoke (shared, TRUE)) {
		llvm::Expected<void *> keyed =
			context_stub (domain, dm, (*owner)->thunk_address ());

		if (!keyed)
			return keyed.takeError ();

		entry = *keyed;
	}

	dm.publish (tier, entry);
	dm.attach_body (tier, entry, nullptr);

	/*
	 * The symbols rather than the printed names. mono_type_get_desc () writes a
	 * shared reference parameter as object, so the shared form and the genuine
	 * <object> instantiation have the same full name and the line could not say
	 * which of them it is about. A symbol ends in the MonoMethod address.
	 */
	if (is_jit_trace_enabled ())
		MONO_LOCK (jit_trace_mutex ())
		{
			llvm::errs () << "[llvm-jit] " << dm.name << " shares the body of "
				      << (*owner)->name << "\n";
		}

	/*
	 * The instantiation has an entry now, and this is the only place that says
	 * so: the notification the compile path sends names the method a body was
	 * built for, which here is the shared form. The interpreter keeps nothing
	 * for that one - it runs the instantiation - so without this an interpreted
	 * caller goes on interpreting a method it could call.
	 */
	if (mono_use_interpreter)
		mini_get_interp_callbacks ()->method_compiled (domain.domain, dm.method);

	return Compiled { entry, nullptr };
}

llvm::Expected<void *>
MonoBackend::context_stub (DomainState &domain, MonoDomainMethod &dm, void *target)
{
	MethodState &engine = engine_state (dm);

	MONO_LOCK (mutex_)
	{
		if (engine.context != nullptr)
			return engine.context;
	}

	/*
	 * The context of this one instantiation: its class vtable, or the MRGCTX
	 * that also carries the method's own type arguments. Reading it lays the
	 * class out and creates the vtable, which is metadata work the compile has
	 * already done - it never runs a class initializer.
	 */
	void *context = mini_method_get_rgctx (dm.method);
	llvm::Expected<char *> at =
		domain.code.reserve (arch::context_stub_size, arch::context_stub_align);

	if (!at)
		return at.takeError ();

	arch::write_context_stub (*at, context, target);
	llvm::sys::Memory::InvalidateInstructionCache (*at, arch::context_stub_size);

	MonoJitInfo *jinfo = register_code_stub (*at, arch::context_stub_size,
	                                         "context stub", domain.domain, dm.method);

	MONO_LOCK (mutex_)
	{
		if (jinfo != nullptr)
			engine.owned.jinfos.push_back (jinfo);
		engine.context = *at;
	}

	return (void *) *at;
}

llvm::Expected<MonoBackend::Compiled>
MonoBackend::compile_body (DomainState &domain, MonoDomainMethod &dm, bool allow_tier0,
                           MonoTier tier, bool for_sharing)
{
	if (allow_tier0) {
		llvm::Expected<Compiled> interpreted = tier0_entry (domain, dm);

		if (interpreted)
			return *interpreted;

		/*
		 * The interpreter refusing the method is not a failure: the method
		 * gets compiled like anything else.
		 */
		llvm::consumeError (interpreted.takeError ());
	}

	// Sharing is decided below, per member, so this route and promotion get the
	// same answer for the same method.
	std::vector<llvm::Expected<Compiled>> compiled =
		compile_bodies (domain, &dm, tier, for_sharing);

	return std::move (compiled.front ());
}

namespace {

/// One method's share of a batched compile: the callbacks a translation needs,
/// and the storage a TranslationTarget's function_refs point into.
struct Member {
	MonoDomainMethod *dm;

	/// What MonoDomainMethod::folds_epoch () gave before the translation. The
	/// publication is refused when it has moved since.
	uint32_t folds_epoch = 0;

	std::vector<uint8_t> profile;
	std::function<llvm::Expected<MonoDomainMethod *> (MonoMethod *)> publish_callee;
	std::function<void (const CompiledMethod &, MonoJitInfo *)> note;
	std::function<llvm::Expected<Compiled> (llvm::Error)> recover;
};

} // namespace

bool
MonoBackend::answered_by_sharing (llvm::Expected<Compiled> &result)
{
	return (bool) result || !result.errorIsA<SharingRefusal> ();
}

std::vector<llvm::Expected<MonoBackend::Compiled>>
MonoBackend::compile_bodies (DomainState &domain, llvm::ArrayRef<MonoDomainMethod *> dms,
                             MonoTier tier, bool for_sharing)
{
	/*
	 * Materialize as the domain the code is for, not as whatever the calling
	 * thread happens to be running as: a thunk fires under the thread's current
	 * domain, and AppDomain:InvokeInDomain switches that before calling. An
	 * address baked in from the wrong domain is a pointer into another domain's
	 * vtables and statics, live until that domain unloads under it.
	 */
	DomainScope entered (domain.domain);

	JitTier pipeline = tier == MonoTier::tier2 ? JitTier::tier2 : JitTier::tier1;
	std::vector<size_t> taken;
	std::map<size_t, llvm::Expected<Compiled>> settled;

	for (size_t i = 0; i < dms.size (); ++i) {
		MonoDomainMethod *dm = dms[i];

		/*
		 * Before anything is translated, so a method gets the same verdict
		 * whichever tier ends up running it: a body the verifier rejects is
		 * one no tier may run, and one it accepts is one every tier may.
		 */
		if (llvm::Error invalid = verify_method (dm->method)) {
			settled.emplace (i, std::move (invalid));
			continue;
		}

		/*
		 * At every tier. Which instantiations one body serves is observable -
		 * a detour on the shared form moves all of them - so it must not
		 * depend on how far a method happens to have promoted.
		 *
		 * The shared method is open, and shared_form () refuses an open
		 * method, so the compile enter_shared_body () asks for arrives here
		 * and stops.
		 */
		MonoMethod *shared = shared_form (dm->method);

		if (shared == nullptr) {
			taken.push_back (i);
			continue;
		}

		llvm::Expected<Compiled> body = enter_shared_body (domain, *dm, shared, tier);

		if (answered_by_sharing (body)) {
			settled.emplace (i, std::move (body));
			continue;
		}

		// Refused, so the method is compiled against its own instantiation.
		if (is_jit_trace_enabled ()) {
			std::string why = llvm::toString (body.takeError ());

			MONO_LOCK (jit_trace_mutex ())
			{
				llvm::errs () << "[llvm-jit] not sharing " << dm->name << ": "
					      << why << "\n";
			}
		} else {
			llvm::consumeError (body.takeError ());
		}

		taken.push_back (i);
	}

	auto answer = [&] (std::vector<llvm::Expected<Compiled>> compiled) {
		std::vector<llvm::Expected<Compiled>> out;
		size_t next = 0;

		out.reserve (dms.size ());
		for (size_t i = 0; i < dms.size (); ++i) {
			auto left = settled.find (i);

			if (left != settled.end ())
				out.push_back (std::move (left->second));
			else
				out.push_back (std::move (compiled[next++]));
		}
		return out;
	};

	if (taken.empty ())
		return answer ({});

	std::vector<std::unique_ptr<Member>> members;
	std::vector<TranslationTarget> targets;
	std::vector<const TranslationTarget *> handles;
	std::vector<MonoMethod *> methods;

	// The handles below point into this, so it must not move under them.
	targets.reserve (taken.size ());

	for (size_t i : taken) {
		MonoDomainMethod *dm = dms[i];
		MethodState &engine = engine_state (*dm);
		auto member = std::make_unique<Member> ();

		member->dm = dm;
		member->folds_epoch = dm->folds_epoch ();
		member->publish_callee = [this, &domain] (MonoMethod *callee) {
			return publish (domain, callee);
		};
		/*
		 * Only a dynamic method is ever freed, and only its own compiles have
		 * to be taken back out again - everything else dies with the domain.
		 */
		member->note = [this, dm, &engine] (const CompiledMethod &compiled,
		                                    MonoJitInfo *jinfo) {
			if (compiled.profile)
				MONO_LOCK (mutex_) { engine.profile = compiled.profile; }

			if (!dm->method->dynamic)
				return;

			MONO_LOCK (mutex_)
			{
				if (compiled.dylib != nullptr)
					engine.owned.dylibs.push_back (compiled.dylib);
				if (jinfo != nullptr)
					engine.owned.jinfos.push_back (jinfo);
			}
		};

		Member *held = member.get ();

		member->recover = [this, &domain, held, for_sharing] (llvm::Error failure)
			-> llvm::Expected<Compiled> {
			/*
			 * A stand-in that raises is the answer for a method the program
			 * has to be told about. A shared body is not that method: every
			 * instantiation behind it would raise, and the failure may well
			 * be the shared form's own. So the refusal goes back to the
			 * caller, which compiles the instantiation that was asked for
			 * and finds out for itself.
			 */
			if (for_sharing)
				return llvm::make_error<SharingRefusal> (
					llvm::toString (std::move (failure)));

			return recover (*domain.jit, domain.domain, held->dm->method,
			                std::move (failure), held->note);
		};

		if (pipeline == JitTier::tier2) {
			MONO_LOCK (mutex_)
			{
				if (engine.profile)
					member->profile = build_profile (*engine.profile);
			}

			if (member->profile.empty () && is_jit_trace_enabled ())
				MONO_LOCK (jit_trace_mutex ())
				{
					llvm::errs ()
						<< "[llvm-jit] no profile for a tier-2 compile of "
						<< dm->name << "\n";
				}
		}

		targets.push_back (TranslationTarget { domain.jit.get (), domain.domain,
			                               member->publish_callee, member->note,
			                               member->recover, pipeline,
			                               member->profile });
		methods.push_back (dm->method);
		members.push_back (std::move (member));
	}

	for (const TranslationTarget &target : targets)
		handles.push_back (&target);

	std::vector<BatchResult> results = [&] {
		timing::Scope timed (timing::Phase::compile);

		return translate_and_compile_batch (handles, methods);
	}();

	// A compile has now run, so the tables LLVM builds inside one exist and the
	// exit teardown can be ordered ahead of them.
	register_exit_teardown ();

	std::vector<llvm::Expected<Compiled>> compiled;

	compiled.reserve (taken.size ());

	for (size_t k = 0; k < taken.size (); ++k) {
		size_t i = taken[k];
		BatchResult &result = results[k];

		if (!result.code) {
			compiled.push_back (result.code.takeError ());
			continue;
		}

		/*
		 * Before the redirects below, which are what make the body reachable. A
		 * body that goes live carrying none of the breakpoints already set on
		 * the method is a breakpoint that stops being hit the moment the method
		 * is compiled again, with nothing said about it.
		 */
		if (result.published != nullptr)
			mini_install_pending_breakpoints (domain.domain,
			                                  jinfo_get_method (result.published),
			                                  result.published);

		/*
		 * A method this body folded in was replaced while it compiled, so the
		 * body holds a copy of IL that is gone. The record has taken the entry
		 * back to its lazy resolver already, and the body is left where it is:
		 * no caller can reach code that was never published.
		 */
		if (!dms[i]->publish (tier, result.code->body, members[k]->folds_epoch)
		    && dms[i]->folds_epoch () != members[k]->folds_epoch) {
			compiled.push_back (llvm::make_error<StaleFold> (
				"a method the body folded in was replaced while it compiled"));
			continue;
		}

		dms[i]->attach_body (tier, result.code->body, result.code->jinfo);

		/*
		 * Only now, and outside the lock: the debugger agent's handler for this
		 * parks the compiling thread and lets its own thread look the method up,
		 * through a door that takes the same lock.
		 */
		if (result.published != nullptr)
			raise_jit_done (jinfo_get_method (result.published), result.published);

		/*
		 * A method the interpreter is already running calls its callees by
		 * interpreting them, and has no other way of noticing that one of them
		 * has since been given code to call instead.
		 */
		if (mono_use_interpreter)
			mini_get_interp_callbacks ()->method_compiled (domain.domain,
			                                               dms[i]->method);

		compiled.push_back (*result.code);
	}

	return answer (std::move (compiled));
}

/// Whether a promotion of method at tier can share a compile with other methods.
static bool
allow_batched_compile (MonoMethod *method, MonoTier tier)
{
	/*
	 * A tier-2 compile is laid out by its own method's counts, so it has no
	 * module to share. It is also asked for by a method that is already hot,
	 * which is the worst thing to make wait behind a batch of others.
	 *
	 * A dynamic method is the one thing that gets freed, and drop () has to
	 * take its queued work with it. Batched work compiles methods its tag says
	 * nothing about, so a dynamic method keeps the one-tag-one-compile shape
	 * drop () needs.
	 */
	return tier == MonoTier::tier1 && !method->dynamic;
}

/*
 * Runs on a mutator thread that just used up a promotion counter - the
 * interpreter's for tier 0, a tier-1 body's own instrumentation for tier 2 -
 * so it hands everything it can to the compile queue. Nothing waits for the
 * result, and a compile that fails once it is running leaves the method at
 * the tier it is at.
 */
bool
MonoBackend::request_promotion (MonoMethod *method, MonoDomain *domain, MonoTier tier)
{
	llvm::Expected<MonoBackend *> backend = get ();

	if (!backend) {
		llvm::consumeError (backend.takeError ());
		return false;
	}

	/* Held rather than read again on the worker thread: at exit the engine is
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

	if (!allow_batched_compile (method, tier))
		return owner->queue.enqueue (method, [self, owner, method, tier] () {
			llvm::Expected<MonoDomainMethod *> published =
				publish (*owner, method);

			if (!published) {
				llvm::logAllUnhandledErrors (
					published.takeError (), llvm::errs (),
					"mono: could not publish a promoted method: ");
				return;
			}

			MonoDomainMethod *one = *published;

			for (llvm::Expected<Compiled> &body :
			     self->compile_bodies (*owner, one, tier))
				if (!body)
					llvm::logAllUnhandledErrors (
						body.takeError (), llvm::errs (),
						"mono: could not promote a method: ");
		});

	/*
	 * Queued before the work that drains it, so the request cannot be taken by
	 * a drainer that ran before it arrived. A request the queue then refuses
	 * leaves the method waiting until the next one drains it, or until the
	 * domain goes - which is what a refused promotion already means.
	 */
	MONO_LOCK (owner->pending_mutex) { owner->pending.push_back (method); }

	/*
	 * One piece of work per request, and each takes as many pending promotions
	 * as it may. So the ones that follow a batch find nothing waiting and
	 * retire immediately.
	 */
	return owner->queue.enqueue (method, [self, owner] () {
		std::vector<MonoMethod *> methods =
			owner->take_pending (promotion_batch_size ());
		std::vector<MonoDomainMethod *> records;

		for (MonoMethod *method : methods) {
			/* The interpreter reaches a callee without the backend being
			 * asked for it, so there may be no state for this method yet. */
			llvm::Expected<MonoDomainMethod *> published =
				publish (*owner, method);

			if (!published) {
				llvm::logAllUnhandledErrors (
					published.takeError (), llvm::errs (),
					"mono: could not publish a promoted method: ");
				continue;
			}

			records.push_back (*published);
		}

		if (records.empty ())
			return;

		for (llvm::Expected<Compiled> &body :
		     self->compile_bodies (*owner, records, MonoTier::tier1)) {
			if (body)
				continue;

			/* Not a failed promotion. The method's entry is on its lazy
			 * resolver and the next call compiles it again. */
			if (body.errorIsA<StaleFold> ()) {
				llvm::consumeError (body.takeError ());
				continue;
			}

			llvm::logAllUnhandledErrors (body.takeError (), llvm::errs (),
			                             "mono: could not promote a method: ");
		}
	});
}

/*
 * On the calling thread rather than a worker, which is what makes it
 * synchronous. That is also why it is safe: a mutator compiling a method is what
 * every ordinary compile does, and the rule the queue exists to keep - that no
 * thread waits on a background compile - is kept by not using the queue at all.
 */
bool
MonoBackend::promote_now (MonoMethod *method, MonoDomain *domain, MonoTier tier)
{
	llvm::Expected<MonoBackend *> backend = get ();

	if (!backend) {
		llvm::consumeError (backend.takeError ());
		return false;
	}

	MonoBackend *self = *backend;

	if (self == nullptr)
		return false;

	llvm::Expected<DomainState *> state = self->state (domain);

	if (!state) {
		llvm::consumeError (state.takeError ());
		return false;
	}

	llvm::Expected<MonoDomainMethod *> published = publish (**state, method);

	if (!published) {
		llvm::logAllUnhandledErrors (published.takeError (), llvm::errs (),
		                             "mono: could not publish a promoted method: ");
		return false;
	}

	llvm::Expected<Compiled> body =
		self->compile_body (**state, **published, /*allow_tier0=*/false, tier);

	if (!body) {
		llvm::logAllUnhandledErrors (body.takeError (), llvm::errs (),
		                             "mono: could not promote a method: ");
		return false;
	}

	return true;
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
MonoBackend::rearm_trampoline (MonoDomain *domain, void *trampoline)
{
	if (!instance)
		return;

	/*
	 * The lock spans the rearm rather than a lookup of the callbacks:
	 * releasing a domain takes its callbacks apart, and the trampoline belongs
	 * to that domain.
	 */
	MONO_LOCK (instance->mutex_)
	{
		auto it = instance->domains_.find (domain);

		if (it != instance->domains_.end ())
			it->second->callbacks->rearm (trampoline);
	}
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
	 * An interpreted method gets null here, as if it had no body. What it has
	 * is the entry thunk, which every interpreted method shares, so handing it
	 * back names no method in particular. A caller takes it for a body: it
	 * looks up the jit info by address. The interpreter reads that as "this
	 * has native code now" and starts calling through the native boundary
	 * instead of interpreting the method.
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
	 * Compiled here rather than on the first call through the thunk, for two
	 * reasons. The first: a refusal can come back through MonoError and be
	 * raised by the runtime, which knows how to throw from where it stands.
	 * The second, and this one is not an optimisation. The caller may hand
	 * the address to native code that calls it from a thread mono has never
	 * seen. There is no domain there to compile against, and no thread info
	 * to read one from.
	 */
	if (llvm::Error err = entry_point (**state, **published).takeError ())
		return std::move (err);

	return published_entry (**published);
}

/*
 * The wrapper is resolved here rather than in attach_interop (), which runs
 * under the record's lock. Compiling the wrapper translates its body, the
 * trivial inliner folds this method into it, and note_folded_into () then wants
 * that same lock. Nothing is cached on this record: the marshalling layer
 * caches the wrapper and the wrapper's own record caches its entry, so the
 * address is the same on every ask.
 */
llvm::Expected<void *>
MonoBackend::published_entry (MonoDomainMethod &dm)
{
	if (!publishes_interop_entry (dm.method))
		return dm.thunk.code ();

	if (!mono_method_is_unmanaged_callers_only (dm.method))
		return dm.interop_entry ();

	llvm::Expected<MonoBackend *> backend = get ();

	if (!backend)
		return backend.takeError ();
	if (*backend == nullptr)
		return llvm::createStringError (llvm::inconvertibleErrorCode (),
		                                "the engine has been taken apart");

	ERROR_DECL (metadata_error);
	MonoMethod *wrapper = mono_marshal_get_managed_wrapper (
		dm.method, nullptr, (MonoGCHandle) 0, metadata_error);

	if (wrapper == nullptr)
		return runtime_error (metadata_error);

	return (*backend)->compile (wrapper, dm.domain);
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

	/*
	 * The same answer compile () gives. A method has one address, so the
	 * engine that asks must not decide which one it gets: the interpreter
	 * reaches here for its ldftn and the icall reaches compile ().
	 */
	return published_entry (**published);
}

} // namespace mono

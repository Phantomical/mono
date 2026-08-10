/**
 * \file
 * \brief Compiling a MonoMethod through the LLVM-only backend.
 *
 * The runtime asks for a method's code; this translates the IL, resolves what
 * the resulting module refers to, compiles it and hands back the method's stub.
 *
 * Two things arrive from here that the engine underneath cannot work out for
 * itself. The runtime addresses a module refers to - classes, vtables, static
 * blocks, MonoMethods - are resolved from what the translator recorded as it
 * emitted them. And the methods it calls are published as stubs that compile
 * themselves when first called, so compiling one method never drags in the
 * transitive closure of everything it might call.
 */

/*
 * Before runtime.hpp, so that MonoError is the internal struct the rest of the
 * runtime passes around rather than the opaque public one.
 */
#include "runtime-error.hpp"

#include "runtime.hpp"

#include "arch/arch.hpp"
#include "callbacks.hpp"
#include "codemem.hpp"
#include "compile-queue.hpp"
#include "compile-worker.hpp"
#include "interp-entry.hpp"
#include "jinfo.hpp"
#include "jit.hpp"
#include "stubs.hpp"
#include "timing.hpp"
#include "method-to-llvm.hpp"
#include "runtime-legacy.hpp"
#include "runtime/engine.hpp"
#include "runtime/dispatcher.hpp"
#include "runtime/tiering.hpp"
#include "runtime/externals.hpp"
#include "runtime/interp.hpp"
#include "runtime/thrower.hpp"
#include "runtime/translate.hpp"
#include "runtime/builtins.hpp"
#include "runtime/minimal-compile.hpp"
#include "runtime/naming.hpp"
#include "runtime/options.hpp"
#include "runtime/stub-jinfo.hpp"
#include "verification.hpp"

#include "mini.h"
#include "mini-llvm.h"
#include "mini-runtime.h"

/*
 * The icalls generated code calls are declared without extern "C" unless the
 * runtime was configured to export them, and they are all defined in C.
 */
extern "C" {
#include "jit-icalls.h"
}

#include "mono/metadata/class-internals.h"
#include "mono/metadata/debug-helpers.h"
#include "mono/metadata/marshal.h"
#include "mono/metadata/object-internals.h"
#include "mono/utils/mono-error-internals.h"
#include "mono/utils/mono-tls-inline.h"

// This breaks some LLVM headers
#undef PIC

#include <llvm/ADT/Twine.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/ErrorHandling.h>
#include <llvm/TargetParser/Triple.h>

#include <chrono>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using namespace llvm;
using namespace llvm::orc;

namespace mono {
namespace {

/*
 * Where a stub lands when the compile behind it failed. The trampoline has
 * already put the call's arguments back and jumped here, so this is running as
 * the method the caller asked for: there is no value it could return and no
 * caller that would know what to do with one.
 *
 * Printed and left by hand rather than through report_fatal_error, which ends in
 * exit () when told not to produce a crash diagnostic - and exit () runs the
 * static destructors of every C++ library in the process while the threads that
 * are still compiling are using what those destructors free.
 */
[[noreturn]] void
lazy_compile_failed ()
{
	static const char msg[] = "LLVM ERROR: a method failed to compile on first call\n";
	[[maybe_unused]] ssize_t written = write (2, msg, sizeof (msg) - 1);
	fflush (nullptr);
	_exit (1);
}

/*
 * One domain's stubs, and the names they are published to the linker under.
 *
 * This is what MonoBackend does in MethodState, gathered behind the API this
 * file was written against instead: stubs are a backend concern and MonoJit only
 * hears the names. It goes when the compile path moves to runtime/backend.cpp
 * and this file with it.
 */
class StubPublisher {
public:
	using LazyCompileFunction = unique_function<Expected<void *> ()>;

	StubPublisher (MonoJit &jit, CodeSlabs &slabs)
	    : jit_ (&jit), slabs_ (&slabs), table_ (&slabs)
	{
	}

	static Expected<std::unique_ptr<StubPublisher>> create (MonoJit &jit, CodeSlabs &slabs)
	{
		auto self = std::make_unique<StubPublisher> (jit, slabs);
		auto callbacks = LazyCallbacks::create ((void *) &lazy_compile_failed);

		if (!callbacks)
			return callbacks.takeError ();
		self->callbacks_ = std::move (*callbacks);
		return self;
	}

	Error create_stub (StringRef name, void *target)
	{
		Expected<Stub> stub = carve (name, nullptr);

		if (!stub)
			return stub.takeError ();
		stub->redirect (target);
		return Error::success ();
	}

	Error create_lazy_stub (StringRef name, LazyCompileFunction compile,
	                        void *key = nullptr)
	{
		std::string owned = name.str ();
		Expected<void *> trampoline = reserve (
			[this, owned, compile = std::move (compile)] () mutable -> void * {
				Expected<void *> code = compile ();

				if (!code) {
					logAllUnhandledErrors (code.takeError (), errs (),
					                       "mono: ");
					return (void *) &lazy_compile_failed;
				}

				if (std::optional<Stub> stub = table_.find (owned))
					stub->redirect (*code);
				return *code;
			});
		if (!trampoline)
			return trampoline.takeError ();

		Expected<Stub> stub = carve (name, key);

		if (!stub) {
			callbacks_->release (*trampoline);
			return stub.takeError ();
		}

		stub->redirect (*trampoline);
		std::lock_guard<std::mutex> lock (mutex_);
		trampolines_[name] = *trampoline;
		return Error::success ();
	}

	Expected<void *> create_lazy_entry (StringRef name, LazyCompileFunction compile)
	{
		Expected<void *> trampoline =
			reserve ([this, compile = std::move (compile)] () mutable -> void * {
				Expected<void *> landing = compile ();

				if (landing)
					return *landing;

				logAllUnhandledErrors (landing.takeError (), errs (), "mono: ");
				return (void *) &lazy_compile_failed;
			});
		if (!trampoline)
			return trampoline;

		std::lock_guard<std::mutex> lock (mutex_);
		trampolines_[name] = *trampoline;
		return trampoline;
	}

	Expected<std::vector<void *>>
	create_counter_thunks (uint32_t threshold, ArrayRef<std::pair<void *, void *>> entries)
	{
		return mono::create_counter_thunks (*slabs_, threshold, entries);
	}

	Error redirect_stub (StringRef name, void *target)
	{
		std::optional<Stub> stub = table_.find (name);

		if (!stub)
			return createStringError (inconvertibleErrorCode (),
			                          "no stub was published for %s",
			                          name.str ().c_str ());
		stub->redirect (target);
		return Error::success ();
	}

	Expected<void *> stub_address (StringRef name)
	{
		std::optional<Stub> stub = table_.find (name);

		if (!stub)
			return createStringError (inconvertibleErrorCode (),
			                          "no stub was published for %s",
			                          name.str ().c_str ());
		return stub->code ();
	}

	Error undefine_stubs (ArrayRef<std::string> names)
	{
		if (Error err = jit_->undefine_stubs (names))
			return err;

		{
			std::lock_guard<std::mutex> lock (mutex_);

			for (const std::string &name : names) {
				auto it = trampolines_.find (name);

				if (it == trampolines_.end ())
					continue;
				callbacks_->release (it->second);
				trampolines_.erase (it);
			}
		}

		table_.remove_all (names);
		return Error::success ();
	}

private:
	Expected<Stub> carve (StringRef name, void *key)
	{
		Expected<Stub> stub = key != nullptr ? table_.create (name, key) : table_.create (name);

		if (!stub)
			return stub;

		std::pair<StringRef, void *> def{name, stub->code ()};

		if (Error err = jit_->define_stubs (def))
			return std::move (err);
		return stub;
	}

	Expected<void *> reserve (LazyCompile compile)
	{
		return callbacks_->reserve (std::move (compile));
	}

	MonoJit *jit_;
	CodeSlabs *slabs_;
	StubTable table_;
	std::unique_ptr<LazyCallbacks> callbacks_;
	std::mutex mutex_;
	StringMap<void *> trampolines_;
};


/// The engine, and everything it needs that outlives one compile.
///
/// Code is compiled per domain, the way mini kept a jit_code_hash per domain: a
/// compiled body bakes in addresses that belong to one domain - its vtables,
/// its statics blocks, its interned strings - so a body is only correct in the
/// domain it was compiled against. Each domain therefore gets a linker of its
/// own, a whole MonoJit: its symbols never meet another domain's, and unloading
/// the domain tears the linker down, stubs, code and all.
class Backend {
public:
	static Expected<Backend *> get ();

	/// Compile METHOD into TARGET_DOMAIN's linker now and return the address
	/// of its stub there.
	Expected<void *> compile (MonoMethod *method, MonoDomain *target_domain);

	/// Take the background compile worker down, waiting for the compile it is
	/// running. Nothing is queued after this.
	static void stop_compiling ();

	/// Refuse further background compiles for DOMAIN and wait for the one in
	/// flight for it. Runs while the domain is still whole; free_domain () is
	/// what takes it apart afterwards.
	static void stop_compiling_for (MonoDomain *domain);

	/// Drop DOMAIN's linker and everything in it. The caller proves the code
	/// dead: nothing may be executing in, or about to call into, the domain.
	static void free_domain (MonoDomain *domain);

	/// Drop everything the backend holds for METHOD, in every domain it was
	/// compiled into: its code, its jit-info records, and every cache entry
	/// keyed by it. The caller proves the code dead, the way free_domain ()
	/// does.
	///
	/// Only dynamic methods are ever freed, and freeing one hands its
	/// MonoMethod straight back to the allocator, so this is not housekeeping
	/// that can be deferred: an entry left keyed by that address is found again
	/// by whatever method lands there next.
	static void free_method (MonoMethod *method);

	/// Where METHOD's body starts in DOMAIN, or null when this backend has not
	/// compiled it there.
	static void *body_of (MonoDomain *domain, MonoMethod *method);

	/// Call VISIT once for each live body this backend compiled METHOD into in
	/// DOMAIN, oldest first.
	static void foreach_body (MonoDomain *domain, MonoMethod *method,
	                          void (*visit) (MonoJitInfo *, void *),
	                          void *user_data);

	/// METHOD's unboxing entry in the current domain, or null when it has
	/// none - a method not implemented in IL is entered through code this
	/// backend did not generate.
	static void *unbox_entry_of (MonoMethod *method);


private:
	using Compiled = mono::Compiled;

	/// What publishing a method handed the rest of the runtime: the legacy
	/// stub's address, and the trampoline jit-info record each of the method's
	/// two stubs was registered under. A record is only held here when it has
	/// to be taken out again by hand - see register_stub_jinfo ().
	struct Publication {
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

	/// One domain's whole compilation state.
	struct DomainState {
		/// The domain this state compiles for. Everything materialized into
		/// the linker resolves against this domain - never against the
		/// thread's current domain, which can point elsewhere while a stub
		/// fires (AppDomain:InvokeInDomain switches the domain and then calls).
		MonoDomain *domain;
		/// The code memory the linker and the stubs both come out of.
		std::shared_ptr<CodeSlabs> slabs;
		std::unique_ptr<MonoJit> jit;
		/// This domain's stubs, and the names the linker knows them by.
		std::unique_ptr<StubPublisher> stubs;
		/// This domain's share of the background compile queue.
		///
		/// Queued work holds a raw pointer to this state, which is sound
		/// because closing the channel is what free_domain () does before
		/// destroying it: after that nothing queued for the domain will run
		/// and nothing running for it still is. The channel being a member is
		/// what ties the two together - the state cannot be destroyed without
		/// it being closed.
		std::optional<CompileQueue::Channel> queue;
		/// Methods whose stubs are defined in the linker, so a callee reached
		/// from several places is only given its stubs once. Defined is all a
		/// caller's module needs to link.
		std::unordered_set<MonoMethod *> defined;
		/// Methods already published: stubs defined, address in hand, tramp
		/// info registered.
		std::unordered_map<MonoMethod *, Publication> published;
		/// Methods whose stubs already point at real code - and where that
		/// code is, so that asking again is a lookup rather than a compile.
		std::unordered_map<MonoMethod *, Compiled> compiled;
		/// Bodies a method had before the one above, in publication order. The
		/// stubs no longer name them, but a thread already running in one is
		/// still running in it, so they stay live and anything that has to
		/// cover every body of a method - the debugger placing a breakpoint -
		/// has to see them. Only a method compiled more than once has an entry.
		std::unordered_map<MonoMethod *, std::vector<MonoJitInfo *>> superseded;
		/// Methods whose body stub was first reached from another domain, and
		/// the per-call dispatcher each got instead of a direct binding.
		std::unordered_map<MonoMethod *, void *> dispatchers;
		/// Methods entered by interpreting them, and the entries their stubs
		/// were pointed at. Separate from compiled above, which is what
		/// everything asking where a method's *code* is reads.
		std::unordered_map<MonoMethod *, Compiled> interpreted;
		/// Methods whose call counter has already asked for a tier-1 compile.
		/// A method has one counter but an entry thunk per door, so this is
		/// what keeps the request to one however it was reached.
		std::unordered_set<MonoMethod *> promoting;
		/// What each dynamic method's compiles produced, for free_method ().
		std::unordered_map<MonoMethod *, Owned> owned;
	};

	/// The current domain's state, created - linker, helpers and all - the
	/// first time the domain compiles something.
	Expected<DomainState *> state ();

	/// DOMAIN's state, created on first use like state ().
	Expected<DomainState *> state_for (MonoDomain *domain);

	Error resolve (DomainState &state, const std::vector<ExternalSymbol> &externals);
	Expected<Compiled> translate_and_compile (DomainState &state, MonoMethod *method,
	                                          MonoJitInfo **published);

	/// Translate METHOD's IL and compile what comes out, storing the jit info
	/// of the body in PUBLISHED. PUBLISHED is left null when a metadata
	/// failure was turned into a stand-in body instead of a real one.
	Expected<Compiled> translate_body (DomainState &state, MonoMethod *method,
	                                   MonoJitInfo **published);

	/// Note that COMPILED, and the jit-info record JINFO registered for it, were
	/// produced for METHOD, so that free_method () can undo both. Either may be
	/// absent - a dispatcher carries no jit info.
	///
	/// Every compile of a method has to pass through here, or freeing the method
	/// leaves that compile's code and record behind for good.
	void remember (DomainState &state, MonoMethod *method,
	               const CompiledMethod &compiled, MonoJitInfo *jinfo);

	/// Decide what a failed translation of METHOD means. A metadata failure is
	/// something the program is owed as an exception rather than a method that
	/// would not compile, and becomes a stand-in body that raises it; anything
	/// else is handed straight back, unchanged.
	Expected<Compiled> recover (DomainState &state, MonoMethod *method, Error failure);

	/// Turn FAILURE into a body for METHOD that raises it, whatever the
	/// failure was: what a call already under way gets instead of an answer.
	/// Consumes FAILURE.
	Expected<Compiled> raise_on_call (DomainState &state, MonoMethod *method,
	                                  Error failure);

	/// Make METHOD reachable through its stubs in STATE, and say where each of
	/// them now goes. That is a compile for most methods and a set of entries
	/// into the interpreter for the ones running at tier 0. AGAIN skips the
	/// cache, so the method is translated into a body of its own however many
	/// it already has.
	Expected<Compiled> ensure_entries (DomainState &state, MonoMethod *method,
	                                   bool again = false);

	/// Point a method's stubs at the interpreter.
	Expected<Compiled> interp_entries (DomainState &state, MonoMethod *method);

	/// How a call to a method is taken apart for the interpreter, worked out on
	/// first use and then shared with every method of the same prototype.
	///


	/// Compile METHOD into STATE's linker and point both of its stubs at what
	/// comes out, whether or not it has been compiled there already.
	///
	/// Recording the result is what makes the new code the answer everything
	/// downstream gives for the method; the redirects are what make callers
	/// that were compiled long ago reach it.
	///
	/// TIER1 asks for a compile even where the routing would have interpreted
	/// the method, which is what promoting one means.
	Expected<Compiled> compile_and_publish (DomainState &state, MonoMethod *method,
	                                        bool tier1 = false);

	/// Ask for METHOD, which is being interpreted in STATE, to be compiled on
	/// the background worker and its stubs redirected to the result.
	///
	/// Called from the method's own call counter, on the thread that made the
	/// call that reached the threshold, and at most once for the method.
	void request_promotion (DomainState &state, MonoMethod *method);

	/// The entries an interpreted METHOD is reached through, each behind a
	/// thunk counting calls towards a compile. ENTRIES is what the stubs would
	/// have been pointed at without one; the addresses returned replace them.
	Expected<std::vector<void *>> counted_entries (DomainState &state,
	                                               MonoMethod *method,
	                                               ArrayRef<void *> entries);

	/// Ask for METHOD to be compiled again on the background worker, its stubs
	/// redirected to the second body when it is done. Returns quietly when the
	/// work is refused - see CompileQueue on why nothing retries.
	void enqueue_recompile (DomainState &state, MonoMethod *method);

	/// The legacy entry - the interop thunk - as a compile of its own, with
	/// no body beside it. It reaches the method through the body's stub, so
	/// it is domain-neutral and safe to build whichever domain the building
	/// thread has current; a method compiled for its own domain gets its
	/// entry emitted into the body's module instead.
	Expected<void *> compile_entry_thunk (DomainState &state, MonoMethod *method);

	/// The per-call dispatcher a body stub binds to when its first caller
	/// arrived from another domain: resolves the current domain's body on
	/// every call instead of baking one domain's copy into another's code.
	Expected<void *> dispatcher (DomainState &state, MonoMethod *method);

	/// The runtime helper behind the dispatcher: the current domain's compiled
	/// body for METHOD, compiling it now if this domain has not yet.
	static void *body_for_current_domain (MonoMethod *method);

	/// Define METHOD's two stubs in STATE's linker, without materializing
	/// either. This is all resolve () needs for a callee: the caller's module
	/// links against the names. Cheap and lock-safe - definitions never run
	/// anyone else's compile.
	Error publish_defs (DomainState &state, MonoMethod *method);

	/// The address to call METHOD at, compiling it on the first call rather
	/// than now. This is how a method's callees are published: reaching one is
	/// what says it is worth compiling. Defines both of the method's stubs and
	/// returns the legacy one.
	Expected<void *> publish (DomainState &state, MonoMethod *method);

	std::mutex mutex_;
	std::unordered_map<MonoDomain *, std::unique_ptr<DomainState>> domains_;

	/*
	 * The interpreter entries, under a lock of their own because every call
	 * into an interpreted method reads them and the compiler must never be
	 * what such a call is waiting behind.
	 *
	 * A layout depends only on the prototype, so the layouts are shared across
	 * domains; an entry pairs one with the interpreter's own record, which is
	 * per domain. Both maps are node-based, so an entry handed out stays put as
	 * later ones are added.
	 */
	/// Where compiles that nobody is waiting for run. Declared last so that it
	/// is torn down first: its worker takes mutex_ and holds domain states.
	CompileQueue queue_ { std::make_unique<CompileWorker> () };
};

/// The one Backend, once get () has made it. Kept at file scope so that
/// free_domain () can decline quietly when nothing was ever compiled.
Backend *live_backend = nullptr;

Expected<Backend *>
Backend::get ()
{
	static std::once_flag once;

	std::call_once (once, [] {
		claim_engine (EngineKind::legacy);
		live_backend = new Backend ();

		/*
		 * The ordered stop is mini_cleanup ()'s, and this is not a substitute
		 * for it - by the time exit () runs, a domain has long since been torn
		 * down out from under anything still compiling. It is the backstop for
		 * a process that never shuts the runtime down at all, which the unit
		 * test binaries do not and an embedder need not: a worker still
		 * compiling while the process unwinds reads whatever has already been
		 * destroyed. Registered here rather than at the first background
		 * compile so it lands after every static constructor and therefore runs
		 * before every static destructor.
		 */
		atexit ([] { Backend::stop_compiling (); });
	});
	return live_backend;
}

Expected<Backend::DomainState *>
Backend::state ()
{
	return state_for (mono_domain_get ());
}

Expected<Backend::DomainState *>
Backend::state_for (MonoDomain *domain)
{
	std::lock_guard<std::mutex> lock (mutex_);

	auto it = domains_.find (domain);
	if (it != domains_.end ())
		return it->second.get ();

	std::shared_ptr<CodeSlabs> slabs = std::make_shared<CodeSlabs> ();
	Expected<std::unique_ptr<MonoJit>> jit = MonoJit::create (slabs);
	if (!jit)
		return createStringError (
			inconvertibleErrorCode (),
			"the llvm backend failed to start for this domain: %s",
			toString (jit.takeError ()).c_str ());

	std::vector<MonoBuiltin> builtins =
		MonoBuiltin::get_platform_builtins ((*jit)->triple ());
	for (const MonoBuiltin &builtin : builtins)
		if (Error err = (*jit)->register_symbol (builtin.name, builtin.address))
			return std::move (err);
	if (is_jit_trace_enabled ())
		fprintf (stderr, "[llvm-jit] %zu runtime builtins registered\n",
		         builtins.size ());

	if (Error err = (*jit)->register_symbol (
	        "mono_llvm_jit_body_for_current_domain",
	        (void *) &Backend::body_for_current_domain))
		return std::move (err);

	auto fresh = std::make_unique<DomainState> ();

	fresh->domain = domain;
	fresh->slabs = std::move (slabs);
	fresh->jit = std::move (*jit);

	Expected<std::unique_ptr<StubPublisher>> stubs =
		StubPublisher::create (*fresh->jit, *fresh->slabs);
	if (!stubs)
		return stubs.takeError ();
	fresh->stubs = std::move (*stubs);

	fresh->queue.emplace (&queue_);
	return (domains_[domain] = std::move (fresh)).get ();
}

void
Backend::stop_compiling ()
{
	if (live_backend == nullptr)
		return;

	/*
	 * Stopping refuses everything from here on, drops what is queued and joins
	 * the worker - which is the wait for the compile it still has in hand.
	 *
	 * Shutdown needs this rather than a per-domain drain because it is not a
	 * domain that is going away: the root domain is never unloaded, and what
	 * shutdown tears down around it - the thread pool, finalization, the image
	 * loader - is no safer to be compiling alongside.
	 */
	live_backend->queue_.stop ();
}

void
Backend::stop_compiling_for (MonoDomain *domain)
{
	if (live_backend == nullptr)
		return;

	CompileQueue::Channel *channel = nullptr;
	{
		std::lock_guard<std::mutex> lock (live_backend->mutex_);
		auto it = live_backend->domains_.find (domain);

		if (it == live_backend->domains_.end ())
			return;

		channel = &*it->second->queue;
	}

	/*
	 * Outside mutex_, which the worker takes - and the channel outlives the
	 * unlock because a domain is notified and then freed by the one thread
	 * unloading it, in that order. Closing is idempotent, so free_domain ()
	 * closing it again costs nothing.
	 */
	channel->close ();
}

void
Backend::free_domain (MonoDomain *domain)
{
	if (live_backend == nullptr)
		return;

	/*
	 * Closing the channel is what makes the rest of this safe. The caller
	 * proves no managed thread is executing in the domain, but it cannot prove
	 * anything about the compile worker: a background compile is not
	 * "executing in the domain" in any sense mono_domain_unload () can see, and
	 * it holds this state and redirects stubs that are about to be freed.
	 * Closing refuses further work for the domain, drops what is queued and
	 * waits for what is running, so by the time the state is destroyed below
	 * nothing is left holding it.
	 *
	 * The drain runs outside mutex_, which the worker takes, and it has to run
	 * outside the loader lock as well: the compile it is waiting for may be
	 * blocked on that lock, and there is no way to ask whether this thread
	 * holds it - mono only tracks that under a debug flag. Domain teardown
	 * releases it well before here.
	 */
	std::unique_ptr<DomainState> state;
	{
		std::lock_guard<std::mutex> lock (live_backend->mutex_);
		auto it = live_backend->domains_.find (domain);

		if (it == live_backend->domains_.end ())
			return;

		/*
		 * Unlinked first, so that nothing new finds this state to compile
		 * into, and only then drained: an enqueue racing the drain either got
		 * in before it - and is dropped by it - or cannot reach the state at
		 * all. The unique_ptr holds the state alive for the drain.
		 */
		state = std::move (it->second);
		live_backend->domains_.erase (it);
	}

	forget_interp_entries (domain);

	state->queue->close ();
	/* The linker goes down with the state, releasing the domain's code. */
}

void *
Backend::body_of (MonoDomain *domain, MonoMethod *method)
{
	if (live_backend == nullptr)
		return nullptr;

	std::lock_guard<std::mutex> lock (live_backend->mutex_);
	auto state = live_backend->domains_.find (domain);

	if (state == live_backend->domains_.end ())
		return nullptr;

	auto compiled = state->second->compiled.find (method);

	if (compiled == state->second->compiled.end ())
		return nullptr;
	return compiled->second.body;
}

void
Backend::foreach_body (MonoDomain *domain, MonoMethod *method,
                       void (*visit) (MonoJitInfo *, void *), void *user_data)
{
	if (live_backend == nullptr)
		return;

	/*
	 * Copied out under the lock and visited without it: VISIT is the debugger
	 * arming a breakpoint, which takes locks of its own and can end up back in
	 * here looking a body up.
	 */
	std::vector<MonoJitInfo *> bodies;

	{
		std::lock_guard<std::mutex> lock (live_backend->mutex_);
		auto state = live_backend->domains_.find (domain);

		if (state == live_backend->domains_.end ())
			return;

		auto earlier = state->second->superseded.find (method);

		if (earlier != state->second->superseded.end ())
			bodies = earlier->second;

		auto compiled = state->second->compiled.find (method);

		if (compiled != state->second->compiled.end ()
		    && compiled->second.jinfo != nullptr)
			bodies.push_back (compiled->second.jinfo);
	}

	for (MonoJitInfo *body : bodies)
		visit (body, user_data);
}

void *
Backend::unbox_entry_of (MonoMethod *method)
{
	if (live_backend == nullptr)
		return nullptr;

	Expected<DomainState *> state = live_backend->state ();

	if (!state) {
		consumeError (state.takeError ());
		return nullptr;
	}

	if (!publishes_unbox_entry (method))
		return nullptr;

	/*
	 * The stub, not the entry behind it. This address is written straight into
	 * a value type's vtable slots, so a slot filled now has to reach whatever
	 * the method's code is later - which is what a stub is for and a raw entry
	 * cannot be.
	 */
	Expected<void *> stub = live_backend->publish (**state, method);

	if (!stub) {
		consumeError (stub.takeError ());
		return nullptr;
	}

	Expected<void *> unbox =
		(*state)->stubs->stub_address (stub_symbol (method, Entry::unbox));

	if (!unbox) {
		consumeError (unbox.takeError ());
		return nullptr;
	}

	return *unbox;
}

/*
 * --jitdump: name every function a linked object defines in /tmp/jit-<pid>.dump,
 * so perf resolves a sample in JIT-produced code to a method instead of to a
 * bare address.
 *
 * A method is several executable ranges - the fastcc body, the `$legacy` and
 * `$entry` thunks, one body per filter clause - and all of them run, so each
 * gets a record of its own.
 */
void
dump_object_code (MonoMethod *method, const CompiledMethod &compiled)
{
	for (const auto &[name, extent] : compiled.functions) {
		const auto &[code, size] = extent;
		std::string display = display_name (method, name);

		mono_emit_jit_dump_code (display.c_str (),
		                         const_cast<uint8_t *> (code),
		                         (guint32) size, nullptr, 0);
	}
}

void
Backend::remember (DomainState &state, MonoMethod *method,
                   const CompiledMethod &compiled, MonoJitInfo *jinfo)
{
	if (mono_jit_dump_is_enabled ())
		dump_object_code (method, compiled);

	/*
	 * Every call that leaves this object and could not reach its target with a
	 * pc-relative branch goes through a stub JITLink planted beside the code,
	 * so that is where a thread caught mid-call is stopped. An address with no
	 * record resolves to nothing and an async stack walk starting there sees no
	 * frame at all - which is how a thread inside a finally gets its abort
	 * delivered on the spot instead of at the end of the block.
	 *
	 * The stubs are bare jumps, so the arch CIE describes them exactly, and
	 * they belong to the object: they come out with it when the method is
	 * freed.
	 */
	std::vector<MonoJitInfo *> stubs;

	for (size_t i = 0; i < compiled.linker_stubs.size (); ++i) {
		auto &[code, size] = compiled.linker_stubs[i];
		std::string name =
			stub_symbol (method, Entry::body) + "$linker_stubs" + std::to_string (i);
		MonoJitInfo *stub_jinfo = register_stub_jinfo (
			state.domain, method, const_cast<uint8_t *> (code), size, name);

		if (stub_jinfo != nullptr)
			stubs.push_back (stub_jinfo);
	}

	if (!method->dynamic)
		return;

	std::lock_guard<std::mutex> lock (mutex_);
	Owned &owned = state.owned[method];

	if (compiled.dylib != nullptr)
		owned.dylibs.push_back (compiled.dylib);
	if (jinfo != nullptr)
		owned.jinfos.push_back (jinfo);
	owned.jinfos.insert (owned.jinfos.end (), stubs.begin (), stubs.end ());
}

void
Backend::free_method (MonoMethod *method)
{
	if (live_backend == nullptr)
		return;

	/*
	 * A background compile of this method has to be finished with before any
	 * of it is taken apart: it redirects the stubs undefined below and it
	 * reads the MonoMethod, which mono_free_method () is about to hand back to
	 * the allocator. Before the lock, which the work takes, and - like the
	 * drain in free_domain () - outside the loader lock, which the compile
	 * being waited for may be blocked on.
	 *
	 * Dropping the tag does not refuse it in future, because the next dynamic
	 * method to land on this address is a different method that has every
	 * right to be compiled.
	 */
	live_backend->queue_.drop (method);

	/* One domain's share of the release: gathered under the lock, carried out
	 * after it, because both removals take the ORC session lock. */
	struct Release {
		MonoDomain *domain;
		MonoJit *jit;
		StubPublisher *stubs_of;
		Owned owned;
		std::vector<std::string> stubs;
	};
	std::vector<Release> releases;

	{
		std::lock_guard<std::mutex> lock (live_backend->mutex_);

		/*
		 * Every domain, not just the one that asked for the method: a body
		 * reached across a domain boundary is compiled into the calling
		 * domain's linker too (body_for_current_domain ()), and every one of
		 * those copies is keyed by this MonoMethod.
		 */
		for (auto &entry : live_backend->domains_) {
			DomainState &state = *entry.second;

			if (!state.published.count (method) && !state.defined.count (method)
			    && !state.owned.count (method))
				continue;

			Release release { state.domain, state.jit.get (),
				          state.stubs.get (), {}, {} };

			/*
			 * The symbols carry the method's printed name as well as its
			 * address, so two methods at one recycled address usually want
			 * different names - but not always: nothing stops a program from
			 * minting DynamicMethods that all print the same, and a compiler
			 * emitting lambdas does exactly that. Undefining the names is what
			 * lets the next method publish stubs of its own instead of
			 * colliding with this one's.
			 */
			if (state.defined.erase (method) != 0) {
				release.stubs.push_back (stub_symbol (method, Entry::body));
				if (publishes_interop_entry (method))
					release.stubs.push_back (
						stub_symbol (method, Entry::interop));
				if (publishes_unbox_entry (method))
					release.stubs.push_back (
						stub_symbol (method, Entry::unbox));
			}
			state.compiled.erase (method);
			state.superseded.erase (method);
			state.dispatchers.erase (method);

			auto tracked = state.owned.find (method);

			if (tracked != state.owned.end ()) {
				release.owned = std::move (tracked->second);
				state.owned.erase (tracked);
			}

			/*
			 * The records that resolve the stubs' addresses back to this method
			 * go out with the stubs, for the same reason the names do: once a
			 * block is on the free list it belongs to whichever method publishes
			 * next, and a delegate built over that method's address would
			 * otherwise be bound to this one - by then freed metadata.
			 * Unreachable by name is not enough; the address is reachable too.
			 * Both stubs are released, so both records have to go.
			 */
			auto published = state.published.find (method);

			if (published != state.published.end ()) {
				if (published->second.entry_jinfo != nullptr)
					release.owned.jinfos.push_back (
						published->second.entry_jinfo);
				if (published->second.body_jinfo != nullptr)
					release.owned.jinfos.push_back (
						published->second.body_jinfo);
				if (published->second.unbox_jinfo != nullptr)
					release.owned.jinfos.push_back (
						published->second.unbox_jinfo);
				state.published.erase (published);
			}
			releases.push_back (std::move (release));
		}
	}

	for (Release &release : releases) {
		if (is_jit_trace_enabled ()) {
			char *name = mono_method_full_name (method, TRUE);

			fprintf (stderr,
			         "[llvm-jit] freeing %s releases %zu modules and %zu "
			         "records (from %s)\n",
			         name, release.owned.dylibs.size (),
			         release.owned.jinfos.size (),
			         release.domain->friendly_name);
			g_free (name);
		}

		/*
		 * Either removal only fails on something the caller promised - a stub
		 * caught materializing means the method being freed is being called.
		 * Carrying on would leave the caches saying the method is gone and the
		 * linker saying it is not, and the next method at this address would
		 * inherit the disagreement.
		 */
		auto must = [] (Error err) {
			if (err)
				report_fatal_error (
					Twine ("a freed method could not be released: ")
						+ toString (std::move (err)),
					false);
		};

		/*
		 * In this order: a lookup must never find a record covering memory a
		 * later compile has already been handed, and nothing may reach the code
		 * through a stub while it is being released.
		 */
		for (MonoJitInfo *jinfo : release.owned.jinfos)
			mono_jit_info_table_remove (release.domain, jinfo);
		must (release.stubs_of->undefine_stubs (release.stubs));
		must (release.jit->remove_dylibs (release.owned.dylibs));
	}
}

/*
 * Everything a module refers to has to have an address before it can be linked.
 * A vtable is created here rather than looked up: creating one does not run the
 * class's static constructor, which generated code calls mono_generic_class_init
 * for at the point it actually needs it.
 */
Error
Backend::resolve (DomainState &state, const std::vector<ExternalSymbol> &externals)
{
	return resolve_externals (*state.jit, state.domain, externals,
	                          [&] (MonoMethod *callee) -> Error {
		                          return publish (state, callee).takeError ();
	                          });
}

/*
 * The end of a compile as the profiler API models it: one jit_done naming the
 * method, and for a managed-to-native wrapper one more naming the icall or
 * pinvoke it wraps, which is the only name a profiler ever hears for those.
 * The alias carries the wrapper's jit info, so a consumer that pairs its
 * begin/done on jinfo->d.method == method drops it and closes on the raise for
 * METHOD itself.
 *
 * The code range goes out too. It is how a profiler that symbolicates native
 * addresses - the log profiler's coverage of the JIT-produced ranges, a
 * perf-map writer - learns that this stretch of memory belongs to a method.
 */
void
raise_jit_done (MonoMethod *method, MonoJitInfo *jinfo)
{
	MONO_PROFILER_RAISE (jit_code_buffer,
	                     (static_cast<const mono_byte *> (jinfo->code_start),
	                      jinfo->code_size, MONO_PROFILER_CODE_BUFFER_METHOD, method));

	if (method->wrapper_type == MONO_WRAPPER_MANAGED_TO_NATIVE) {
		MonoMethod *wrapped = mono_marshal_method_from_wrapper (method);

		/* A wrapper around a bare native function wraps no method. */
		if (wrapped != nullptr)
			MONO_PROFILER_RAISE (jit_done, (wrapped, jinfo));
	}

	MONO_PROFILER_RAISE (jit_done, (method, jinfo));

	/* --jitmap: name this code range in /tmp/perf-<pid>.map, so perf resolves
	 * compiled method bodies instead of reporting a bare address. */
	if (mono_jit_map_is_enabled ())
		mono_emit_jit_map (jinfo);
}

Expected<Backend::Compiled>
Backend::recover (DomainState &state, MonoMethod *method, Error failure)
{
	auto note = [&] (const CompiledMethod &code, MonoJitInfo *jinfo) {
		remember (state, method, code, jinfo);
	};

	return mono::recover (*state.jit, state.domain, method, std::move (failure), note);
}

Expected<Backend::Compiled>
Backend::translate_and_compile (DomainState &state, MonoMethod *method,
                                MonoJitInfo **published)
{
	auto publish_callee = [&] (MonoMethod *callee) -> Error {
		return publish (state, callee).takeError ();
	};
	auto stub_address = [&] (StringRef name) -> Expected<void *> {
		return state.stubs->stub_address (name);
	};
	auto note = [&] (const CompiledMethod &code, MonoJitInfo *jinfo) {
		remember (state, method, code, jinfo);
	};
	auto recover_failure = [&] (Error failure) -> Expected<Compiled> {
		return recover (state, method, std::move (failure));
	};

	TranslationTarget target { state.jit.get (), state.domain, publish_callee,
		                       stub_address, note, recover_failure };

	return mono::translate_and_compile (target, method, published);
}



Expected<Backend::Compiled>
Backend::raise_on_call (DomainState &state, MonoMethod *method, Error failure)
{
	auto note = [&] (const CompiledMethod &code, MonoJitInfo *jinfo) {
		remember (state, method, code, jinfo);
	};

	return mono::raise_on_call (*state.jit, state.domain, method,
	                            std::move (failure), note);
}

/*
 * A method whose metadata will not load is not a method that failed to compile;
 * it is a method whose every call must raise. The exception cannot be raised
 * from here - this runs inside the lazy-compile callback, and the frames between
 * it and the caller carry no jit info for mono's two-pass search to walk - so
 * the failure is boxed into the assembly that owns the method and a body is
 * compiled that unboxes and throws it. That body has jit info of its own, so the
 * throw happens somewhere the search can start, and the caller's catch sees it.
 *
 * The stand-in takes no arguments and returns nothing whatever the method's real
 * signature was, which is the point: the signature is often exactly what would
 * not convert. A function that never returns leaves its arguments and its return
 * slot untouched, so no caller can tell.
 */

Expected<void *>
Backend::compile_entry_thunk (DomainState &state, MonoMethod *method)
{
	if (m_class_get_rank (method->klass) > 0
	    && (method->iflags & METHOD_IMPL_ATTRIBUTE_INTERNAL_CALL)
	    && (method->iflags & METHOD_IMPL_ATTRIBUTE_NATIVE))
		method = mono_marshal_get_array_accessor_wrapper (method);

	ERROR_DECL (metadata_error);
	MinimalCompile cfg (method, state.domain, metadata_error);

	if (cfg.get ()->header == nullptr)
		return runtime_error (metadata_error);

	auto thunk_context = std::make_unique<LLVMContext> ();
	auto thunk_module =
		std::make_unique<Module> (stub_symbol (method, Entry::interop), *thunk_context);

	std::vector<ExternalSymbol> thunk_externals;
	MethodLLVMEmitter declarer (thunk_module.get (), cfg.get (), method,
	                            &thunk_externals);
	Expected<Function *> target = declarer.declare (method);

	if (!target)
		return target.takeError ();

	MonoMethodSignature *sig = mono_method_signature_internal (method);

	if (method->string_ctor)
		sig = mono_marshal_get_string_ctor_signature (method);

	std::string thunk_name = stub_symbol (method, Entry::interop);

	arch::create_legacy_entry_thunk (*thunk_module, thunk_name, *target,
	                                 legacy_call_flavor (sig));

	if (Error err = bind_symbols (*thunk_module))
		return std::move (err);

	if (dumping (thunk_name.c_str ()))
		thunk_module->print (llvm::errs (), nullptr);

	if (Error err = resolve (state, thunk_externals))
		return std::move (err);

	Expected<CompiledMethod> thunk = state.jit->compile (
		ThreadSafeModule (std::move (thunk_module),
		                  ThreadSafeContext (std::move (thunk_context))),
		thunk_name);
	if (!thunk)
		return thunk.takeError ();

	Expected<MonoJitInfo *> jinfo = register_jit_info (
		state.domain, method, nullptr, *thunk, CodeKind::AbiThunk);

	if (!jinfo)
		return jinfo.takeError ();
	remember (state, method, *thunk, *jinfo);

	return thunk->entry;
}

Expected<void *>
Backend::compile (MonoMethod *method, MonoDomain *target_domain)
{
	/*
	 * SGen identifies threads suspended inside the managed allocator and the
	 * write barrier by resolving code addresses through the jit-info table,
	 * and the runtime asserts the pointer it hands out for those wrappers
	 * resolves too. The body carries jit info, so it is what they get;
	 * everything else gets the stub, which is what keeps callers correct across
	 * promotions. Nothing enters either wrapper through the address itself -
	 * generated code calls them by symbol - so handing out the body rather than
	 * the stub costs those two the ability to be promoted and nothing else.
	 */
	bool wants_body = method->wrapper_type == MONO_WRAPPER_ALLOC
	                  || method->wrapper_type == MONO_WRAPPER_WRITE_BARRIER;

	Expected<DomainState *> state = this->state_for (target_domain);
	if (!state)
		return state.takeError ();

	Expected<void *> stub = publish (**state, method);
	if (!stub)
		return stub;

	/*
	 * Publishing only reserves the address; the code still has to exist before
	 * the caller is handed something to call. Compiling here rather than on the
	 * first call is what lets a refusal come back through MonoError and be
	 * raised by the runtime, which knows how to throw from where it stands.
	 */
	Expected<Compiled> code =
		ensure_entries (**state, method, recompiling (method));
	if (!code)
		return code.takeError ();

	return wants_body ? code->body : *stub;
}


void
Backend::enqueue_recompile (DomainState &state, MonoMethod *method)
{
	char *name = mono_method_full_name (method, TRUE);
	bool wanted = async_recompiling (name) && compilable_off_thread (method);

	g_free (name);

	if (!wanted)
		return;

	/*
	 * The state is captured raw: it cannot be destroyed until its channel has
	 * been closed, and closing waits for this work. See DomainState::queue.
	 */
	DomainState *owner = &state;

	state.queue->enqueue (method, [this, owner, method] {
		if (unsigned ms = async_delay ())
			std::this_thread::sleep_for (std::chrono::milliseconds (ms));

		Expected<Compiled> code = compile_and_publish (*owner, method);

		if (code)
			return;

		/*
		 * Nobody is waiting for this, and the method already has a body that
		 * works. A background compile that fails leaves the method exactly as
		 * it was, which is why nothing here retries or raises.
		 */
		if (is_jit_trace_enabled ()) {
			char *failed = mono_method_full_name (method, TRUE);

			fprintf (stderr, "[llvm-jit] background compile of %s failed: %s\n",
			         failed, toString (code.takeError ()).c_str ());
			g_free (failed);
			return;
		}
		consumeError (code.takeError ());
	});
}

Expected<Backend::Compiled>
Backend::ensure_entries (DomainState &state, MonoMethod *method, bool again)
{
	if (!again) {
		std::lock_guard<std::mutex> lock (mutex_);
		auto it = state.compiled.find (method);

		if (it != state.compiled.end ())
			return it->second;

		auto interp = state.interpreted.find (method);

		if (interp != state.interpreted.end ())
			return interp->second;
	}

	Expected<Compiled> code = compile_and_publish (state, method);

	/*
	 * Here rather than in compile_and_publish (), which is also what the
	 * background compile itself calls: asking for another one from in there
	 * would queue a compile per compile forever.
	 */
	if (code)
		enqueue_recompile (state, method);
	return code;
}

void
Backend::request_promotion (DomainState &state, MonoMethod *method)
{
	{
		std::lock_guard<std::mutex> lock (mutex_);

		if (!state.promoting.insert (method).second)
			return;
	}

	/*
	 * The state is captured raw: it cannot be destroyed until its channel has
	 * been closed, and closing waits for this work. See DomainState::queue.
	 */
	DomainState *owner = &state;
	bool queued = state.queue->enqueue (method, [this, owner, method] {
		if (unsigned ms = async_delay ())
			std::this_thread::sleep_for (std::chrono::milliseconds (ms));

		Expected<Compiled> code = compile_and_publish (*owner, method, true);

		if (code) {
			if (is_jit_trace_enabled ()) {
				char *name = mono_method_full_name (method, TRUE);

				fprintf (stderr, "[llvm-jit] promoted %s\n", name);
				g_free (name);
			}
			return;
		}

		/*
		 * Nobody is waiting for this and the method is still being interpreted,
		 * which is a correct way to run it. A promotion that fails therefore
		 * leaves the method exactly as it was rather than raising.
		 */
		if (is_jit_trace_enabled ()) {
			char *name = mono_method_full_name (method, TRUE);

			fprintf (stderr, "[llvm-jit] promoting %s failed: %s\n", name,
			         toString (code.takeError ()).c_str ());
			g_free (name);
			return;
		}
		consumeError (code.takeError ());
	});

	/*
	 * A refusal is the domain being torn down or the queue having stopped, and
	 * the counter has already fired for good - so the method stays interpreted
	 * for the rest of its life. That is the direction that cannot be wrong, and
	 * it is why nothing here retries.
	 */
	if (!queued && is_jit_trace_enabled ()) {
		char *name = mono_method_full_name (method, TRUE);

		fprintf (stderr, "[llvm-jit] promotion of %s was refused\n", name);
		g_free (name);
	}
}

Expected<std::vector<void *>>
Backend::counted_entries (DomainState &state, MonoMethod *method,
                          ArrayRef<void *> entries)
{
	/*
	 * One trampoline per entry rather than one for the method: each has to
	 * carry on into the door the call came for, and a re-entry trampoline
	 * remembers a single landing address. They share the counter, so whichever
	 * fires is the only one that ever does, and they share the request behind
	 * it either way.
	 */
	DomainState *owner = &state;
	std::vector<std::pair<void *, void *>> doors;

	for (size_t i = 0; i < entries.size (); ++i) {
		void *carry_on = entries[i];
		std::string name = stub_symbol (method, Entry::body) + "$promote."
		                   + std::to_string (i);
		Expected<void *> promote = state.stubs->create_lazy_entry (
			name, [this, owner, method, carry_on] () -> Expected<void *> {
				request_promotion (*owner, method);
				return carry_on;
			});

		if (!promote)
			return promote.takeError ();
		doors.push_back ({ carry_on, *promote });
	}

	Expected<std::vector<void *>> thunks =
		state.stubs->create_counter_thunks (tier1_threshold (), doors);

	if (!thunks)
		return thunks;

	/*
	 * A thunk touches no stack, so a walk that catches a thread in one is
	 * looking at the frame the call left behind - which is what the stub unwind
	 * program says and what these records hand back.
	 */
	for (size_t i = 0; i < thunks->size (); ++i)
		register_stub_jinfo (state.domain, method, (*thunks)[i],
		                     arch::counter_thunk_code_size,
		                     stub_symbol (method, Entry::body) + "$count."
		                             + std::to_string (i));

	return thunks;
}

Expected<Backend::Compiled>
Backend::compile_and_publish (DomainState &state, MonoMethod *method, bool tier1)
{
	/*
	 * Materialize as the domain the code is for. Translation itself is kept
	 * domain-clean, but what it calls back into is not: the outside-il branch
	 * re-enters mono_jit_compile_method, which compiles wrappers for the
	 * thread's current domain, and welding another domain's wrapper stub into
	 * this linker is a pointer into code that can be freed under it. The
	 * runtime asking for an icall wrapper with the root domain as target
	 * arrives here the same way, whatever domain the thread is running as.
	 */
	MonoDomain *entered = mono_domain_get ();

	if (entered != state.domain)
		mono_domain_set_internal_with_options (state.domain, FALSE);

	auto leave = [&] {
		if (entered != state.domain)
			mono_domain_set_internal_with_options (entered, FALSE);
	};

	/*
	 * Above the tier split, so that a method gets the same verdict whichever
	 * tier ends up running it. A body the verifier rejects is a body no tier
	 * may run, and a body it accepts is one every tier may.
	 */
	if (Error invalid = verify_method (method)) {
		leave ();
		return std::move (invalid);
	}

	if (!tier1 && runs_at_tier0 (method)) {
		/*
		 * The stubs are pointed at the interpreter before the method is
		 * transformed, and that order matters: transforming runs the class
		 * initializer, and this can be running inside a lazy stub's callback,
		 * which holds a lock across it. A cctor that calls back into this very
		 * method would then re-enter the trampoline being resolved and deadlock
		 * against that lock. Publishing first means such a call lands on the
		 * entry instead.
		 */
		Expected<Compiled> entries = interp_entries (state, method);

		if (!entries) {
			/*
			 * Neither the interpreter refusing the method nor this machine's
			 * entry being unable to carry the call is a failure: the method
			 * gets compiled like anything else.
			 */
			consumeError (entries.takeError ());
		} else {
			ERROR_DECL (transform_error);

			if (mini_get_interp_callbacks ()->transform_method (
			            method, transform_error)) {
				leave ();
				return entries;
			}

			mono_error_cleanup (transform_error);

			{
				std::lock_guard<std::mutex> lock (mutex_);

				state.interpreted.erase (method);
			}
		}
	}

	MonoJitInfo *published = nullptr;
	Expected<Compiled> code = [&] {
		timing::Scope timed (timing::Phase::compile);

		return translate_and_compile (state, method, &published);
	}();

	leave ();

	auto give_up = [&] (Error err) {
		if (published != nullptr)
			MONO_PROFILER_RAISE (jit_failed, (jinfo_get_method (published)));
		return std::move (err);
	};

	if (!code)
		return give_up (code.takeError ());

	/*
	 * Before the redirects below, which are what make the body reachable. A
	 * body that goes live carrying none of the breakpoints already set on the
	 * method is a breakpoint that stops being hit the moment the method is
	 * compiled again, with nothing said about it.
	 */
	if (published != nullptr)
		mini_install_pending_breakpoints (state.domain,
		                                  jinfo_get_method (published), published);

	if (Error err = state.stubs->redirect_stub (stub_symbol (method, Entry::body), code->body))
		return give_up (std::move (err));
	if (publishes_interop_entry (method)) {
		if (Error err = state.stubs->redirect_stub (stub_symbol (method, Entry::interop),
		                                            code->entry))
			return give_up (std::move (err));
	}
	if (publishes_unbox_entry (method)) {
		if (Error err = state.stubs->redirect_stub (stub_symbol (method, Entry::unbox),
		                                          code->unbox))
			return give_up (std::move (err));
	}

	/*
	 * Two threads racing here both compile; the loser's code is merely
	 * unreferenced, and both ends stay coherent because the redirects above
	 * always point every one of a method's stubs at one compile's output.
	 * A stub that had already gone to a dispatcher gets rebound to this
	 * domain's own code here, which is mini's behavior too: a same-domain
	 * resolve patches the call site.
	 *
	 * The body being replaced is not dead - the loser of that race, or an
	 * earlier compile, can still have threads running in it - so it moves to
	 * the superseded list rather than being dropped.
	 */
	{
		std::lock_guard<std::mutex> lock (mutex_);
		Compiled &live = state.compiled[method];

		if (live.jinfo != nullptr && live.jinfo != code->jinfo)
			state.superseded[method].push_back (live.jinfo);
		live = *code;
	}

	/*
	 * Only now, and outside the lock. The debugger agent's handler for this
	 * parks the compiling thread and lets its own thread run, and what that
	 * thread does with a freshly compiled method is look it up - through
	 * mono_llvm_jit_find_body (), which reads the record above and takes the
	 * same lock.
	 */
	if (published != nullptr)
		raise_jit_done (jinfo_get_method (published), published);

	/*
	 * A method the interpreter is already running calls its callees by
	 * interpreting them, and it has no other way of noticing that one of them
	 * has since been given code to call instead.
	 */
	if (mono_use_interpreter)
		mini_get_interp_callbacks ()->method_compiled (state.domain, method);

	return *code;
}

Expected<Backend::Compiled>
Backend::interp_entries (DomainState &state, MonoMethod *method)
{
	Expected<const arch::InterpEntryPoint *> entry =
		mono::interp_entry (state.domain, method);

	if (!entry)
		return entry.takeError ();

	void *body = arch::interp_entry_thunk ();
	void *counted_body = body;

	/* One door now, so N really is N calls to the method. */
	if (tier1_threshold () != 0) {
		Expected<std::vector<void *>> counted =
			counted_entries (state, method, { body });

		if (!counted)
			return counted.takeError ();

		counted_body = (*counted)[0];
	}

	/*
	 * The receiver a value type's vtable slot arrives with is the boxed object,
	 * and stepping it past the header is the whole of the difference - so this
	 * is the runtime's own unboxing trampoline, the same one a method whose code
	 * the backend did not generate gets. It goes over the counted shim rather
	 * than over the interpreter's own entry: the shim is what speaks this
	 * backend's convention, and it is where mono_arch_get_unbox_trampoline's
	 * receiver-in-the-first-register assumption holds.
	 */
	Compiled entries { counted_body, counted_body,
		           publishes_unbox_entry (method)
		                   ? mono_arch_get_unbox_trampoline (method, counted_body)
		                   : nullptr };

	/*
	 * More than one store where the method has more than one door, so a caller
	 * can see a method's stubs disagree about which of them has been pointed
	 * somewhere yet. That is harmless while the alternative to each is the lazy
	 * trampoline it replaces, and two threads arriving together cannot disagree
	 * about more than that: whether a method runs here or is compiled is settled
	 * by the method, so both of them take this branch or neither does.
	 */
	if (Error err =
	            state.stubs->redirect_stub (stub_symbol (method, Entry::body), entries.body))
		return std::move (err);
	if (entries.unbox != nullptr) {
		if (Error err = state.stubs->redirect_stub (stub_symbol (method, Entry::unbox),
		                                          entries.unbox))
			return std::move (err);
	}

	if (is_jit_trace_enabled ()) {
		char *name = mono_method_full_name (method, TRUE);

		fprintf (stderr, "[llvm-jit] interpreting %s (for %s)\n", name,
		         state.domain->friendly_name);
		g_free (name);
	}

	std::lock_guard<std::mutex> lock (mutex_);

	return state.interpreted[method] = entries;
}

Expected<void *>
Backend::dispatcher (DomainState &state, MonoMethod *method)
{
	{
		std::lock_guard<std::mutex> lock (mutex_);
		auto it = state.dispatchers.find (method);

		if (it != state.dispatchers.end ())
			return it->second;
	}

	auto note = [&] (const CompiledMethod &code, MonoJitInfo *jinfo) {
		remember (state, method, code, jinfo);
	};
	Expected<void *> built =
		build_dispatcher (*state.jit, state.domain, method, note);

	if (!built)
		return built;

	std::lock_guard<std::mutex> lock (mutex_);
	return state.dispatchers[method] = *built;
}

void *
Backend::body_for_current_domain (MonoMethod *method)
{
	/*
	 * A dispatcher is the only caller, so the backend exists. Having no state
	 * for the domain being called into is not something the program could be
	 * told about - there is no linker to compile the telling.
	 */
	Expected<DomainState *> state = live_backend->state ();

	if (!state)
		report_fatal_error (
			Twine ("no domain state for a dispatched call: ")
				+ toString (state.takeError ()),
			false);

	Expected<Compiled> code = live_backend->ensure_entries (**state, method);

	if (code)
		return code->body;

	/* The call is already under way, so the failure is raised from it. */
	Expected<Compiled> raising =
		live_backend->raise_on_call (**state, method, code.takeError ());

	if (!raising)
		report_fatal_error (
			Twine ("a dispatched method failed to compile: ")
				+ toString (raising.takeError ()),
			false);

	return raising->body;
}

Error
Backend::publish_defs (DomainState &state, MonoMethod *method)
{
	/*
	 * Held across the creation: two threads reaching an undefined method
	 * together must not both define its stubs. Everything under the lock is
	 * reserving - a re-entry trampoline, a callback unit, a stub block - and
	 * none of it compiles or links anything.
	 */
	std::lock_guard<std::mutex> lock (mutex_);

	if (state.defined.count (method))
		return Error::success ();

	/*
	 * The lambdas capture the owning state rather than looking the domain up
	 * again: a stub is only reachable from its own domain's code, and the state
	 * outlives the stub - free_domain () takes them down together.
	 *
	 * The thread that fires a stub is not necessarily running as the owning
	 * domain: AppDomain:InvokeInDomain switches the domain and then calls, so
	 * the first caller through a stub can arrive from the far side of that
	 * switch. Binding then would weld one domain's copy into another domain's
	 * code - wrong statics and vtables while it runs, dangling code once the
	 * bound domain unloads. mini refused to patch such a call site and left
	 * the trampoline to re-resolve each call; the dispatcher below is that
	 * refusal here. Methods without a body of their own bind directly - what
	 * stands behind them is mini's, one copy for the whole process.
	 */
	DomainState *owner = &state;
	auto bindable = [owner, method] () {
		return mono_domain_get () == owner->domain
		       || implemented_outside_il (method);
	};
	/* HALF says which of the stand-in's two compiles this stub wants. */
	auto raising = [this, owner, method] (Error failure,
	                                      void *Compiled::*half) -> Expected<void *> {
		Expected<Compiled> thrower =
			raise_on_call (*owner, method, std::move (failure));

		if (!thrower)
			return thrower.takeError ();
		return (*thrower).*half;
	};

	/*
	 * The interop entry is a stub of its own only where there is one to publish:
	 * a wrapper native code enters. It is the one entry a dispatcher cannot
	 * answer for - a cross-domain caller arrives speaking C, and the dispatcher
	 * forwards in this backend's convention - so it keeps a thunk of its own,
	 * which reaches the method through the body stub and is domain-neutral.
	 */
	if (publishes_interop_entry (method)) {
		if (Error err = state.stubs->create_lazy_stub (
		        stub_symbol (method, Entry::interop),
		        [this, owner, method, bindable, raising] () -> Expected<void *> {
			        if (!bindable ()) {
				        Expected<void *> thunk =
					        compile_entry_thunk (*owner, method);

				        if (!thunk)
					        return raising (thunk.takeError (),
					                        &Compiled::entry);
				        return *thunk;
			        }

			        Expected<Compiled> code = ensure_entries (*owner, method);

			        if (!code)
				        return raising (code.takeError (), &Compiled::entry);
			        return code->entry;
		        }))
			return err;
	}

	// Keyed on the method, so one body can stand for many of them.
	if (Error err = state.stubs->create_lazy_stub (
	        stub_symbol (method, Entry::body),
	        [this, owner, method, bindable, raising] () -> Expected<void *> {
		        if (!bindable ()) {
			        Expected<void *> call = dispatcher (*owner, method);

			        if (!call)
				        return raising (call.takeError (), &Compiled::body);
			        return *call;
		        }

		        Expected<Compiled> code = ensure_entries (*owner, method);

		        if (!code)
			        return raising (code.takeError (), &Compiled::body);
		        return code->body;
	        },
	        method))
		return err;

	/*
	 * The unboxing entry is a stub too, and for the reason the others are: it
	 * is what fills a value type's vtable slots, so a slot filled before the
	 * method had code has to keep working once it does. It gets no dispatcher -
	 * the runtime only ever asks for one while compiling in the owning domain.
	 */
	if (publishes_unbox_entry (method)) {
		if (Error err = state.stubs->create_lazy_stub (
		        stub_symbol (method, Entry::unbox),
		        [this, owner, method, raising] () -> Expected<void *> {
			        Expected<Compiled> code =
				        ensure_entries (*owner, method);

			        if (!code)
				        return raising (code.takeError (),
				                        &Compiled::unbox);
			        return code->unbox;
		        },
		        method))
			return err;
	}

	state.defined.insert (method);
	return Error::success ();
}

Expected<void *>
Backend::publish (DomainState &state, MonoMethod *method)
{
	{
		std::lock_guard<std::mutex> lock (mutex_);
		auto it = state.published.find (method);

		if (it != state.published.end ())
			return it->second.stub;
	}

	if (Error err = publish_defs (state, method))
		return std::move (err);

	/* Whichever thread reserved them, these are the addresses it reserved. */
	bool interop = publishes_interop_entry (method);
	std::string entry_name = interop ? stub_symbol (method, Entry::interop) : std::string ();
	std::string body_name = stub_symbol (method, Entry::body);
	Expected<void *> body = state.stubs->stub_address (body_name);
	if (!body)
		return body;

	/* The address the rest of the runtime is handed: the C door where there is one. */
	void *stub = *body;

	if (interop) {
		Expected<void *> reserved = state.stubs->stub_address (entry_name);

		if (!reserved)
			return reserved;
		stub = *reserved;
	}

	bool unboxed = publishes_unbox_entry (method);
	std::string unbox_name = unboxed ? stub_symbol (method, Entry::unbox) : std::string ();
	void *unbox = nullptr;

	if (unboxed) {
		Expected<void *> reserved = state.stubs->stub_address (unbox_name);

		if (!reserved)
			return reserved;
		unbox = *reserved;
	}

	std::lock_guard<std::mutex> lock (mutex_);
	auto it = state.published.find (method);

	if (it != state.published.end ())
		return it->second.stub;

	/*
	 * A stub is the only address the runtime ever sees for the method, so
	 * anything recovering a method from a code pointer - delegate creation off
	 * a ldftn most visibly - must find it in the jit-info table. Register each
	 * the way mini registers trampolines: an is_trampoline entry carrying the
	 * method, in the domain whose linker holds the stub so the two die
	 * together. The published check above keeps racing threads from registering
	 * any of them twice.
	 */
	MonoJitInfo *entry_jinfo =
		interop ? register_stub_jinfo (state.domain, method, stub,
	                                       arch::stub_block_size, entry_name)
	                : nullptr;
	MonoJitInfo *body_jinfo = register_stub_jinfo (
		state.domain, method, *body, arch::stub_block_size, body_name);
	MonoJitInfo *unbox_jinfo =
		unboxed ? register_stub_jinfo (state.domain, method, unbox,
	                                       arch::stub_block_size, unbox_name)
	                : nullptr;

	state.published[method] =
		Publication { stub, entry_jinfo, body_jinfo, unbox_jinfo, unboxed };

	return stub;
}

} // namespace

namespace legacy {

llvm::Expected<void *>
compile (MonoMethod *method, MonoDomain *target_domain)
{
	llvm::Expected<Backend *> backend = Backend::get ();

	if (!backend)
		return backend.takeError ();
	return (*backend)->compile (method, target_domain);
}

void
stop_compiling ()
{
	Backend::stop_compiling ();
}

void
stop_compiling_for (MonoDomain *domain)
{
	Backend::stop_compiling_for (domain);
}

void
free_domain (MonoDomain *domain)
{
	Backend::free_domain (domain);
}

void
free_method (MonoMethod *method)
{
	Backend::free_method (method);
}

void *
body_of (MonoDomain *domain, MonoMethod *method)
{
	return Backend::body_of (domain, method);
}

void
foreach_body (MonoDomain *domain, MonoMethod *method,
              void (*visit) (MonoJitInfo *, void *), void *user_data)
{
	Backend::foreach_body (domain, method, visit, user_data);
}

void *
unbox_entry_of (MonoMethod *method)
{
	return Backend::unbox_entry_of (method);
}

} // namespace legacy

} // namespace mono

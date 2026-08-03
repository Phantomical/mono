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

#include "jinfo.hpp"
#include "jit.hpp"
#include "stubs.hpp"
#include "method-to-llvm.hpp"

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

// This breaks some LLVM headers
#undef PIC

#include <llvm/ADT/Twine.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/ErrorHandling.h>

#include <cmath>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using namespace llvm;
using namespace llvm::orc;

namespace mono {
namespace {

/*
 * The runtime entry points generated code calls by name. Everything else it
 * refers to is per-method or per-class and is resolved from what the translator
 * recorded; these are fixed, so they are registered once when the engine starts.
 *
 * A name the translator emits that is missing here fails the compile rather than
 * resolving to something arbitrary, which is why the list is explicit.
 */
struct Helper {
	const char *name;
	void *address;
};

std::vector<Helper>
runtime_helpers ()
{
	return {
		{ "mono_domain_get", (void *) &mono_domain_get },
		{ "mono_marshal_set_last_error", (void *) &mono_marshal_set_last_error },
		{ "mono_gc_wbarrier_generic_store_internal",
		  (void *) &mono_gc_wbarrier_generic_store_internal },
		{ "mono_gc_wbarrier_value_copy_internal",
		  (void *) &mono_gc_wbarrier_value_copy_internal },

		/*
		 * The throw path. These are mono's own throw trampolines: they
		 * capture the register state and enter mono_handle_exception, whose
		 * two-pass search over the MonoJitInfo published per method is how a
		 * handler is found - the native unwinder is never involved. The
		 * corlib variant takes the exception's type-def index and reads the
		 * throw site out of the return address; resume_unwind is what a
		 * finally or fault calls when it was entered by unwinding and has
		 * run out.
		 */
		{ "mono_llvm_throw_exception", mono_get_throw_exception () },
		{ "mono_llvm_rethrow_exception", mono_get_rethrow_exception () },
		{ "mono_llvm_throw_corlib_exception",
		  (void *) mono_find_jit_icall_info (
			  MONO_JIT_ICALL_mono_llvm_throw_corlib_exception_abs_trampoline)
			  ->func },
		{ "mono_llvm_resume_unwind",
		  (void *) mono_find_jit_icall_info (
			  MONO_JIT_ICALL_mono_llvm_resume_unwind_trampoline)
			  ->func },

		/*
		 * The personality routine a landing pad names. Generated code never
		 * calls it; the unwinder does, on the way through a frame that has a
		 * handler.
		 */
		{ "mono_personality", (void *) &mono_personality },

		/*
		 * The libcalls LLVM lowers its memory and float-remainder operations
		 * to. The translator never names these; codegen synthesizes the calls,
		 * so linking against the process's libc is part of what the backend has
		 * to provide.
		 */
		{ "memcmp", (void *) &memcmp },
		{ "memcpy", (void *) &memcpy },
		{ "memmove", (void *) &memmove },
		{ "memset", (void *) &memset },
		{ "fmod", (void *) static_cast<double (*) (double, double)> (&fmod) },
		{ "fmodf", (void *) static_cast<float (*) (float, float)> (&fmodf) },
	};
}

/*
 * The parts of a MonoCompile the translator reads. The rest belongs to the mini
 * pipeline, which is not running here.
 */
class MinimalCompile {
public:
	MinimalCompile (MonoMethod *method, MonoDomain *domain, MonoError *error)
	{
		memset (&cfg, 0, sizeof (cfg));
		cfg.method = method;
		/*
		 * The domain the code is being compiled for - the owning linker's,
		 * not the compiling thread's current one. The translator reads it
		 * wherever it resolves per-domain state at translate time (ldstr).
		 */
		cfg.domain = domain;
		cfg.compile_llvm = TRUE;
		cfg.opt = MONO_OPT_SIMD;
		cfg.header = mono_method_get_header_checked (method, error);
	}

	~MinimalCompile () { mono_metadata_free_mh (cfg.header); }

	MinimalCompile (const MinimalCompile &) = delete;
	MinimalCompile &operator= (const MinimalCompile &) = delete;

	MonoCompile *get () { return &cfg; }

private:
	MonoCompile cfg;
};

/// Whether MONO_LLVM_JIT_TRACE asked to see every method this backend translates.
/// Worth having because a method reached as a callee is compiled without the
/// runtime ever being asked for it, so nothing else says it happened.
bool
tracing ()
{
	static bool on = g_getenv ("MONO_LLVM_JIT_TRACE") != NULL;

	return on;
}

/// Whether MONO_LLVM_JIT_DUMP names this method: a substring of its full name
/// selects it for having its IL and translated IR printed to stderr.
bool
dumping (const char *name)
{
	static const char *filter = g_getenv ("MONO_LLVM_JIT_DUMP");

	return filter != nullptr && strstr (name, filter) != nullptr;
}

void
dump_il (MonoMethod *method, MonoMethodHeader *header)
{
	const uint8_t *code = mono_method_header_get_code (header, nullptr, nullptr);
	uint32_t size;

	mono_method_header_get_code (header, &size, nullptr);

	char *il = mono_disasm_code (nullptr, method, code, code + size);
	fprintf (stderr, "%s\n", il);
	g_free (il);
}

std::string
symbol_for_code (MonoMethod *method)
{
	char *name = mono_method_full_name (method, TRUE);
	std::string symbol = name;
	char suffix[32];

	g_free (name);

	/*
	 * The printed name is for reading; the pointer is the identity. No name
	 * scheme is unique on its own - conversion operators overload on their
	 * return type, which no printed signature carries, and the runtime mints
	 * wrappers whose names are only as distinct as what they were generated
	 * from - and a symbol is one method's identity here.
	 *
	 * create_method_decl () names methods the same way; the two must agree or
	 * a caller's reference never finds the stub.
	 */
	snprintf (suffix, sizeof (suffix), "@%p", (void *) method);
	symbol += suffix;
	return symbol;
}

/// The `$fast` symbol: the fastcc body generated callers bind to directly.
/// The plain symbol stays the legacy entry - the interop thunk - which is what
/// the runtime, delegates and every escaped function pointer see.
std::string
symbol_for_body (MonoMethod *method)
{
	return symbol_for_code (method) + "$fast";
}

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

	/// Drop DOMAIN's linker and everything in it. The caller proves the code
	/// dead: nothing may be executing in, or about to call into, the domain.
	static void free_domain (MonoDomain *domain);

private:
	/// Where one method's code ended up: the legacy entry the runtime hands
	/// out, and the fastcc body generated callers reach.
	struct Compiled {
		void *entry;
		void *body;
	};

	/// One domain's whole compilation state.
	struct DomainState {
		/// The domain this state compiles for. Everything materialized into
		/// the linker resolves against this domain - never against the
		/// thread's current domain, which can point elsewhere while a stub
		/// fires (AppDomain:InvokeInDomain switches the domain and then calls).
		MonoDomain *domain;
		std::unique_ptr<MonoJit> jit;
		/// Methods whose stubs are defined in the linker, so a callee reached
		/// from several places is only given its stubs once. Defined is all a
		/// caller's module needs to link.
		std::unordered_set<MonoMethod *> defined;
		/// Methods already published: stubs defined, address in hand, tramp
		/// info registered. Holds the legacy stub's address.
		std::unordered_map<MonoMethod *, void *> published;
		/// Methods whose stubs already point at real code - and where that
		/// code is, so that asking again is a lookup rather than a compile.
		std::unordered_map<MonoMethod *, Compiled> compiled;
		/// Methods whose body stub was first reached from another domain, and
		/// the per-call dispatcher each got instead of a direct binding.
		std::unordered_map<MonoMethod *, void *> dispatchers;
	};

	/// The current domain's state, created - linker, helpers and all - the
	/// first time the domain compiles something.
	Expected<DomainState *> state ();

	/// DOMAIN's state, created on first use like state ().
	Expected<DomainState *> state_for (MonoDomain *domain);

	Error resolve (DomainState &state, const std::vector<ExternalSymbol> &externals);
	Expected<Compiled> translate_and_compile (DomainState &state, MonoMethod *method);
	Expected<Compiled> ensure_compiled (DomainState &state, MonoMethod *method);

	/// The legacy entry - the interop thunk - as a compile of its own. It
	/// reaches the method through the body's stub, so it is domain-neutral
	/// and safe to build whichever domain the building thread has current.
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
};

/// The one Backend, once get () has made it. Kept at file scope so that
/// free_domain () can decline quietly when nothing was ever compiled.
Backend *live_backend = nullptr;

Expected<Backend *>
Backend::get ()
{
	static std::once_flag once;

	std::call_once (once, [] { live_backend = new Backend (); });
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

	Expected<std::unique_ptr<MonoJit>> jit = MonoJit::create ();
	if (!jit)
		return createStringError (
			inconvertibleErrorCode (),
			"the llvm backend failed to start for this domain: %s",
			toString (jit.takeError ()).c_str ());

	for (const Helper &helper : runtime_helpers ())
		if (Error err = (*jit)->register_symbol (helper.name, helper.address))
			return std::move (err);
	if (Error err = (*jit)->register_symbol (
	        "mono_llvm_jit_body_for_current_domain",
	        (void *) &Backend::body_for_current_domain))
		return std::move (err);

	auto fresh = std::make_unique<DomainState> ();

	fresh->domain = domain;
	fresh->jit = std::move (*jit);
	return (domains_[domain] = std::move (fresh)).get ();
}

void
Backend::free_domain (MonoDomain *domain)
{
	if (live_backend == nullptr)
		return;

	std::unique_ptr<DomainState> state;
	{
		std::lock_guard<std::mutex> lock (live_backend->mutex_);
		auto it = live_backend->domains_.find (domain);

		if (it == live_backend->domains_.end ())
			return;
		state = std::move (it->second);
		live_backend->domains_.erase (it);
	}
	/* The linker goes down with the state, releasing the domain's code. */
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
	/*
	 * The owning domain, never the current one: a vtable or statics address
	 * baked into this linker's code must belong to the domain the code will
	 * run as. The two can differ here - a lazy stub fires under whatever
	 * domain the calling thread has current.
	 */
	MonoDomain *domain = state.domain;

	for (const ExternalSymbol &external : externals) {
		void *address = nullptr;

		switch (external.kind) {
		case ExternalSymbol::Kind::Class:
			address = external.object;
			break;
		case ExternalSymbol::Kind::Method:
		case ExternalSymbol::Kind::Field:
		case ExternalSymbol::Kind::Address:
			address = external.object;
			break;
		case ExternalSymbol::Kind::VTable:
		case ExternalSymbol::Kind::Statics: {
			ERROR_DECL (error);
			MonoVTable *vtable = mono_class_vtable_checked (
				domain, static_cast<MonoClass *> (external.object), error);

			if (vtable == nullptr)
				return runtime_error (error);

			address = external.kind == ExternalSymbol::Kind::VTable
			              ? static_cast<void *> (vtable)
			              : mono_vtable_get_static_field_data (vtable);
			break;
		}
		case ExternalSymbol::Kind::Code: {
			/*
			 * The full publish, not just the definitions: an ldftn'd pointer
			 * escapes into delegates, and recovering the method from it needs
			 * the tramp info publish () registers. resolve () runs with no
			 * backend lock held, so the address lookup inside is safe.
			 */
			Expected<void *> stub =
				publish (state, static_cast<MonoMethod *> (external.object));
			if (!stub)
				return stub.takeError ();
			/* publish () defines the symbol itself. */
			continue;
		}
		}

		if (Error err = state.jit->register_symbol (external.name, address))
			return err;
	}

	return Error::success ();
}

Expected<Backend::Compiled>
Backend::translate_and_compile (DomainState &state, MonoMethod *method)
{
	/*
	 * Array Get/Set/Address have no body and no icall - every call site
	 * lowers them inline. The compilable form, for the runtime paths that
	 * need the method itself, is the marshal wrapper, whose inner call to
	 * the accessor lowers the same way.
	 */
	if (m_class_get_rank (method->klass) > 0
	    && (method->iflags & METHOD_IMPL_ATTRIBUTE_INTERNAL_CALL)
	    && (method->iflags & METHOD_IMPL_ATTRIBUTE_NATIVE))
		method = mono_marshal_get_array_accessor_wrapper (method);

	auto context = std::make_unique<LLVMContext> ();
	auto module = std::make_unique<Module> (symbol_for_body (method), *context);

	if (implemented_outside_il (method)) {
		ERROR_DECL (compile_error);
		void *code = mono_jit_compile_method (method, compile_error);

		if (code == nullptr)
			return runtime_error (compile_error);

		/*
		 * mini's code is the legacy convention; generated code declares such
		 * a method against the plain symbol and lowers its calls, so nothing
		 * ever reaches the body stub expecting fastcc.
		 */
		return Compiled { code, code };
	}

	/*
	 * Translation itself resolves everything per-domain against state.domain
	 * (resolve (), ldstr through cfg->domain) - never against the thread's
	 * current domain, and any new translate-time mono_domain_get () is a
	 * cross-domain bug of the kind the dispatcher exists to prevent. The
	 * stub lambdas keep the two equal except for vararg methods, which
	 * cannot be dispatched and so still compile here when first reached
	 * from another domain.
	 */
	g_assert (mono_domain_get () == state.domain
	          || mono_method_signature_internal (method)->call_convention
	                 == MONO_CALL_VARARG);

	if (tracing ()) {
		char *name = mono_method_full_name (method, TRUE);

		fprintf (stderr, "[llvm-jit] translating %s (for %s)\n", name,
		         state.domain->friendly_name);
		g_free (name);
	}

	ERROR_DECL (metadata_error);
	MinimalCompile cfg (method, state.domain, metadata_error);

	if (cfg.get ()->header == nullptr)
		return runtime_error (metadata_error);

	std::vector<ExternalSymbol> externals;
	Expected<Function *> function =
		method_to_llvm (module.get (), cfg.get (), method, &externals);
	if (!function)
		return function.takeError ();

	std::string entry = (*function)->getName ().str ();

	if (dumping (entry.c_str ())) {
		dump_il (method, cfg.get ()->header);
		module->print (llvm::errs (), nullptr);
	}

	if (Error err = resolve (state, externals))
		return std::move (err);

	Expected<CompiledMethod> compiled =
		state.jit->compile (ThreadSafeModule (std::move (module),
		                                      ThreadSafeContext (std::move (context))),
		                    entry);
	if (!compiled)
		return compiled.takeError ();

	/*
	 * Filter bodies were compiled alongside the method as `<entry>$filter<i>`;
	 * their entries go into the published clauses.
	 */
	std::vector<std::pair<uint32_t, void *>> filters;

	for (const auto &[name, extent] : compiled->functions) {
		size_t at = name.rfind ("$filter");

		if (at == std::string::npos)
			continue;
		filters.emplace_back (
			(uint32_t) std::stoul (name.substr (at + 7)),
			const_cast<uint8_t *> (extent.first));
	}

	if (Error err = register_jit_info (state.domain, method, cfg.get ()->header,
	                                   *compiled, filters))
		return std::move (err);

	/*
	 * The legacy entry, as a module of its own: the side tables attribute
	 * their records to the one function a module holds, so the thunk cannot
	 * share the body's. It calls the body through the body's stub, which keeps
	 * it valid across repromotions, and it gets jit info of its own so the
	 * unwinder can walk a frame suspended inside it.
	 */
	Expected<void *> thunk_entry = compile_entry_thunk (state, method);

	if (!thunk_entry)
		return thunk_entry.takeError ();

	if (tracing ())
		fprintf (stderr, "[llvm-jit] %s is at %p (enters at %p, for %s)\n",
		         entry.c_str (), compiled->entry, *thunk_entry,
		         state.domain->friendly_name);

	return Compiled { *thunk_entry, compiled->entry };
}

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
		std::make_unique<Module> (symbol_for_code (method), *thunk_context);

	std::vector<ExternalSymbol> thunk_externals;
	MethodLLVMEmitter declarer (thunk_module.get (), cfg.get (), method,
	                            &thunk_externals);
	Expected<Function *> target = declarer.declare (method);

	if (!target)
		return target.takeError ();

	MonoMethodSignature *sig = mono_method_signature_internal (method);

	if (method->string_ctor)
		sig = mono_marshal_get_string_ctor_signature (method);

	std::string thunk_name = symbol_for_code (method);

	create_legacy_entry_thunk (*thunk_module, thunk_name, *target,
	                           legacy_call_flavor (sig));

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

	if (Error err = register_jit_info (state.domain, method, nullptr, *thunk))
		return std::move (err);

	return thunk->entry;
}

Expected<void *>
Backend::compile (MonoMethod *method, MonoDomain *target_domain)
{
	/*
	 * SGen identifies threads suspended inside the managed allocator and the
	 * write barrier by resolving code addresses through the jit-info table,
	 * and the runtime asserts the pointer it hands out for those wrappers
	 * resolves too. The legacy entry carries jit info, so it is what they
	 * get; everything else gets the stub, which is what keeps callers correct
	 * across promotions.
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
	Expected<Compiled> code = ensure_compiled (**state, method);
	if (!code)
		return code.takeError ();

	return wants_body ? code->entry : *stub;
}

Expected<Backend::Compiled>
Backend::ensure_compiled (DomainState &state, MonoMethod *method)
{
	{
		std::lock_guard<std::mutex> lock (mutex_);
		auto it = state.compiled.find (method);

		if (it != state.compiled.end ())
			return it->second;
	}

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

	Expected<Compiled> code = translate_and_compile (state, method);

	if (entered != state.domain)
		mono_domain_set_internal_with_options (entered, FALSE);
	if (!code)
		return code.takeError ();

	if (Error err = state.jit->redirect_stub (symbol_for_code (method), code->entry))
		return std::move (err);
	if (Error err = state.jit->redirect_stub (symbol_for_body (method), code->body))
		return std::move (err);

	/*
	 * Two threads racing here both compile; the loser's code is merely
	 * unreferenced, and both ends stay coherent because the redirects above
	 * always point a method's two stubs at one compile's output.
	 * A stub that had already gone to a dispatcher gets rebound to this
	 * domain's own code here, which is mini's behavior too: a same-domain
	 * resolve patches the call site.
	 */
	std::lock_guard<std::mutex> lock (mutex_);
	state.compiled[method] = *code;
	return *code;
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

	/*
	 * The dispatcher borrows the fastcc body's exact type - signature,
	 * convention, attributes - from a declaration; the accessor substitution
	 * mirrors translate_and_compile (), whose compiled body is what the helper
	 * will return. The declaration itself goes back out of the module: the
	 * forward is through the helper's answer, never through the stub, or the
	 * dispatcher would bounce off its own binding forever.
	 */
	MonoMethod *declared = method;

	if (m_class_get_rank (declared->klass) > 0
	    && (declared->iflags & METHOD_IMPL_ATTRIBUTE_INTERNAL_CALL)
	    && (declared->iflags & METHOD_IMPL_ATTRIBUTE_NATIVE))
		declared = mono_marshal_get_array_accessor_wrapper (declared);

	ERROR_DECL (metadata_error);
	MinimalCompile cfg (declared, state.domain, metadata_error);

	if (cfg.get ()->header == nullptr)
		return runtime_error (metadata_error);

	auto context = std::make_unique<LLVMContext> ();
	std::string name = symbol_for_body (method) + "$dispatch";
	auto module = std::make_unique<Module> (name, *context);

	std::vector<ExternalSymbol> externals;
	MethodLLVMEmitter declarer (module.get (), cfg.get (), declared, &externals);
	Expected<Function *> target = declarer.declare (declared);

	if (!target)
		return target.takeError ();

	FunctionType *type = (*target)->getFunctionType ();
	CallingConv::ID conv = (*target)->getCallingConv ();
	AttributeList attrs = (*target)->getAttributes ();

	if ((*target)->use_empty ())
		(*target)->eraseFromParent ();

	Function *disp =
		Function::Create (type, Function::ExternalLinkage, name, module.get ());

	disp->setCallingConv (conv);
	disp->setAttributes (attrs);

	IRBuilder<> builder (BasicBlock::Create (*context, "", disp));
	PointerType *ptr = PointerType::getUnqual (*context);
	FunctionCallee helper = module->getOrInsertFunction (
		"mono_llvm_jit_body_for_current_domain",
		FunctionType::get (ptr, { ptr }, false));
	Value *self = builder.CreateIntToPtr (
		builder.getInt64 ((uint64_t) (uintptr_t) method), ptr);
	Value *body = builder.CreateCall (helper, { self });

	std::vector<Value *> args;

	for (Argument &arg : disp->args ())
		args.push_back (&arg);

	CallInst *forward = builder.CreateCall (type, body, args);

	forward->setCallingConv (conv);
	forward->setAttributes (attrs);
	forward->setTailCallKind (CallInst::TCK_MustTail);

	if (type->getReturnType ()->isVoidTy ())
		builder.CreateRetVoid ();
	else
		builder.CreateRet (forward);

	Expected<CompiledMethod> compiled = state.jit->compile (
		ThreadSafeModule (std::move (module),
		                  ThreadSafeContext (std::move (context))),
		name);
	if (!compiled)
		return compiled.takeError ();

	if (tracing ())
		fprintf (stderr,
		         "[llvm-jit] %s dispatches per call (owner %s, first reached "
		         "from %s)\n",
		         name.c_str (), state.domain->friendly_name,
		         mono_domain_get ()->friendly_name);

	std::lock_guard<std::mutex> lock (mutex_);
	return state.dispatchers[method] = compiled->entry;
}

void *
Backend::body_for_current_domain (MonoMethod *method)
{
	/*
	 * A dispatcher is the only caller, so the backend exists. Failure lands
	 * where lazy_compile_failed () lands for a stub: the call's arguments are
	 * already in place and there is no caller prepared for a miss.
	 */
	Expected<DomainState *> state = live_backend->state ();

	if (!state)
		report_fatal_error (
			Twine ("no domain state for a dispatched call: ")
				+ toString (state.takeError ()),
			false);

	Expected<Compiled> code = live_backend->ensure_compiled (**state, method);

	if (!code)
		report_fatal_error (
			Twine ("a dispatched method failed to compile: ")
				+ toString (code.takeError ()),
			false);

	return code->body;
}

Error
Backend::publish_defs (DomainState &state, MonoMethod *method)
{
	/*
	 * Held across the creation: two threads reaching an undefined method
	 * together must not both define its stubs. Everything under the lock is
	 * definition only - trampoline, callback unit, stub symbols - and none of
	 * it materializes. The address lookup lives in publish (), outside this
	 * lock, because an ORC lookup drains the session's pending
	 * materializations inline on the calling thread, and a drained lazy-stub
	 * compile re-enters this backend and takes mutex_: holding it across a
	 * lookup deadlocks against ourselves.
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
	 * stands behind them is mini's, one copy for the whole process - and
	 * vararg signatures too, because a dispatcher cannot forward an arglist.
	 */
	DomainState *owner = &state;
	auto bindable = [owner, method] () {
		return mono_domain_get () == owner->domain
		       || implemented_outside_il (method)
		       || mono_method_signature_internal (method)->call_convention
		              == MONO_CALL_VARARG;
	};
	if (Error err = state.jit->create_lazy_stub (
	        symbol_for_code (method),
	        [this, owner, method, bindable] () -> Expected<void *> {
		        if (!bindable ())
			        return compile_entry_thunk (*owner, method);

		        Expected<Compiled> code = ensure_compiled (*owner, method);

		        if (!code)
			        return code.takeError ();
		        return code->entry;
	        }))
		return err;

	if (Error err = state.jit->create_lazy_stub (
	        symbol_for_body (method),
	        [this, owner, method, bindable] () -> Expected<void *> {
		        if (!bindable ())
			        return dispatcher (*owner, method);

		        Expected<Compiled> code = ensure_compiled (*owner, method);

		        if (!code)
			        return code.takeError ();
		        return code->body;
	        }))
		return err;

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
			return it->second;
	}

	if (Error err = publish_defs (state, method))
		return std::move (err);

	/*
	 * Unlocked on purpose - see publish_defs (). Concurrent lookups of one
	 * symbol are ORC's bread and butter; every thread that races through here
	 * computes the same address.
	 */
	Expected<void *> stub = state.jit->stub_address (symbol_for_code (method));
	if (!stub)
		return stub;

	std::lock_guard<std::mutex> lock (mutex_);
	auto it = state.published.find (method);

	if (it != state.published.end ())
		return it->second;

	/*
	 * The stub is the only address the runtime ever hands out for the method,
	 * so anything recovering a method from a code pointer - delegate creation
	 * off a ldftn most visibly - must find it in the jit-info table. Register
	 * it the way mini registers trampolines: an is_trampoline entry carrying
	 * the method, in the domain whose linker holds the stub so the two die
	 * together. The published check above keeps racing threads from
	 * registering it twice.
	 */
	MonoTrampInfo *tramp = g_new0 (MonoTrampInfo, 1);

	tramp->code = (guint8 *) *stub;
	tramp->code_size = (guint32) stub_block_size;
	tramp->name = g_strdup (symbol_for_code (method).c_str ());
	tramp->method = method;
	mono_tramp_info_register (tramp, state.domain);

	state.published[method] = *stub;
	return *stub;
}

} // namespace
} // namespace mono

void
mono_llvm_jit_free_domain (MonoDomain *domain)
{
	mono::Backend::free_domain (domain);
}

void *
mono_llvm_jit_compile_method (MonoMethod *method, MonoDomain *target_domain,
                              MonoError *error)
{
	error_init (error);

	llvm::Expected<mono::Backend *> backend = mono::Backend::get ();
	if (!backend) {
		mono_error_set_execution_engine (error, "%s",
		                                 llvm::toString (backend.takeError ()).c_str ());
		return NULL;
	}

	llvm::Expected<void *> code = (*backend)->compile (method, target_domain);
	if (code)
		return *code;

	/*
	 * A refusal the translator raised through a MonoError is handed back as the
	 * exception it described; anything else is the engine itself failing, which
	 * managed code sees as an ExecutionEngineException all the same.
	 */
	llvm::Error failure = code.takeError ();
	bool recovered = false;

	llvm::handleAllErrors (
		std::move (failure),
		[&] (mono::RuntimeError &runtime) {
			runtime.move_to (error);
			recovered = true;
		},
		[&] (const llvm::ErrorInfoBase &other) {
			mono_error_set_execution_engine (error, "%s", other.message ().c_str ());
			recovered = true;
		});

	g_assert (recovered);
	return NULL;
}

void
mono_llvm_jit_add_option (const char *opt)
{
	mono::MonoJit::add_option (opt);
}

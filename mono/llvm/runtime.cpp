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

#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>

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
	MinimalCompile (MonoMethod *method, MonoError *error)
	{
		memset (&cfg, 0, sizeof (cfg));
		cfg.method = method;
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

	/// Compile METHOD now and return the address of its stub.
	Expected<void *> compile (MonoMethod *method);

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
		std::unique_ptr<MonoJit> jit;
		/// Methods already published, so a callee reached from several places
		/// is only given its stubs once. Holds the legacy stub's address.
		std::unordered_map<MonoMethod *, void *> published;
		/// Methods whose stubs already point at real code - and where that
		/// code is, so that asking again is a lookup rather than a compile.
		std::unordered_map<MonoMethod *, Compiled> compiled;
	};

	/// The current domain's state, created - linker, helpers and all - the
	/// first time the domain compiles something.
	Expected<DomainState *> state ();

	Error resolve (DomainState &state, const std::vector<ExternalSymbol> &externals);
	Expected<Compiled> translate_and_compile (DomainState &state, MonoMethod *method);
	Expected<Compiled> ensure_compiled (DomainState &state, MonoMethod *method);

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
	MonoDomain *domain = mono_domain_get ();
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

	auto fresh = std::make_unique<DomainState> ();

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
	MonoDomain *domain = mono_domain_get ();

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

	if (tracing ()) {
		char *name = mono_method_full_name (method, TRUE);

		fprintf (stderr, "[llvm-jit] translating %s\n", name);
		g_free (name);
	}

	ERROR_DECL (metadata_error);
	MinimalCompile cfg (method, metadata_error);

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

	if (Error err = register_jit_info (method, cfg.get ()->header, *compiled, filters))
		return std::move (err);

	/*
	 * The legacy entry, as a module of its own: the side tables attribute
	 * their records to the one function a module holds, so the thunk cannot
	 * share the body's. It calls the body through the body's stub, which keeps
	 * it valid across repromotions, and it gets jit info of its own so the
	 * unwinder can walk a frame suspended inside it.
	 */
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

	if (dumping (entry.c_str ()))
		thunk_module->print (llvm::errs (), nullptr);

	if (Error err = resolve (state, thunk_externals))
		return std::move (err);

	Expected<CompiledMethod> thunk = state.jit->compile (
		ThreadSafeModule (std::move (thunk_module),
		                  ThreadSafeContext (std::move (thunk_context))),
		thunk_name);
	if (!thunk)
		return thunk.takeError ();

	if (Error err = register_jit_info (method, nullptr, *thunk))
		return std::move (err);

	if (tracing ())
		fprintf (stderr, "[llvm-jit] %s is at %p (enters at %p)\n",
		         entry.c_str (), compiled->entry, thunk->entry);

	return Compiled { thunk->entry, compiled->entry };
}

Expected<void *>
Backend::compile (MonoMethod *method)
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

	Expected<DomainState *> state = this->state ();
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

	Expected<Compiled> code = translate_and_compile (state, method);
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
	 */
	std::lock_guard<std::mutex> lock (mutex_);
	state.compiled[method] = *code;
	return *code;
}

Expected<void *>
Backend::publish (DomainState &state, MonoMethod *method)
{
	/*
	 * Held across the creation, not just the lookup: two threads reaching an
	 * unpublished method together must not both define its stubs, and stub
	 * creation is cheap enough that one lock for the whole step is fine.
	 */
	std::lock_guard<std::mutex> lock (mutex_);

	auto it = state.published.find (method);
	if (it != state.published.end ())
		return it->second;

	/*
	 * The lambdas capture the owning state rather than looking the domain up
	 * again: a stub is only reachable from its own domain's code, and the state
	 * outlives the stub - free_domain () takes them down together.
	 */
	DomainState *owner = &state;
	Expected<void *> stub = state.jit->create_lazy_stub (
		symbol_for_code (method), [this, owner, method] () -> Expected<void *> {
			Expected<Compiled> code = ensure_compiled (*owner, method);

			if (!code)
				return code.takeError ();
			return code->entry;
		});
	if (!stub)
		return stub;

	Expected<void *> body_stub = state.jit->create_lazy_stub (
		symbol_for_body (method), [this, owner, method] () -> Expected<void *> {
			Expected<Compiled> code = ensure_compiled (*owner, method);

			if (!code)
				return code.takeError ();
			return code->body;
		});
	if (!body_stub)
		return body_stub.takeError ();

	/*
	 * The stub is the only address the runtime ever hands out for the method,
	 * so anything recovering a method from a code pointer - delegate creation
	 * off a ldftn most visibly - must find it in the jit-info table. Register
	 * it the way mini registers trampolines: an is_trampoline entry carrying
	 * the method, in the domain whose linker holds the stub so the two die
	 * together.
	 */
	MonoTrampInfo *tramp = g_new0 (MonoTrampInfo, 1);

	tramp->code = (guint8 *) *stub;
	tramp->code_size = (guint32) stub_block_size;
	tramp->name = g_strdup (symbol_for_code (method).c_str ());
	tramp->method = method;
	mono_tramp_info_register (tramp, mono_domain_get ());

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
mono_llvm_jit_compile_method (MonoMethod *method, MonoError *error)
{
	error_init (error);

	llvm::Expected<mono::Backend *> backend = mono::Backend::get ();
	if (!backend) {
		mono_error_set_execution_engine (error, "%s",
		                                 llvm::toString (backend.takeError ()).c_str ());
		return NULL;
	}

	llvm::Expected<void *> code = (*backend)->compile (method);
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

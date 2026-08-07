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
#include "jinfo.hpp"
#include "jit.hpp"
#include "stubs.hpp"
#include "timing.hpp"
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
#include "mono/utils/mono-tls-inline.h"

// This breaks some LLVM headers
#undef PIC

#include <llvm/ADT/Twine.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/RuntimeLibcalls.h>
#include <llvm/Support/DynamicLibrary.h>
#include <llvm/Support/ErrorHandling.h>
#include <llvm/TargetParser/Triple.h>

#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <unwind.h>
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

/*
 * Re-create the failure a method's metadata carried as the exception its call
 * site is owed. The box is metadata and lives in the assembly's mempool; the
 * exception is not, so every call through a stand-in body builds its own, which
 * is what mono does everywhere else it raises a boxed class failure.
 */
/*
 * The personality routine every method with a handler names.
 *
 * mono finds a handler by searching the MonoJitInfo it publishes per method, so
 * nothing on the managed path ever calls this. What does call it is a foreign
 * unwinder walking a JIT'd frame: glibc implements pthread_exit as
 * _Unwind_ForcedUnwind, and libgcc finds the frame through the FDEs ORC
 * registered. A forced unwind runs no managed finally blocks, so there is nothing
 * to do here beyond letting it carry on - and it has to be _URC_CONTINUE_UNWIND
 * rather than _URC_NO_REASON, which phase 2 reads as a fatal error.
 */
_Unwind_Reason_Code
jit_personality (int, _Unwind_Action, _Unwind_Exception_Class, struct _Unwind_Exception *,
                 struct _Unwind_Context *)
{
	return _URC_CONTINUE_UNWIND;
}

MonoObject *
load_error_exception (MonoErrorBoxed *failure)
{
	ERROR_DECL (error);

	mono_error_set_from_boxed (error, failure);

	return (MonoObject *) mono_error_convert_to_exception (error);
}

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
		 * What a stand-in body for a method whose metadata would not load
		 * calls to build the exception it then throws.
		 */
		{ "mono_llvm_load_error_exception", (void *) &load_error_exception },

		/*
		 * The personality routine a landing pad names. Generated code never
		 * calls it; the unwinder does, on the way through a frame that has a
		 * handler.
		 */
		{ "mono_personality", (void *) &jit_personality },

		/*
		 * Not a runtime libcall as far as RuntimeLibcallsInfo is concerned -
		 * amd64 has no MEMCMP libcall - so resolvable_libcalls () does not
		 * cover it, but MergeICmps builds calls to it at the IR level.
		 */
		{ "memcmp", (void *) &memcmp },
	};
}

/*
 * The runtime libcalls this process can satisfy: every name codegen is allowed to
 * synthesize for TRIPLE that something already loaded defines, minus whatever
 * TAKEN spells out for itself.
 *
 * The translator never names any of these - codegen invents the calls during
 * lowering, so the first anyone hears of one is a materialization failure. Asking
 * LLVM which names it might invent is the only way to get ahead of that; the list
 * is generated from the same tables lowering picks from, so it tracks the LLVM the
 * backend is built against instead of being maintained by hand.
 *
 * Names that resolve to nothing are dropped rather than diagnosed. Most of what
 * amd64 declares available is soft-float and small-integer arithmetic it has
 * instructions for, or the __atomic_/__sync_ families that live in a libatomic
 * nothing here links, and none of it is reachable in practice.
 */
std::vector<Helper>
resolvable_libcalls (const Triple &triple, const std::vector<Helper> &taken)
{
	std::unordered_set<std::string> skip;
	for (const Helper &helper : taken)
		skip.insert (helper.name);

	/* Without this the search below only sees libraries LLVM itself opened. */
	sys::DynamicLibrary::LoadLibraryPermanently (nullptr);

	RTLIB::RuntimeLibcallsInfo libcalls (triple);
	std::vector<Helper> resolved;

	for (RTLIB::LibcallImpl impl : RTLIB::libcall_impls ()) {
		if (!libcalls.isAvailable (impl))
			continue;

		StringRef name = RTLIB::RuntimeLibcallsInfo::getLibcallImplName (impl);
		if (name.empty () || skip.count (name.str ()))
			continue;

		if (void *addr = sys::DynamicLibrary::SearchForAddressOfSymbol (name.str ()))
			resolved.push_back ({ name.data (), addr });
	}

	return resolved;
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

/// Whether MONO_LLVM_JIT_RECOMPILE names METHOD: a substring of its full name
/// selects it for being translated afresh on every compile request rather than
/// answered from the cache, which is what gives a method more than one live
/// body. It is a way to exercise the paths that have to cope with that - the
/// debugger arming a breakpoint everywhere a method is executing - rather than
/// a tiering policy.
bool
recompiling (MonoMethod *method)
{
	static const char *filter = g_getenv ("MONO_LLVM_JIT_RECOMPILE");

	if (filter == nullptr)
		return false;

	char *name = mono_method_full_name (method, TRUE);
	bool selected = strstr (name, filter) != nullptr;

	g_free (name);
	return selected;
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

/// The plain symbol: the fastcc body, which is the method's implementation and
/// what generated callers bind to. Every other symbol a method owns hangs a
/// suffix off this one.
std::string
symbol_for_body (MonoMethod *method)
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

/// The `$legacy` symbol: the legacy-convention entry - the interop thunk - which
/// is what the runtime, delegates and every escaped function pointer see. It is
/// the stub every module binds to and the address publish () hands out.
std::string
symbol_for_code (MonoMethod *method)
{
	return symbol_for_body (method) + "$legacy";
}

/// The `$entry` symbol: the legacy entry compiled beside a method's body.
///
/// This is a definition, not the stub above, and it needs a name of its own
/// because the two share a module: a body that takes its own address emits the
/// `$legacy` symbol and must get the stub, which survives every later recompile,
/// rather than the copy sitting next to it.
std::string
symbol_for_entry (MonoMethod *method)
{
	return symbol_for_body (method) + "$entry";
}

/// The `$unbox` symbol: the legacy entry that steps a boxed receiver past the
/// object header before entering the method. This is the stub - a value type's
/// vtable slots are filled from it, so it has to survive a promotion the same
/// way the ordinary entry does.
std::string
symbol_for_unbox (MonoMethod *method)
{
	return symbol_for_body (method) + "$unbox";
}

/// The `$unboxentry` symbol: the unboxing entry compiled beside a method's
/// body. A definition rather than the stub above, for the same reason `$entry`
/// is not `$legacy`.
std::string
symbol_for_unbox_entry (MonoMethod *method)
{
	return symbol_for_body (method) + "$unboxentry";
}

/// Whether a call can ever reach METHOD with a boxed receiver, so that the
/// method wants an unboxing entry beside its ordinary one.
///
/// Every such call comes off a value type's vtable or its IMT, which is
/// exactly where the runtime asks for the unboxing address.
bool
wants_unbox_entry (MonoMethod *method, MonoMethodSignature *sig)
{
	return sig != nullptr && sig->hasthis && m_class_is_valuetype (method->klass);
}

/// Whether this backend gives METHOD an unboxing entry of its own, and so a
/// stub to publish it through. A method not implemented in IL is entered
/// through code this backend did not generate; the runtime wraps those itself.
bool
publishes_unbox_entry (MonoMethod *method)
{
	return !implemented_outside_il (method)
	       && wants_unbox_entry (method,
	                             mono_method_signature_internal (method));
}

/// Which methods --interp-tier0 selected, or null when it was not given. An
/// empty filter takes every method that can be interpreted at all; anything
/// else is matched as a substring of the printed name.
const char *interp_tier0_filter = nullptr;

/// Whether METHOD is entered by interpreting its bytecode rather than by
/// compiling it.
///
/// The refusals below are the ones that would be wrong rather than merely
/// slow:
///
///  - a method not implemented in IL has no bytecode of its own, and reaching
///    one goes back through mono_jit_compile_method;
///  - the allocator and write-barrier wrappers are handed out as raw entries
///    rather than as stubs, because SGen identifies a thread suspended in one
///    by resolving the address through the jit-info table. Nothing stands
///    between them and their callers that a later tier could redirect, so an
///    interpreted one would stay interpreted for good;
///  - a wrapper is generated for the runtime to enter natively, and several
///    kinds of them the interpreter answers for with something that is not a
///    callable address at all;
///  - freeing a dynamic method hands its MonoMethod back to the allocator,
///    while the shims below are shared between methods and outlive any one of
///    them.
///
/// AggressiveInlining goes straight to the compiler because that is what the
/// attribute asks for, even though nothing is inlined across methods yet.
bool
runs_at_tier0 (MonoMethod *method)
{
	if (interp_tier0_filter == nullptr || !mono_use_interpreter)
		return false;

	if (implemented_outside_il (method) || method->dynamic
	    || method->wrapper_type != MONO_WRAPPER_NONE
	    || (method->iflags & METHOD_IMPL_ATTRIBUTE_AGGRESSIVE_INLINING) != 0)
		return false;

	if (*interp_tier0_filter == '\0')
		return true;

	char *name = mono_method_full_name (method, TRUE);
	bool selected = strstr (name, interp_tier0_filter) != nullptr;

	g_free (name);
	return selected;
}

/// A key naming everything about F's prototype that decides how a call to it is
/// laid out - the types, the attributes that move a value, and which side of the
/// boundary the callee behind it speaks for. Two methods that agree on this can
/// share one thunk between them, and nothing narrower is safe to share on.
std::string
prototype_key (Function *f, arch::LegacyFlavor flavor)
{
	std::string key;
	raw_string_ostream os (key);
	AttributeList attrs = f->getAttributes ();

	f->getFunctionType ()->print (os);
	os << "|cc" << f->getCallingConv () << "|flavor" << (int) flavor << "|ret "
	   << attrs.getRetAttrs ().getAsString ();
	for (unsigned i = 0; i < f->getFunctionType ()->getNumParams (); ++i)
		os << "|p" << i << " " << attrs.getParamAttrs (i).getAsString ();
	os.flush ();
	return key;
}

MonoJitInfo *register_stub_jinfo (MonoDomain *domain, MonoMethod *method,
                                  void *stub, size_t size,
                                  const std::string &name);

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

	/// Select the methods entered by interpreting them: FILTER is matched as a
	/// substring of the printed name, and an empty one takes every method that
	/// can be interpreted at all.
	static void set_interp_filter (const char *filter);

private:
	/// Where one method's code ended up: the legacy entry the runtime hands
	/// out, the fastcc body generated callers reach, and - for an instance
	/// method of a value type - the unboxing entry a call off that value
	/// type's vtable comes in through. JINFO is the body's record, and is null
	/// for a method mini compiled instead.
	struct Compiled {
		void *entry;
		void *body;
		void *unbox = nullptr;
		MonoJitInfo *jinfo = nullptr;
	};

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
		std::unique_ptr<MonoJit> jit;
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
		/// The fastcc-to-interpreter shims compiled here, by the prototype
		/// each serves. One shim stands for every method with that prototype.
		std::unordered_map<std::string, void *> shims;
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

	/// Compile a body for METHOD that raises FAILURE and nothing else. Consumes
	/// FAILURE.
	Expected<Compiled> compile_thrower (DomainState &state, MonoMethod *method,
	                                    MonoError *failure);

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

	/// Point METHOD's stubs at the interpreter, LEGACY being the entry the
	/// interpreter itself handed out for it.
	Expected<Compiled> interp_entries (DomainState &state, MonoMethod *method,
	                                   void *legacy);

	/// The fastcc entry an interpreted METHOD is given: a stub that hands
	/// LEGACY to the shim compiled for METHOD's prototype, which is where the
	/// arguments are marshalled across.
	Expected<void *> interp_body_entry (DomainState &state, MonoMethod *method,
	                                    void *legacy);

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

	std::vector<Helper> helpers = runtime_helpers ();
	for (const Helper &helper : helpers)
		if (Error err = (*jit)->register_symbol (helper.name, helper.address))
			return std::move (err);

	std::vector<Helper> libcalls = resolvable_libcalls ((*jit)->triple (), helpers);
	for (const Helper &libcall : libcalls)
		if (Error err = (*jit)->register_symbol (libcall.name, libcall.address))
			return std::move (err);
	if (tracing ())
		fprintf (stderr, "[llvm-jit] %zu runtime libcalls registered\n",
		         libcalls.size ());

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
		(*state)->jit->stub_address (symbol_for_unbox (method));

	if (!unbox) {
		consumeError (unbox.takeError ());
		return nullptr;
	}

	return *unbox;
}

void
Backend::set_interp_filter (const char *filter)
{
	interp_tier0_filter = filter;
}

void
Backend::remember (DomainState &state, MonoMethod *method,
                   const CompiledMethod &compiled, MonoJitInfo *jinfo)
{
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
			symbol_for_body (method) + "$linker_stubs" + std::to_string (i);
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

	/* One domain's share of the release: gathered under the lock, carried out
	 * after it, because both removals take the ORC session lock. */
	struct Release {
		MonoDomain *domain;
		MonoJit *jit;
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

			Release release { state.domain, state.jit.get (), {}, {} };

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
				release.stubs.push_back (symbol_for_code (method));
				release.stubs.push_back (symbol_for_body (method));
				if (publishes_unbox_entry (method))
					release.stubs.push_back (
						symbol_for_unbox (method));
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
		if (tracing ()) {
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
		must (release.jit->undefine_stubs (release.stubs));
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
Backend::translate_and_compile (DomainState &state, MonoMethod *method,
                                MonoJitInfo **published)
{
	*published = nullptr;

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

	if (implemented_outside_il (method)) {
		ERROR_DECL (compile_error);
		void *code = mono_jit_compile_method (method, compile_error);

		if (code == nullptr)
			return runtime_error (compile_error);

		/*
		 * mini's code is the legacy convention; generated code declares such
		 * a method against the plain symbol and lowers its calls, so nothing
		 * ever reaches the body stub expecting fastcc. The profiler hears
		 * about this one from mono_jit_compile_method_with_opt (), which
		 * reports the declaration against the wrapper it built - and that
		 * wrapper came back through here and bracketed itself.
		 */
		return Compiled { code, code };
	}

	/*
	 * This is the one place a method is translated, so it is where the
	 * profiler's compilation of it begins. Exactly one end follows every
	 * begin: a consumer pairing the two would otherwise carry an open span for
	 * the rest of the process. A method whose metadata would not load gets a
	 * stand-in body that raises instead of a translation, and that is a failed
	 * compile however it is served. The successful end is raised by the caller,
	 * once the method can be looked up - PUBLISHED being non-null is what says
	 * one is owed.
	 */
	MONO_PROFILER_RAISE (jit_begin, (method));

	Expected<Compiled> code = translate_body (state, method, published);

	if (!code || *published == nullptr) {
		*published = nullptr;
		MONO_PROFILER_RAISE (jit_failed, (method));
	}

	return code;
}

Expected<Backend::Compiled>
Backend::translate_body (DomainState &state, MonoMethod *method,
                         MonoJitInfo **published)
{
	std::unique_ptr<LLVMContext> context;
	std::unique_ptr<Module> module;

	{
		timing::Scope timed (timing::Phase::ctxnew);

		context = std::make_unique<LLVMContext> ();
		module = std::make_unique<Module> (symbol_for_body (method), *context);
	}

	/*
	 * Translation itself resolves everything per-domain against state.domain
	 * (resolve (), ldstr through cfg->domain) - never against the thread's
	 * current domain, and any new translate-time mono_domain_get () is a
	 * cross-domain bug of the kind the dispatcher exists to prevent. The
	 * stub lambdas keep the two equal.
	 */
	g_assert (mono_domain_get () == state.domain);

	if (tracing ()) {
		char *name = mono_method_full_name (method, TRUE);

		fprintf (stderr, "[llvm-jit] translating %s (for %s)\n", name,
		         state.domain->friendly_name);
		g_free (name);
	}

	ERROR_DECL (metadata_error);
	std::optional<MinimalCompile> cfg;

	{
		timing::Scope timed (timing::Phase::metadata);

		cfg.emplace (method, state.domain, metadata_error);
	}

	if (cfg->get ()->header == nullptr)
		return recover (state, method, runtime_error (metadata_error));

	std::vector<ExternalSymbol> externals;
	MonoLLVMBreakpointSwitch *bp_switch = nullptr;
	SeqPointGraph seq_points;
	Expected<Function *> function = [&] {
		timing::Scope timed (timing::Phase::translate);

		return method_to_llvm (module.get (), cfg->get (), method, &externals,
		                       &bp_switch, &seq_points);
	}();
	if (!function)
		return recover (state, method, function.takeError ());

	std::string entry = (*function)->getName ().str ();

	if (dumping (entry.c_str ()))
		dump_il (method, cfg->get ()->header);

	/*
	 * Laying out a class to create its vtable is the other place metadata gets
	 * loaded, and the last one: a method whose callee's class will not load
	 * fails here rather than in the translation above. It is the same failure
	 * and it is raised the same way - at the call, not at the declaration.
	 */
	Error resolved = [&] {
		timing::Scope timed (timing::Phase::resolve);

		return resolve (state, externals);
	}();

	if (resolved)
		return recover (state, method, std::move (resolved));

	/*
	 * The legacy entry rides along in the body's module. It is a handful of
	 * instructions, and a module of its own would cost a whole second pass
	 * pipeline, codegen and link to hold them.
	 *
	 * It calls the body's stub rather than the definition beside it, which is
	 * what keeps it correct across a later recompile - the stub is redirected
	 * and the entry follows it without being rebuilt. Translating the method
	 * declared it, so resolve () above has published that stub.
	 */
	Expected<void *> body_stub = state.jit->stub_address (symbol_for_body (method));

	if (!body_stub)
		return body_stub.takeError ();

	MonoMethodSignature *sig = mono_method_signature_internal (method);

	if (method->string_ctor)
		sig = mono_marshal_get_string_ctor_signature (method);

	Constant *body_address = ConstantExpr::getIntToPtr (
		ConstantInt::get (Type::getInt64Ty (*context),
		                  (uint64_t) (uintptr_t) *body_stub),
		PointerType::get (*context, 0));
	std::string legacy_entry = symbol_for_entry (method);

	arch::create_legacy_entry_thunk (*module, legacy_entry, *function,
	                                 legacy_call_flavor (sig), body_address);

	/*
	 * Reached off a value type's vtable, the method arrives with the boxed
	 * object as its receiver instead of the value. That is a second entry
	 * rather than a branch inside the first: which one a caller wants is
	 * settled when the address is handed out, and the ordinary entry must not
	 * pay for the question.
	 */
	std::string unbox_entry;

	if (wants_unbox_entry (method, sig)) {
		unbox_entry = symbol_for_unbox_entry (method);
		arch::create_legacy_entry_thunk (*module, unbox_entry, *function,
		                                 legacy_call_flavor (sig), body_address,
		                                 MONO_ABI_SIZEOF (MonoObject));
	}

	if (dumping (entry.c_str ()))
		module->print (llvm::errs (), nullptr);

	Expected<CompiledMethod> compiled = [&] {
		timing::Scope timed (timing::Phase::orc);

		return state.jit->compile (
			ThreadSafeModule (std::move (module),
		                      ThreadSafeContext (std::move (context))),
			entry);
	}();
	if (!compiled)
		return compiled.takeError ();

	/*
	 * Filter bodies were compiled alongside the method as `<entry>$filter<i>`;
	 * their entries go into the published clauses. The legacy entry rode along
	 * too, and is what the runtime is handed for the method.
	 */
	std::vector<std::pair<uint32_t, void *>> filters;
	const uint8_t *entry_code = nullptr;
	size_t entry_code_size = 0;
	const uint8_t *unbox_code = nullptr;
	size_t unbox_code_size = 0;

	for (const auto &[name, extent] : compiled->functions) {
		if (name == legacy_entry) {
			entry_code = extent.first;
			entry_code_size = extent.second;
			continue;
		}

		if (!unbox_entry.empty () && name == unbox_entry) {
			unbox_code = extent.first;
			unbox_code_size = extent.second;
			continue;
		}

		size_t at = name.rfind ("$filter");

		if (at == std::string::npos)
			continue;
		filters.emplace_back (
			(uint32_t) std::stoul (name.substr (at + 7)),
			const_cast<uint8_t *> (extent.first));
	}

	if (entry_code == nullptr)
		return createStringError (inconvertibleErrorCode (),
		                          "the linked object for %s defines no legacy "
		                          "entry", entry.c_str ());

	Expected<MonoJitInfo *> jinfo = [&] {
		timing::Scope timed (timing::Phase::jinfo);

		return register_jit_info (state.domain, method, cfg->get ()->header,
		                          *compiled, CodeKind::Body, filters, bp_switch,
		                          seq_points);
	}();

	if (!jinfo)
		return jinfo.takeError ();
	remember (state, method, *compiled, *jinfo);
	*published = *jinfo;

	/*
	 * A record of its own for each of the module's other functions: an
	 * exception unwinding out of the body passes back through the entries, a
	 * suspended thread can be stopped in any of them, and a filter body is a
	 * frame something walking the stack has to be able to name. They carry no
	 * clauses, so their frame description - and, for a filter, the IL-offset
	 * map that says where in the method's IL its frame is - is all they take
	 * from the module's side tables. No dylib either: the body's record
	 * already owns the one they all share.
	 */
	auto register_side_body = [&] (const uint8_t *code, size_t size, CodeKind kind,
	                               std::vector<IlLineRow> lines) -> Error {
		CompiledMethod side;

		side.entry = const_cast<uint8_t *> (code);
		side.code = code;
		side.code_size = size;
		side.unwind_table = compiled->unwind_table;
		side.unwind_table_size = compiled->unwind_table_size;
		side.il_lines = std::move (lines);

		Expected<MonoJitInfo *> jinfo =
			register_jit_info (state.domain, method, nullptr, side, kind);

		if (!jinfo)
			return jinfo.takeError ();
		remember (state, method, side, *jinfo);
		return Error::success ();
	};

	if (Error err = register_side_body (entry_code, entry_code_size,
	                                    CodeKind::AbiThunk, {}))
		return std::move (err);

	if (unbox_code != nullptr) {
		if (Error err = register_side_body (unbox_code, unbox_code_size,
		                                    CodeKind::AbiThunk, {}))
			return std::move (err);
	} else if (!unbox_entry.empty ()) {
		return createStringError (inconvertibleErrorCode (),
		                          "the linked object for %s defines no unboxing "
		                          "entry", entry.c_str ());
	}

	for (const auto &[name, extent] : compiled->functions) {
		if (name.find ("$filter") == std::string::npos)
			continue;

		std::vector<IlLineRow> lines;

		for (auto &rows : compiled->other_il_lines)
			if (rows.first == name) {
				lines = std::move (rows.second);
				break;
			}

		if (Error err = register_side_body (extent.first, extent.second,
		                                    CodeKind::Body, std::move (lines)))
			return std::move (err);
	}

	if (tracing ())
		fprintf (stderr, "[llvm-jit] %s is at %p (enters at %p, for %s)\n",
		         entry.c_str (), compiled->entry, entry_code,
		         state.domain->friendly_name);

	return Compiled { const_cast<uint8_t *> (entry_code), compiled->entry,
		              const_cast<uint8_t *> (unbox_code), *jinfo };
}

/*
 * The metadata failures ECMA-335 raises where the thing is used rather than
 * where it is named: a method calling one that is missing gets to run until the
 * call, and its caller gets to catch what the call throws. Deferring one costs
 * nothing, because the name it could not resolve is only ever consulted by a
 * call.
 *
 * Invalid IL is not in the set. A body's validity is the answer to the question
 * "can this method be compiled", so whoever asked for the compile is entitled to
 * hear it: mini reports it through the MonoError, which is why creating a
 * delegate over a malformed DynamicMethod throws from Delegate.CreateDelegate
 * rather than from the first call through it. A call that arrives at a stub
 * without anyone having asked still gets the deferral it needs - raise_on_call ()
 * defers everything, that being the only answer available there.
 */
bool
raised_where_used (uint16_t code)
{
	switch (code) {
	case MONO_ERROR_MISSING_METHOD:
	case MONO_ERROR_MISSING_FIELD:
	case MONO_ERROR_TYPE_LOAD:
	case MONO_ERROR_FILE_NOT_FOUND:
	case MONO_ERROR_BAD_IMAGE:
	case MONO_ERROR_MEMBER_ACCESS:
		return true;
	default:
		return false;
	}
}

Expected<Backend::Compiled>
Backend::recover (DomainState &state, MonoMethod *method, Error failure)
{
	if (!failure.isA<RuntimeError> ())
		return std::move (failure);

	ERROR_DECL (metadata_error);

	handleAllErrors (std::move (failure),
	                 [&] (RuntimeError &runtime) { runtime.move_to (metadata_error); });

	if (!raised_where_used (mono_error_get_error_code (metadata_error)))
		return runtime_error (metadata_error);

	return compile_thrower (state, method, metadata_error);
}

/*
 * A stub is the end of the line for a failure. The trampoline behind it has
 * already put the call's arguments back and there is no caller expecting a
 * miss, so a failure that gets this far either becomes an exception the program
 * can see or ends the process. Which means the choice recover () makes - defer
 * this one, report that one - is not available here: everything is deferred,
 * and a failure that never went through a MonoError becomes the
 * ExecutionEngineException managed code sees for an engine that gave up. A
 * symbol that failed to resolve then costs the one method that named it rather
 * than every method in the process.
 */
Expected<Backend::Compiled>
Backend::raise_on_call (DomainState &state, MonoMethod *method, Error failure)
{
	ERROR_DECL (call_error);

	if (failure.isA<RuntimeError> ())
		handleAllErrors (std::move (failure),
		                 [&] (RuntimeError &runtime) { runtime.move_to (call_error); });
	else
		mono_error_set_execution_engine (call_error, "%s",
		                                 toString (std::move (failure)).c_str ());

	return compile_thrower (state, method, call_error);
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
Expected<Backend::Compiled>
Backend::compile_thrower (DomainState &state, MonoMethod *method, MonoError *failure)
{
	MonoErrorBoxed *boxed =
		mono_error_box (failure, m_class_get_image (method->klass));

	if (boxed == nullptr)
		return runtime_error (failure);

	if (tracing ()) {
		char *name = mono_method_full_name (method, TRUE);

		fprintf (stderr, "[llvm-jit] %s throws on call: %s\n", name,
		         mono_error_get_message (failure));
		g_free (name);
	}

	mono_error_cleanup (failure);

	/*
	 * The entry and the body are separate symbols the runtime redirects
	 * independently, and here they stand for the same three instructions - so
	 * this is that body, built twice under the two names.
	 */
	auto build = [&] (const std::string &name) -> Expected<void *> {
		auto context = std::make_unique<LLVMContext> ();
		auto module = std::make_unique<Module> (name, *context);
		LLVMContext &ctx = *context;
		Type *ptr = PointerType::get (ctx, 0);

		FunctionCallee load = module->getOrInsertFunction (
			"mono_llvm_load_error_exception",
			FunctionType::get (ptr, { ptr }, false));
		FunctionCallee raise = module->getOrInsertFunction (
			"mono_llvm_throw_exception",
			FunctionType::get (Type::getVoidTy (ctx), { ptr }, false));

		Function *function =
			Function::Create (FunctionType::get (Type::getVoidTy (ctx), false),
			                  GlobalValue::ExternalLinkage, name, module.get ());

		/* Mono walks this frame like any other, from its unwind record. */
		function->setUWTableKind (UWTableKind::Default);

		IRBuilder<> builder (BasicBlock::Create (ctx, "entry", function));
		Value *box = builder.CreateIntToPtr (
			builder.getInt64 ((uint64_t) (uintptr_t) boxed), ptr);

		builder.CreateCall (raise, { builder.CreateCall (load, { box }) });
		builder.CreateUnreachable ();

		if (dumping (name.c_str ()))
			module->print (llvm::errs (), nullptr);

		Expected<CompiledMethod> compiled = state.jit->compile (
			ThreadSafeModule (std::move (module),
			                  ThreadSafeContext (std::move (context))),
			name);
		if (!compiled)
			return compiled.takeError ();

		Expected<MonoJitInfo *> jinfo = register_jit_info (
			state.domain, method, nullptr, *compiled, CodeKind::Body);

		if (!jinfo)
			return jinfo.takeError ();
		remember (state, method, *compiled, *jinfo);

		return compiled->entry;
	};

	Expected<void *> entry = build (symbol_for_code (method));

	if (!entry)
		return entry.takeError ();

	Expected<void *> body = build (symbol_for_body (method));

	if (!body)
		return body.takeError ();

	/* Whatever a caller does with the receiver, this body never reads it. */
	return Compiled { *entry, *body, *entry };
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

	arch::create_legacy_entry_thunk (*thunk_module, thunk_name, *target,
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
	Expected<Compiled> code =
		ensure_entries (**state, method, recompiling (method));
	if (!code)
		return code.takeError ();

	return wants_body ? code->entry : *stub;
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

	if (runs_at_tier0 (method)) {
		ERROR_DECL (interp_error);
		void *legacy = mini_get_interp_callbacks ()->create_method_pointer (
			method, TRUE, interp_error);

		/*
		 * The interpreter refusing a method is an answer, not a failure: it
		 * gets compiled like anything else. Only what happens after it has
		 * agreed to run the method is an error worth raising.
		 */
		if (legacy == nullptr) {
			mono_error_cleanup (interp_error);
		} else {
			Expected<Compiled> entries =
				interp_entries (state, method, legacy);

			leave ();
			return entries;
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

	if (Error err = state.jit->redirect_stub (symbol_for_code (method), code->entry))
		return give_up (std::move (err));
	if (Error err = state.jit->redirect_stub (symbol_for_body (method), code->body))
		return give_up (std::move (err));
	if (publishes_unbox_entry (method)) {
		if (Error err = state.jit->redirect_stub (symbol_for_unbox (method),
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

	return *code;
}

Expected<Backend::Compiled>
Backend::interp_entries (DomainState &state, MonoMethod *method, void *legacy)
{
	Expected<void *> body = interp_body_entry (state, method, legacy);

	if (!body)
		return body.takeError ();

	/*
	 * The receiver a value type's vtable slot arrives with is the boxed object,
	 * and stepping it past the header is the whole of the difference - so this
	 * is the runtime's own unboxing trampoline over the interpreter's entry,
	 * the same one a method whose code the backend did not generate gets.
	 */
	Compiled entries { legacy, *body,
		           publishes_unbox_entry (method)
		                   ? mono_arch_get_unbox_trampoline (method, legacy)
		                   : nullptr };

	/*
	 * Three stores rather than one, so a caller can see a method's stubs
	 * disagree about which of them has been pointed somewhere yet. That is
	 * harmless while the alternative to each is the lazy trampoline it
	 * replaces, and two threads arriving together cannot disagree about more
	 * than that: whether a method runs here or is compiled is settled by the
	 * method, so both of them take this branch or neither does.
	 */
	if (Error err =
	            state.jit->redirect_stub (symbol_for_code (method), entries.entry))
		return std::move (err);
	if (Error err =
	            state.jit->redirect_stub (symbol_for_body (method), entries.body))
		return std::move (err);
	if (entries.unbox != nullptr) {
		if (Error err = state.jit->redirect_stub (symbol_for_unbox (method),
		                                          entries.unbox))
			return std::move (err);
	}

	if (tracing ()) {
		char *name = mono_method_full_name (method, TRUE);

		fprintf (stderr, "[llvm-jit] interpreting %s (for %s)\n", name,
		         state.domain->friendly_name);
		g_free (name);
	}

	std::lock_guard<std::mutex> lock (mutex_);

	return state.interpreted[method] = entries;
}

Expected<void *>
Backend::interp_body_entry (DomainState &state, MonoMethod *method, void *legacy)
{
	/*
	 * The shim is written against the prototype rather than against the
	 * method, so it is built out of a declaration of one and the declaration
	 * then goes back out of the module - nothing here calls the method by
	 * name, and leaving it would make the shim's own compile resolve a symbol
	 * it has no use for.
	 */
	ERROR_DECL (metadata_error);
	MinimalCompile cfg (method, state.domain, metadata_error);

	if (cfg.get ()->header == nullptr)
		return runtime_error (metadata_error);

	auto context = std::make_unique<LLVMContext> ();
	auto module = std::make_unique<Module> ("mono.interp.shim", *context);

	std::vector<ExternalSymbol> externals;
	MethodLLVMEmitter declarer (module.get (), cfg.get (), method, &externals);
	Expected<Function *> shape = declarer.declare (method);

	if (!shape)
		return shape.takeError ();

	MonoMethodSignature *sig = mono_method_signature_internal (method);

	if (method->string_ctor)
		sig = mono_marshal_get_string_ctor_signature (method);

	arch::LegacyFlavor flavor = legacy_call_flavor (sig);
	std::string key = prototype_key (*shape, flavor);
	void *shim = nullptr;

	{
		std::lock_guard<std::mutex> lock (mutex_);
		auto it = state.shims.find (key);

		if (it != state.shims.end ())
			shim = it->second;
	}

	if (shim == nullptr) {
		static std::atomic<uint64_t> next { 0 };
		std::string name = "mono.interp.shim."
		                   + std::to_string (next.fetch_add (1));

		arch::create_fastcc_entry_thunk (*module, name, *shape, flavor);

		if ((*shape)->use_empty ())
			(*shape)->eraseFromParent ();

		if (dumping (name.c_str ()))
			module->print (llvm::errs (), nullptr);

		Expected<CompiledMethod> compiled = state.jit->compile (
			ThreadSafeModule (std::move (module),
		                          ThreadSafeContext (std::move (context))),
			name);

		if (!compiled)
			return compiled.takeError ();

		/*
		 * The shim calls, so a thread can be stopped in its frame and an
		 * exception out of the interpreted method unwinds through it: it needs
		 * a record like any other frame. The method it is registered under is
		 * whichever one first wanted this prototype, which is why the record
		 * says the code holds none of that method's IL - a stack walk reports
		 * no frame for it either way.
		 */
		Expected<MonoJitInfo *> jinfo = register_jit_info (
			state.domain, method, nullptr, *compiled, CodeKind::AbiThunk);

		if (!jinfo)
			return jinfo.takeError ();

		std::lock_guard<std::mutex> lock (mutex_);
		/* Two threads racing on one prototype both compile; both are correct
		 * code, and every method after them converges on whichever won. */
		shim = state.shims.emplace (key, compiled->entry).first->second;
	}

	/*
	 * Which method the shim was entered for is the one thing it cannot read
	 * off the prototype, so it arrives in the key register - a stub of the
	 * method's own is what puts it there. Redirectable like the others, though
	 * nothing redirects this one: the body stub in front of it is what a later
	 * tier moves.
	 */
	std::string name = symbol_for_body (method) + "$interp";
	Expected<void *> thunk = state.jit->create_keyed_stub (name, shim, legacy);

	if (!thunk)
		return thunk;

	register_stub_jinfo (state.domain, method, *thunk, arch::stub_block_size,
	                     name);
	return *thunk;
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

	remember (state, method, *compiled, nullptr);

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

	if (Error err = state.jit->create_lazy_stub (
	        symbol_for_code (method),
	        [this, owner, method, bindable, raising] () -> Expected<void *> {
		        if (!bindable ()) {
			        Expected<void *> thunk = compile_entry_thunk (*owner, method);

			        if (!thunk)
				        return raising (thunk.takeError (), &Compiled::entry);
			        return *thunk;
		        }

		        Expected<Compiled> code = ensure_entries (*owner, method);

		        if (!code)
			        return raising (code.takeError (), &Compiled::entry);
		        return code->entry;
	        }))
		return err;

	if (Error err = state.jit->create_lazy_stub (
	        symbol_for_body (method),
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
	        }))
		return err;

	/*
	 * The unboxing entry is a stub too, and for the reason the others are: it
	 * is what fills a value type's vtable slots, so a slot filled before the
	 * method had code has to keep working once it does. It gets no dispatcher -
	 * the runtime only ever asks for one while compiling in the owning domain.
	 */
	if (publishes_unbox_entry (method)) {
		if (Error err = state.jit->create_lazy_stub (
		        symbol_for_unbox (method),
		        [this, owner, method, raising] () -> Expected<void *> {
			        Expected<Compiled> code =
				        ensure_entries (*owner, method);

			        if (!code)
				        return raising (code.takeError (),
				                        &Compiled::unbox);
			        return code->unbox;
		        }))
			return err;
	}

	state.defined.insert (method);
	return Error::success ();
}

/// The encoded unwind program every stub runs under.
///
/// A stub is a bare jump: it has pushed nothing, so at any instruction in it the
/// frame is still exactly the one the call left behind - which is what the
/// arch's CIE describes and nothing more. Without this a walk that catches a
/// thread mid-jump cannot step off the stub into its caller at all.
ArrayRef<uint8_t>
stub_unwind_info ()
{
	static const std::vector<uint8_t> encoded = [] {
		GSList *ops = mono_arch_get_cie_program ();
		guint32 len = 0;
		guint8 *bytes = mono_unwind_ops_encode (ops, &len);
		std::vector<uint8_t> program (bytes, bytes + len);

		g_free (bytes);
		mono_free_unwind_info (ops);
		return program;
	}();

	return encoded;
}

/// Register the jit-info record that resolves the SIZE bytes of stub code at
/// STUB back to METHOD, under the symbol NAME.
///
/// Returns the record when the caller has to take it out again, and null when
/// it dies with its domain.
MonoJitInfo *
register_stub_jinfo (MonoDomain *domain, MonoMethod *method, void *stub,
                     size_t size, const std::string &name)
{
	ArrayRef<uint8_t> unwind = stub_unwind_info ();
	guint8 *uw_info = const_cast<guint8 *> (unwind.data ());
	guint32 uw_info_len = (guint32) unwind.size ();

	/*
	 * A dynamic method's stub block goes back on the free list when the method
	 * is freed, so its record has to be taken out again before the next method
	 * lands there - and mono_jit_info_table_remove () frees what it unregisters,
	 * so that record has to come from the allocator that call uses. Every other
	 * method lives exactly as long as its domain, and so does its record.
	 */
	if (method->dynamic)
		return mono_tramp_info_register_reclaimable (
			domain, method, stub, (guint32) size, name.c_str (),
			uw_info, uw_info_len);

	MonoTrampInfo *tramp = g_new0 (MonoTrampInfo, 1);

	tramp->code = (guint8 *) stub;
	tramp->code_size = (guint32) size;
	tramp->name = g_strdup (name.c_str ());
	tramp->method = method;
	tramp->uw_info = uw_info;
	tramp->uw_info_len = uw_info_len;
	mono_tramp_info_register (tramp, domain);
	return nullptr;
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
	std::string entry_name = symbol_for_code (method);
	std::string body_name = symbol_for_body (method);
	Expected<void *> stub = state.jit->stub_address (entry_name);
	if (!stub)
		return stub;

	Expected<void *> body = state.jit->stub_address (body_name);
	if (!body)
		return body;

	bool unboxed = publishes_unbox_entry (method);
	std::string unbox_name = unboxed ? symbol_for_unbox (method) : std::string ();
	void *unbox = nullptr;

	if (unboxed) {
		Expected<void *> reserved = state.jit->stub_address (unbox_name);

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
	 * a ldftn most visibly - must find it in the jit-info table. Register both
	 * the way mini registers trampolines: an is_trampoline entry carrying the
	 * method, in the domain whose linker holds the stub so the two die
	 * together. The legacy entry is what the runtime calls, but the body stub
	 * is what every cross-method call jumps through, so it is the one a thread
	 * is far more likely to be caught in. The published check above keeps
	 * racing threads from registering either twice.
	 */
	MonoJitInfo *entry_jinfo = register_stub_jinfo (
		state.domain, method, *stub, arch::stub_block_size, entry_name);
	MonoJitInfo *body_jinfo = register_stub_jinfo (
		state.domain, method, *body, arch::stub_block_size, body_name);
	MonoJitInfo *unbox_jinfo =
		unboxed ? register_stub_jinfo (state.domain, method, unbox,
	                                       arch::stub_block_size, unbox_name)
	                : nullptr;

	state.published[method] =
		Publication { *stub, entry_jinfo, body_jinfo, unbox_jinfo, unboxed };

	return *stub;
}

} // namespace

} // namespace mono

void
mono_llvm_jit_free_domain (MonoDomain *domain)
{
	mono::Backend::free_domain (domain);
}

void
mono_llvm_jit_free_method (MonoMethod *method)
{
	mono::Backend::free_method (method);
}

void *
mono_llvm_jit_find_body (MonoDomain *domain, MonoMethod *method)
{
	return mono::Backend::body_of (domain, method);
}

void
mono_llvm_jit_foreach_body (MonoDomain *domain, MonoMethod *method,
                            void (*visit) (MonoJitInfo *, void *), void *user_data)
{
	mono::Backend::foreach_body (domain, method, visit, user_data);
}

void *
mono_llvm_jit_unbox_entry (MonoMethod *method)
{
	return mono::Backend::unbox_entry_of (method);
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

void
mono_llvm_jit_interpret_methods (const char *filter)
{
	mono::Backend::set_interp_filter (filter);
}

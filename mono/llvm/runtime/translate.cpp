#include "runtime-error.hpp"

#include "translate.hpp"

#include "arch/arch.hpp"
#include "externals.hpp"
#include "jinfo.hpp"
#include "method-to-llvm.hpp"
#include "minimal-compile.hpp"
#include "naming.hpp"
#include "options.hpp"
#include "timing.hpp"

#include <llvm/IR/Constants.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>

#include <optional>
#include <string>
#include <vector>

#include "mini.h"
#include "mini-runtime.h"

#include "mono/metadata/class-internals.h"
#include "mono/metadata/domain-internals.h"
#include "mono/metadata/marshal.h"

using namespace llvm;
using namespace llvm::orc;

namespace mono {

DomainScope::DomainScope (MonoDomain *domain)
    : entered_ (mono_domain_get ()), wanted_ (domain)
{
	if (entered_ != wanted_)
		mono_domain_set_internal_with_options (wanted_, FALSE);
}

DomainScope::~DomainScope ()
{
	if (entered_ != wanted_)
		mono_domain_set_internal_with_options (entered_, FALSE);
}

Expected<Compiled>
translate_and_compile (const TranslationTarget &target, MonoMethod *method,
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
	 * once the method can be looked up - a non-null published record is what says
	 * one is owed.
	 */
	MONO_PROFILER_RAISE (jit_begin, (method));

	Expected<Compiled> code = translate_body (target, method, published);

	if (!code || *published == nullptr) {
		*published = nullptr;
		MONO_PROFILER_RAISE (jit_failed, (method));
	}

	return code;
}

Expected<Compiled>
translate_body (const TranslationTarget &target, MonoMethod *method,
                MonoJitInfo **published)
{
	std::unique_ptr<LLVMContext> context;
	std::unique_ptr<Module> module;

	{
		timing::Scope timed (timing::Phase::ctxnew);

		context = std::make_unique<LLVMContext> ();
		module = std::make_unique<Module> (stub_symbol (method, Entry::body), *context);
	}

	/*
	 * Translation itself resolves everything per-domain against target.domain
	 * (resolve (), ldstr through cfg->domain) - never against the thread's
	 * current domain, and any new translate-time mono_domain_get () is a
	 * cross-domain bug of the kind the dispatcher exists to prevent. The
	 * stub lambdas keep the two equal.
	 */
	g_assert (mono_domain_get () == target.domain);

	if (is_jit_trace_enabled ()) {
		char *name = mono_method_full_name (method, TRUE);

		fprintf (stderr, "[llvm-jit] translating %s (for %s)\n", name,
		         target.domain->friendly_name);
		g_free (name);
	}

	ERROR_DECL (metadata_error);
	std::optional<MinimalCompile> cfg;

	{
		timing::Scope timed (timing::Phase::metadata);

		cfg.emplace (method, target.domain, metadata_error);
	}

	if (cfg->get ()->header == nullptr)
		return target.recover (runtime_error (metadata_error));

	std::vector<ExternalSymbol> externals;
	MonoLLVMBreakpointSwitch *bp_switch = nullptr;
	SeqPointGraph seq_points;
	Expected<Function *> function = [&] {
		timing::Scope timed (timing::Phase::translate);

		return method_to_llvm (module.get (), cfg->get (), method, &externals,
		                       &bp_switch, &seq_points);
	}();
	if (!function)
		return target.recover (function.takeError ());

	/* Before the entry name is read: the body's own declaration is one of these. */
	if (Error err = bind_symbols (*module))
		return target.recover (std::move (err));

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

		return resolve_externals (*target.jit, target.domain, externals,
		                          target.publish_callee);
	}();

	if (resolved)
		return target.recover (std::move (resolved));

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
	Expected<void *> body_stub = target.stub_address (stub_symbol (method, Entry::body));

	if (!body_stub)
		return body_stub.takeError ();

	MonoMethodSignature *sig = mono_method_signature_internal (method);

	if (method->string_ctor)
		sig = mono_marshal_get_string_ctor_signature (method);

	Constant *body_address = ConstantExpr::getIntToPtr (
		ConstantInt::get (Type::getInt64Ty (*context),
		                  (uint64_t) (uintptr_t) *body_stub),
		PointerType::get (*context, 0));
	std::string legacy_entry;

	if (publishes_interop_entry (method)) {
		legacy_entry = definition_symbol (method, Entry::interop);
		arch::create_legacy_entry_thunk (*module, legacy_entry, *function,
		                                 legacy_call_flavor (sig), body_address);
	}

	/*
	 * Reached off a value type's vtable, the method arrives with the boxed
	 * object as its receiver instead of the value. That is a second entry
	 * rather than a branch inside the first: which one a caller wants is
	 * settled when the address is handed out, and the ordinary entry must not
	 * pay for the question.
	 */
	std::string unbox_entry;

	if (wants_unbox_entry (method, sig)) {
		unbox_entry = definition_symbol (method, Entry::unbox);
		arch::create_unbox_entry (*module, unbox_entry, *function, body_address,
		                          MONO_ABI_SIZEOF (MonoObject));
	}

	if (dumping (entry.c_str ()))
		module->print (llvm::errs (), nullptr);

	Expected<CompiledMethod> compiled = [&] {
		timing::Scope timed (timing::Phase::orc);

		return target.jit->compile (
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

	if (entry_code == nullptr && !legacy_entry.empty ())
		return createStringError (inconvertibleErrorCode (),
		                          "the linked object for %s defines no interop "
		                          "entry", entry.c_str ());

	Expected<MonoJitInfo *> jinfo = [&] {
		timing::Scope timed (timing::Phase::jinfo);

		return register_jit_info (target.domain, method, cfg->get ()->header,
		                          *compiled, CodeKind::Body, filters, bp_switch,
		                          seq_points);
	}();

	if (!jinfo)
		return jinfo.takeError ();
	target.remember (*compiled, *jinfo);
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
			register_jit_info (target.domain, method, nullptr, side, kind);

		if (!jinfo)
			return jinfo.takeError ();
		target.remember (side, *jinfo);
		return Error::success ();
	};

	if (entry_code != nullptr) {
		if (Error err = register_side_body (entry_code, entry_code_size,
		                                    CodeKind::AbiThunk, {}))
			return std::move (err);
	}

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

	if (is_jit_trace_enabled ())
		fprintf (stderr, "[llvm-jit] %s is at %p (for %s)\n", entry.c_str (),
		         compiled->entry, target.domain->friendly_name);

	return Compiled { const_cast<uint8_t *> (entry_code), compiled->entry,
		              const_cast<uint8_t *> (unbox_code), *jinfo };
}

} // namespace mono

#include "runtime-error.hpp"

#include "translate.hpp"

#include "arch/arch.hpp"
#include "debugging/perf/dump-method.hpp"
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

#include <memory>
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
		 * What stands behind the symbol is C. Generated code declares such a
		 * method against the plain symbol and lowers its calls, so nothing
		 * ever reaches the body stub expecting the natural convention.
		 *
		 * The profiler hears about this one from
		 * mono_jit_compile_method_with_opt (), which reports the declaration
		 * against the wrapper it built - and that wrapper came back through
		 * here and bracketed itself.
		 */
		return Compiled { code };
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
		module = std::make_unique<Module> (stub_symbol (method), *context);
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
	std::vector<std::pair<StringRef, void *>> module_symbols;
	Error resolved = [&] {
		timing::Scope timed (timing::Phase::resolve);

		return resolve_externals (*target.jit, target.domain, externals,
		                          target.publish_callee, module_symbols);
	}();

	if (resolved)
		return target.recover (std::move (resolved));

	if (dumping (entry.c_str ()))
		module->print (llvm::errs (), nullptr);

	std::vector<ProfileCounters> layout =
		MonoJit::optimize (*module, target.tier, target.profile);

	Expected<CompiledMethod> compiled = [&] {
		timing::Scope timed (timing::Phase::orc);

		return target.jit->compile (
			ThreadSafeModule (std::move (module),
		                      ThreadSafeContext (std::move (context))),
			entry, module_symbols, layout);
	}();
	if (!compiled)
		return compiled.takeError ();

	perf::dump_method (method, *compiled);

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
	 * A record of its own for each filter body the module holds: a suspended
	 * thread can be stopped in one, and it is a frame something walking the
	 * stack has to be able to name. A filter carries no clauses, so its frame
	 * description and the IL-offset map that says where in the method's IL its
	 * frame is are all it takes from the module's side tables. No dylib either:
	 * the body's record already owns the one they both share.
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

	return Compiled { compiled->entry, *jinfo };
}

Expected<void *>
compile_interop_entry (MonoJit &jit, MonoDomain *domain, MonoMethod *method,
                       void *body, RememberFn remember)
{
	ERROR_DECL (metadata_error);
	MinimalCompile cfg (method, domain, metadata_error);

	if (cfg.get ()->header == nullptr)
		return runtime_error (metadata_error);

	auto context = std::make_unique<LLVMContext> ();
	std::string name = interop_symbol (method);
	auto module = std::make_unique<Module> (name, *context);

	/*
	 * A declaration is enough: the thunk reads the prototype to know how to
	 * take a C call apart, and calls the address rather than the function.
	 */
	std::vector<ExternalSymbol> externals;
	MethodLLVMEmitter declarer (module.get (), cfg.get (), method, &externals);
	Expected<Function *> shape = declarer.declare (method);

	if (!shape)
		return shape.takeError ();

	Constant *address = ConstantExpr::getIntToPtr (
		ConstantInt::get (Type::getInt64Ty (*context), (uint64_t) (uintptr_t) body),
		PointerType::get (*context, 0));

	arch::create_mono_entry_thunk (*module, name, *shape, address);

	/* Nothing names it now, and leaving it would ask the linker for a symbol. */
	(*shape)->eraseFromParent ();

	if (dumping (name.c_str ()))
		module->print (llvm::errs (), nullptr);

	MonoJit::optimize (*module, JitTier::tier1);

	Expected<CompiledMethod> compiled =
		jit.compile (ThreadSafeModule (std::move (module),
	                                       ThreadSafeContext (std::move (context))),
	                     name);

	if (!compiled)
		return compiled.takeError ();

	perf::dump_method (method, *compiled);

	Expected<MonoJitInfo *> jinfo =
		register_jit_info (domain, method, nullptr, *compiled, CodeKind::AbiThunk);

	if (!jinfo)
		return jinfo.takeError ();
	remember (*compiled, *jinfo);

	return compiled->entry;
}

} // namespace mono

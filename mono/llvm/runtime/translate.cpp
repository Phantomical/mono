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
#include "passes/inline-copies.hpp"
#include "profile-inlines.hpp"
#include "timing.hpp"
#include "trivial-inlines.hpp"

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

/// Registers the jit info for a compiled body and for every side function that
/// came with it, and returns where the method's code is.
///
/// published receives the body's record. header is the method's, and null is
/// not allowed: a body needs its clauses.
static Expected<Compiled> publish_body (const TranslationTarget &target, MonoMethod *method,
                                        MonoMethodHeader *header, CompiledMethod &compiled,
                                        MonoLLVMBreakpointSwitch *bp_switch,
                                        const SeqPointGraph &seq_points,
                                        MonoJitInfo **published);

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
	 * lowers them inline. The marshal wrapper is the compilable form for
	 * runtime paths that need the method itself. Its inner call to the
	 * accessor lowers the same way.
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
		 * against the wrapper it built. That wrapper comes back through here
		 * and brackets itself.
		 */
		return Compiled { code };
	}

	/*
	 * The profiler's compilation of the method begins here. Exactly one end
	 * follows every begin, so a consumer pairing the two never carries an open
	 * span for the rest of the process. A method whose metadata fails to load
	 * gets a stand-in body that raises instead of a translation, and that is a
	 * failed compile however it is served. The successful end is raised by the
	 * caller, once the method can be looked up - a non-null published record is
	 * what says one is owed.
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
	 * (resolve_externals (), ldstr through cfg->domain), never against the
	 * thread's current domain. Any new translate-time mono_domain_get () is a
	 * cross-domain bug of the kind the dispatcher exists to prevent. The stub
	 * lambdas keep the two equal.
	 */
	g_assert (mono_domain_get () == target.domain);

	if (is_jit_trace_enabled ()) {
		char *name = mono_method_full_name (method, TRUE);

		fprintf (stderr, "[llvm-jit] translating %s at tier %d (for %s)\n", name,
		         target.tier == JitTier::tier2 ? 2 : 1, target.domain->friendly_name);
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
	ModuleTypes types;
	Expected<Function *> function = [&] {
		timing::Scope timed (timing::Phase::translate);

		return method_to_llvm (module.get (), cfg->get (), method, &externals,
		                       &bp_switch, &seq_points, {}, &types);
	}();
	if (!function)
		return target.recover (function.takeError ());

	InlineScope inlining;

	inlining.root = method;
	inlining.defined.push_back (method);
	inlining.budget = trivial_inline_budget ();

	// Both compiled tiers fold in the callees whose IL already says the inline
	// pays. It runs here so that the bodies it adds still reach the naming and
	// the resolution below.
	materialize_trivial_callees (*module, target.domain, method, **function,
	                             externals, types, inlining);

	/* Before the entry name is read: the body's own declaration is one of these. */
	if (Error err = bind_symbols (*module))
		return target.recover (std::move (err));

	std::string entry = (*function)->getName ().str ();

	if (dumping (entry.c_str ()))
		dump_il (method, cfg->get ()->header);

	/*
	 * Laying out a class to create its vtable is the other place metadata gets
	 * loaded, and the last one. A method whose callee's class will not load
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

	/*
	 * The cost model materializes on demand, from inside the pipeline and so
	 * past the resolution above. It appends to the same two vectors, and the
	 * link is given module_symbols after this rather than before it.
	 */
	std::optional<ProfileInliner> inliner;

	if (target.tier == JitTier::tier2)
		inliner.emplace (*module, target, externals, types, inlining,
		                 module_symbols);

	std::vector<ProfileCounters> layout = MonoJit::optimize (
		*module, target.tier, target.profile,
		inliner ? &*inliner : nullptr);

	if (Error err = inline_copies_stripped (*module))
		return std::move (err);

	Expected<CompiledMethod> compiled = [&] {
		timing::Scope timed (timing::Phase::orc);

		return target.jit->compile (
			ThreadSafeModule (std::move (module),
		                      ThreadSafeContext (std::move (context))),
			entry, module_symbols, layout);
	}();
	if (!compiled)
		return compiled.takeError ();

	return publish_body (target, method, cfg->get ()->header, *compiled, bp_switch,
	                     seq_points, published);
}

static Expected<Compiled>
publish_body (const TranslationTarget &target, MonoMethod *method, MonoMethodHeader *header,
              CompiledMethod &compiled, MonoLLVMBreakpointSwitch *bp_switch,
              const SeqPointGraph &seq_points, MonoJitInfo **published)
{
	perf::dump_method (method, compiled);

	/*
	 * Filter bodies were compiled alongside the method as `<entry>$filter<i>`.
	 * Their entries go into the published clauses.
	 */
	std::vector<std::pair<uint32_t, void *>> filters;

	for (const auto &[name, extent] : compiled.functions) {
		size_t at = name.rfind ("$filter");

		if (at == std::string::npos)
			continue;
		filters.emplace_back (
			(uint32_t) std::stoul (name.substr (at + 7)),
			const_cast<uint8_t *> (extent.first));
	}

	Expected<MonoJitInfo *> jinfo = [&] {
		timing::Scope timed (timing::Phase::jinfo);

		return register_jit_info (target.domain, method, header, compiled,
		                          CodeKind::Body, filters, bp_switch, seq_points);
	}();

	if (!jinfo)
		return jinfo.takeError ();
	target.remember (compiled, *jinfo);
	*published = *jinfo;

	/*
	 * Each filter body gets a record of its own. A suspended thread can stop
	 * inside one, and a stack walk has to be able to name that frame. A filter
	 * carries no clauses. It takes two things from the module's side tables:
	 * the frame description, and the IL-offset map that locates its frame in
	 * the method's IL. It needs no dylib either - the body's record already
	 * owns the one they share.
	 */
	auto register_side_body = [&] (const uint8_t *code, size_t size, CodeKind kind,
	                               std::vector<IlLineRow> lines) -> Error {
		CompiledMethod side;

		side.entry = const_cast<uint8_t *> (code);
		side.code = code;
		side.code_size = size;
		side.unwind_table = compiled.unwind_table;
		side.unwind_table_size = compiled.unwind_table_size;
		side.il_lines = std::move (lines);

		Expected<MonoJitInfo *> jinfo =
			register_jit_info (target.domain, method, nullptr, side, kind);

		if (!jinfo)
			return jinfo.takeError ();
		target.remember (side, *jinfo);
		return Error::success ();
	};

	for (const auto &[name, extent] : compiled.functions) {
		if (name.find ("$filter") == std::string::npos)
			continue;

		std::vector<IlLineRow> lines;

		for (auto &rows : compiled.other_il_lines)
			if (rows.first == name) {
				lines = std::move (rows.second);
				break;
			}

		if (Error err = register_side_body (extent.first, extent.second,
		                                    CodeKind::Body, std::move (lines)))
			return std::move (err);
	}

	if (is_jit_trace_enabled ()) {
		char *name = mono_method_full_name (method, TRUE);

		fprintf (stderr, "[llvm-jit] %s is at %p (for %s)\n", name, compiled.entry,
		         target.domain->friendly_name);
		g_free (name);
	}

	return Compiled { compiled.entry, *jinfo };
}

namespace {

/// What one method of a batch keeps for as long as the module it shares.
struct BatchMember {
	const TranslationTarget *target = nullptr;
	MonoMethod *method = nullptr;
	std::unique_ptr<MinimalCompile> cfg;
	llvm::Function *body = nullptr;
	std::string entry;
	std::vector<ExternalSymbol> externals;
	MonoLLVMBreakpointSwitch *bp_switch = nullptr;
	SeqPointGraph seq_points;
};

} // namespace

std::vector<BatchResult>
translate_and_compile_batch (llvm::ArrayRef<const TranslationTarget *> targets,
                             llvm::ArrayRef<MonoMethod *> methods)
{
	auto one_by_one = [&] {
		std::vector<BatchResult> alone;

		alone.reserve (methods.size ());
		for (size_t i = 0; i < methods.size (); ++i) {
			MonoJitInfo *published = nullptr;
			Expected<Compiled> code =
				translate_and_compile (*targets[i], methods[i], &published);

			alone.push_back (BatchResult { std::move (code), published });
		}
		return alone;
	};

	const TranslationTarget &shared = *targets.front ();

	// A tier-2 body is laid out by its own method's counts, so it has no module
	// to share. Which methods arrive here together is the caller's choice, so
	// the refusal is checked here rather than assumed of the caller.
	if (methods.size () < 2 || shared.tier != JitTier::tier1)
		return one_by_one ();

	for (MonoMethod *method : methods) {
		if (implemented_outside_il (method) || method->dynamic)
			return one_by_one ();

		// An array accessor is compiled as its marshal wrapper, which is a
		// method of its own.
		if (m_class_get_rank (method->klass) > 0
		    && (method->iflags & METHOD_IMPL_ATTRIBUTE_INTERNAL_CALL)
		    && (method->iflags & METHOD_IMPL_ATTRIBUTE_NATIVE))
			return one_by_one ();
	}

	g_assert (mono_domain_get () == shared.domain);

	std::unique_ptr<LLVMContext> context;
	std::unique_ptr<Module> module;

	{
		timing::Scope timed (timing::Phase::ctxnew);

		context = std::make_unique<LLVMContext> ();
		module = std::make_unique<Module> (stub_symbol (methods.front ()), *context);
	}

	std::vector<std::unique_ptr<BatchMember>> members;
	ModuleTypes types;

	for (MonoMethod *method : methods)
		MONO_PROFILER_RAISE (jit_begin, (method));

	/*
	 * Give up on the shared module and compile the methods one at a time. Each
	 * one begins again there, so this attempt owes every member of it an end.
	 * A failure is rare and the retry is what keeps a single bad method from
	 * costing the others their compile.
	 */
	auto give_up = [&] {
		for (MonoMethod *method : methods)
			MONO_PROFILER_RAISE (jit_failed, (method));
		return one_by_one ();
	};

	for (size_t i = 0; i < methods.size (); ++i) {
		auto member = std::make_unique<BatchMember> ();

		member->target = targets[i];
		member->method = methods[i];

		ERROR_DECL (metadata_error);

		{
			timing::Scope timed (timing::Phase::metadata);

			member->cfg = std::make_unique<MinimalCompile> (methods[i], shared.domain,
			                                                metadata_error);
		}

		if (member->cfg->get ()->header == nullptr) {
			mono_error_cleanup (metadata_error);
			return give_up ();
		}

		Expected<llvm::Function *> body = [&] {
			timing::Scope timed (timing::Phase::translate);

			return method_to_llvm (module.get (), member->cfg->get (), methods[i],
			                       &member->externals, &member->bp_switch,
			                       &member->seq_points, methods, &types);
		}();

		if (!body) {
			consumeError (body.takeError ());
			return give_up ();
		}

		member->body = *body;
		members.push_back (std::move (member));
	}

	/*
	 * After every member is translated: a member is declared to the others
	 * under a name of its own, so one folded in here would be a second copy of
	 * a body the module already holds. A copy one member takes in is folded
	 * into any other member that calls it as well, since they share the
	 * declaration it was built into.
	 */
	InlineScope inlining;

	inlining.defined.assign (methods.begin (), methods.end ());

	for (auto &member : members) {
		// The budget is counted for each member rather than for the module, so
		// what a method folds in does not depend on how many others happened to
		// promote beside it. MONO_LLVM_JIT_BATCH then changes the compile count
		// and nothing else.
		inlining.root = member->method;
		inlining.budget = trivial_inline_budget ();

		materialize_trivial_callees (*module, shared.domain, member->method,
		                             *member->body, member->externals, types,
		                             inlining);
	}

	if (Error err = bind_symbols (*module)) {
		consumeError (std::move (err));
		return give_up ();
	}

	std::vector<std::pair<StringRef, void *>> module_symbols;

	for (auto &member : members) {
		member->entry = member->body->getName ().str ();

		if (dumping (member->entry.c_str ()))
			dump_il (member->method, member->cfg->get ()->header);

		Error resolved = [&] {
			timing::Scope timed (timing::Phase::resolve);

			return resolve_externals (*shared.jit, shared.domain, member->externals,
			                          member->target->publish_callee, module_symbols);
		}();

		if (resolved) {
			consumeError (std::move (resolved));
			return give_up ();
		}
	}

	std::vector<ProfileCounters> layout =
		MonoJit::optimize (*module, shared.tier, shared.profile);

	if (Error err = inline_copies_stripped (*module)) {
		consumeError (std::move (err));
		return give_up ();
	}

	std::vector<StringRef> entries;
	bool dump = false;

	for (auto &member : members) {
		dump = dump || dumping (member->entry.c_str ());
		entries.push_back (member->entry);
	}

	// One print for the module, however many of its methods were asked for.
	if (dump)
		module->print (llvm::errs (), nullptr);

	Expected<std::vector<CompiledMethod>> compiled = [&] {
		timing::Scope timed (timing::Phase::orc);

		return shared.jit->compile_batch (
			ThreadSafeModule (std::move (module),
		                      ThreadSafeContext (std::move (context))),
			entries, module_symbols, layout);
	}();

	std::vector<BatchResult> out;

	out.reserve (members.size ());

	// The object is one thing, so a failure to link it is every member's.
	if (!compiled) {
		std::string failure = toString (compiled.takeError ());

		for (auto &member : members) {
			MONO_PROFILER_RAISE (jit_failed, (member->method));
			out.push_back (BatchResult {
				createStringError (inconvertibleErrorCode (), "%s",
			                           failure.c_str ()),
				nullptr });
		}
		return out;
	}

	for (size_t i = 0; i < members.size (); ++i) {
		MonoJitInfo *published = nullptr;
		Expected<Compiled> code =
			publish_body (*members[i]->target, members[i]->method,
		                      members[i]->cfg->get ()->header, (*compiled)[i],
		                      members[i]->bp_switch, members[i]->seq_points,
		                      &published);

		if (!code || published == nullptr) {
			published = nullptr;
			MONO_PROFILER_RAISE (jit_failed, (members[i]->method));
		}

		out.push_back (BatchResult { std::move (code), published });
	}

	return out;
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

	/* Nothing names it now. If it stays, the linker asks for a symbol. */
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

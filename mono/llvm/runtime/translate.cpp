#include "runtime-error.hpp"

#include "translate.hpp"

#include "arch/arch.hpp"
#include "compile-state.hpp"
#include "debugging/perf/dump-method.hpp"
#include "dump.hpp"
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
#include <unordered_set>
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

/// Whether method is one of an array type's Get, Set or Address accessors.
///
/// These have no body and no icall - every call site lowers them inline - so
/// compiling the method itself means compiling its marshal wrapper instead.
static bool
is_array_accessor (MonoMethod *method)
{
	return m_class_get_rank (method->klass) > 0
	       && (method->iflags & METHOD_IMPL_ATTRIBUTE_INTERNAL_CALL)
	       && (method->iflags & METHOD_IMPL_ATTRIBUTE_NATIVE);
}

static DumpPoint
optimized_ir_point (JitTier tier)
{
	return tier == JitTier::tier2 ? DumpPoint::tier2_ir : DumpPoint::tier1_ir;
}

static void
dump_ir (DumpPoint point, const Module &module, StringRef entry, StringRef name)
{
	if (!dumping (point, name.str ().c_str ()))
		return;

	// A pipeline can replace a function, so the entry is looked up by name
	// rather than through the pointer translation returned.
	cantFail (dump_body_module (point, module, entry, name));
}

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

	/* The wrapper's own inner call to the accessor lowers inline as any other. */
	if (is_array_accessor (method))
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

	MONO_PROFILER_RAISE (jit_begin, (method));

	Expected<Compiled> code = translate_body (target, method, published);

	// A metadata failure recovered into a raising stand-in still has no
	// jinfo, so it counts as failed here too.
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
	 * cross-domain bug of the kind the dispatcher exists to prevent.
	 * DomainScope, entered by the caller, keeps the two equal.
	 */
	g_assert (mono_domain_get () == target.domain);

	if (is_jit_trace_enabled ()) {
		char *name = mono_method_full_name (method, TRUE);

		MONO_LOCK (jit_trace_mutex ())
		{
			fprintf (stderr, "[llvm-jit] translating %s at tier %d (for %s)\n",
			         name, target.tier == JitTier::tier2 ? 2 : 1,
			         target.domain->friendly_name);
		}
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
	inlining.folded.push_back ({ method, nullptr });
	inlining.budget = { trivial_inline_budget (), costed_inline_budget () };

	std::vector<std::pair<StringRef, void *>> module_symbols;
	auto resolve = [&] (ArrayRef<ExternalSymbol> named) {
		timing::Scope timed (timing::Phase::resolve);

		return resolve_externals (*target.jit, target.domain, named,
		                          target.publish_callee, module_symbols);
	};

	// What the method itself named, which is everything the translation above
	// recorded. The pre-pass resolves each copy's own share as it goes, so
	// that a copy the runtime cannot resolve costs a fold rather than the
	// whole compile.
	size_t own = externals.size ();

	// Both compiled tiers fold in the callees whose IL already says the inline
	// pays. It runs here so that the bodies it adds still reach the naming and
	// the resolution below.
	materialize_trivial_callees (*module, target.domain, method, **function,
	                             externals, types, inlining, resolve);

	if (Error err = bind_symbols (*module))
		return target.recover (std::move (err));

	std::string entry = (*function)->getName ().str ();
	std::string dumped = any_dump_point_enabled () ? dump_name (method)
	                                               : std::string ();

	if (!dumped.empty ())
		set_dump_name (**function, dumped);

	// A method that starts interpreted had its IL printed there, and printing it
	// again here says nothing new. This leaves one dump for each method, from
	// whichever engine reached it first.
	if (!runs_at_tier0 (method) && dumping (DumpPoint::il, dumped.c_str ())) {
		DumpDestination destination (DumpPoint::il, dumped.c_str ());

		if (destination.stream () != nullptr)
			dump_il (destination.stream (), method, cfg->get ()->header);
	}

	/*
	 * Laying out a class to create its vtable is the other place metadata gets
	 * loaded, and the last one. A method whose callee's class will not load
	 * fails here rather than in the translation above. It is the same failure
	 * and it is raised the same way - at the call, not at the declaration.
	 */
	Error resolved = resolve (ArrayRef (externals).take_front (own));

	if (resolved)
		return target.recover (std::move (resolved));

	dump_ir (DumpPoint::unopt_ir, *module, entry, dumped);

	/*
	 * The cost model materializes on demand, from inside the pipeline and so
	 * past the resolution above. It appends to the same two vectors, and the
	 * link is given module_symbols after this rather than before it.
	 */
	std::optional<ProfileInliner> inliner;

	if (target.tier == JitTier::tier2)
		inliner.emplace (target, externals, types, inlining, module_symbols);

	/*
	 * A pass that answers a dispatch site adds a declaration of its own, from
	 * inside the pipeline and so past the resolution above. So it names and
	 * resolves that one here, appending to the same two vectors the cost model
	 * does. One that will not resolve goes back off and the site keeps its
	 * lookup.
	 */
	auto publish_declaration = [&] (llvm::Function &decl, MonoMethod *callee) -> llvm::Function * {
		llvm::Module &holder = *decl.getParent ();
		std::string published = stub_symbol (callee);
		size_t from = externals.size ();

		externals.push_back (
			{ decl.getName ().str (), ExternalSymbol::Kind::Code, callee });

		Error named = bind_symbols (holder);

		if (!named)
			named = resolve (ArrayRef (externals).slice (from, externals.size () - from));

		if (named) {
			consumeError (std::move (named));
			externals.resize (from);
			return nullptr;
		}

		// bind_symbols () folds the declaration into one the module already
		// held for this method, so read the survivor back by name.
		return holder.getFunction (published);
	};

	/*
	 * A fold that settles a receiver's class needs that class's vtable named,
	 * and the class can be one the body never mentioned. So it is minted here
	 * and resolved at once, the way a devirtualized callee is published above.
	 *
	 * The emitter is built for the module it is handed and thrown away, because
	 * the cost model translates a candidate into a module of its own and links
	 * that away again. A symbol from one module means nothing in another, and
	 * neither does an emitter holding the first. What is kept across the compile
	 * is which classes the runtime already refused.
	 */
	std::unordered_set<MonoClass *> refused_vtables;
	auto name_vtable = [&] (llvm::Module &holder, MonoClass *klass) -> Constant * {
		if (refused_vtables.count (klass) != 0)
			return nullptr;

		MethodLLVMEmitter namer (&holder, cfg->get (), method, &externals);
		size_t from = externals.size ();
		Constant *symbol = namer.vtable_for (klass);
		Error named = symbol != nullptr
		                      ? resolve (ArrayRef (externals).slice (
					      from, externals.size () - from))
		                      : Error::success ();

		if (symbol == nullptr || named) {
			consumeError (std::move (named));
			externals.resize (from);
			refused_vtables.insert (klass);
			return nullptr;
		}

		return symbol;
	};

	CompileScope compiling ({ target.domain, publish_declaration, name_vtable });

	std::vector<ProfileCounters> layout = MonoJit::optimize (
		*module, target.tier, target.profile,
		inliner ? &*inliner : nullptr);

	if (Error err = inline_copies_stripped (*module))
		return std::move (err);

	dump_ir (optimized_ir_point (target.tier), *module, entry, dumped);

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

	/* A filter body's entry goes into the published clauses. */
	std::vector<std::pair<uint32_t, void *>> filters;

	for (const auto &[name, extent] : compiled.functions) {
		size_t at = name.rfind (filter_body_suffix);

		if (at == std::string::npos)
			continue;
		filters.emplace_back (
			(uint32_t) std::stoul (name.substr (at + filter_body_suffix.size ())),
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
	                               std::vector<IlLineRow> lines,
	                               std::vector<IlInlineRow> inlined) -> Error {
		CompiledMethod side;

		side.entry = const_cast<uint8_t *> (code);
		side.code = code;
		side.code_size = size;
		side.unwind_table = compiled.unwind_table;
		side.unwind_table_size = compiled.unwind_table_size;
		side.il_lines = std::move (lines);
		side.inline_frames = std::move (inlined);

		Expected<MonoJitInfo *> jinfo =
			register_jit_info (target.domain, method, nullptr, side, kind);

		if (!jinfo)
			return jinfo.takeError ();
		target.remember (side, *jinfo);
		return Error::success ();
	};

	for (const auto &[name, extent] : compiled.functions) {
		if (name.find (filter_body_suffix) == std::string::npos)
			continue;

		std::vector<IlLineRow> lines;
		std::vector<IlInlineRow> inlined;

		for (auto &rows : compiled.other_il_lines)
			if (rows.first == name) {
				lines = std::move (rows.second);
				break;
			}

		for (auto &rows : compiled.other_inline_frames)
			if (rows.first == name) {
				inlined = std::move (rows.second);
				break;
			}

		if (Error err = register_side_body (extent.first, extent.second,
		                                    CodeKind::Body, std::move (lines),
		                                    std::move (inlined)))
			return std::move (err);
	}

	if (is_jit_trace_enabled ()) {
		char *name = mono_method_full_name (method, TRUE);

		MONO_LOCK (jit_trace_mutex ())
		{
			fprintf (stderr, "[llvm-jit] %s is at %p (for %s)\n", name,
			         compiled.entry, target.domain->friendly_name);
		}
		g_free (name);
	}

	return Compiled { compiled.entry, *jinfo };
}

namespace {

struct BatchMember {
	const TranslationTarget *target = nullptr;
	MonoMethod *method = nullptr;
	std::unique_ptr<MinimalCompile> cfg;
	llvm::Function *body = nullptr;
	std::string entry;
	/// What a dump of this member is filed under, empty when none is asked for.
	std::string dumped;
	std::vector<ExternalSymbol> externals;

	/// How much of externals the member itself named. The pre-pass resolves
	/// what each copy it adds names, so the resolution below takes the front.
	size_t own = 0;
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

		// The wrapper it compiles as is a method of its own.
		if (is_array_accessor (method))
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

	InlineScope inlining;

	inlining.defined.assign (methods.begin (), methods.end ());

	std::vector<std::pair<StringRef, void *>> module_symbols;

	for (auto &member : members) {
		inlining.root = member->method;
		inlining.folded.assign ({ InlineScope::Folded { member->method, nullptr } });
		inlining.budget = { trivial_inline_budget (), costed_inline_budget () };
		member->own = member->externals.size ();

		materialize_trivial_callees (*module, shared.domain, member->method,
		                             *member->body, member->externals, types,
		                             inlining,
		                             [&] (ArrayRef<ExternalSymbol> named) {
			timing::Scope timed (timing::Phase::resolve);

			return resolve_externals (*shared.jit, shared.domain, named,
			                          member->target->publish_callee,
			                          module_symbols);
		});
	}

	if (Error err = bind_symbols (*module)) {
		consumeError (std::move (err));
		return give_up ();
	}

	for (auto &member : members) {
		member->entry = member->body->getName ().str ();
		member->dumped = any_dump_point_enabled ()
		               ? dump_name (member->method)
		               : std::string ();

		if (!member->dumped.empty ())
			set_dump_name (*member->body, member->dumped);

		/* One dump for each method: see the single-method path. */
		if (!runs_at_tier0 (member->method)
		    && dumping (DumpPoint::il, member->dumped.c_str ())) {
			DumpDestination destination (DumpPoint::il, member->dumped.c_str ());

			if (destination.stream () != nullptr)
				dump_il (destination.stream (), member->method,
				         member->cfg->get ()->header);
		}

		dump_ir (DumpPoint::unopt_ir, *module, member->entry, member->dumped);

		Error resolved = [&] {
			timing::Scope timed (timing::Phase::resolve);

			return resolve_externals (
				*shared.jit, shared.domain,
				ArrayRef (member->externals).take_front (member->own),
				member->target->publish_callee, module_symbols);
		}();

		if (resolved) {
			consumeError (std::move (resolved));
			return give_up ();
		}
	}

	/*
	 * Without a compile in scope, current_compile ().domain is null, and
	 * fold_dispatch_sites (), fold_object_vtables () and initonly_static_read ()
	 * all leave their site alone. This is where nearly every tier-1 compile
	 * happens. Without the closures below, a tier-1 body and a tier-2 compile
	 * of the same method can fold different sites and hash different CFGs.
	 *
	 * One pair of closures serves the whole batch. publish_callee closes over
	 * only the domain (see member->publish_callee above), so it answers the
	 * same for every member. A symbol named here resolves against the shared
	 * module_symbols, whichever member's function asked for it.
	 */
	std::vector<ExternalSymbol> late_externals;
	auto resolve = [&] (ArrayRef<ExternalSymbol> named) {
		timing::Scope timed (timing::Phase::resolve);

		return resolve_externals (*shared.jit, shared.domain, named,
		                          shared.publish_callee, module_symbols);
	};

	auto publish_declaration = [&] (llvm::Function &decl, MonoMethod *callee) -> llvm::Function * {
		llvm::Module &holder = *decl.getParent ();
		std::string published = stub_symbol (callee);
		size_t from = late_externals.size ();

		late_externals.push_back (
			{ decl.getName ().str (), ExternalSymbol::Kind::Code, callee });

		Error named = bind_symbols (holder);
		if (!named)
			named = resolve (ArrayRef (late_externals).slice (
				from, late_externals.size () - from));

		if (named) {
			consumeError (std::move (named));
			late_externals.resize (from);
			return nullptr;
		}

		return holder.getFunction (published);
	};

	std::unordered_set<MonoClass *> refused_vtables;
	auto name_vtable = [&] (llvm::Module &holder, MonoClass *klass) -> Constant * {
		if (refused_vtables.count (klass) != 0)
			return nullptr;

		MethodLLVMEmitter namer (&holder, members.front ()->cfg->get (),
		                         members.front ()->method, &late_externals);
		size_t from = late_externals.size ();
		Constant *symbol = namer.vtable_for (klass);
		Error named = symbol != nullptr
		                      ? resolve (ArrayRef (late_externals).slice (
					      from, late_externals.size () - from))
		                      : Error::success ();

		if (symbol == nullptr || named) {
			consumeError (std::move (named));
			late_externals.resize (from);
			refused_vtables.insert (klass);
			return nullptr;
		}

		return symbol;
	};

	std::vector<ProfileCounters> layout;

	{
		// Scoped to the call the closures above are for. give_up () below
		// can run a single-method compile of its own, which takes this same
		// thread's current_compile () for that compile in turn.
		CompileScope compiling ({ shared.domain, publish_declaration, name_vtable });

		layout = MonoJit::optimize (*module, shared.tier, shared.profile);
	}

	if (Error err = inline_copies_stripped (*module)) {
		consumeError (std::move (err));
		return give_up ();
	}

	std::vector<StringRef> entries;

	for (auto &member : members) {
		dump_ir (optimized_ir_point (shared.tier), *module, member->entry,
		         member->dumped);
		entries.push_back (member->entry);
	}

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

	std::string dumped = any_dump_point_enabled () ? dump_name (method)
	                                               : std::string ();

	if (Function *thunk = module->getFunction (name); thunk != nullptr
	    && !dumped.empty ())
		set_dump_name (*thunk, dumped);

	dump_ir (DumpPoint::unopt_ir, *module, name, dumped);

	MonoJit::optimize (*module, JitTier::tier1);

	dump_ir (DumpPoint::tier1_ir, *module, name, dumped);

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

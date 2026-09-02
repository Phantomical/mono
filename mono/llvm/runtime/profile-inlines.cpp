#include "runtime-error.hpp"

#include "profile-inlines.hpp"

#include "backend.hpp"
#include "domain-method.hpp"
#include "externals.hpp"
#include "method-symbols.hpp"
#include "minimal-compile.hpp"
#include "naming.hpp"
#include "options.hpp"
#include "passes/inline-copies.hpp"
#include "timing.hpp"
#include "trivial-inlines.hpp"

#include <llvm/ADT/ArrayRef.h>
#include <llvm/ADT/STLExtras.h>
#include <llvm/ADT/SmallPtrSet.h>
#include <llvm/Analysis/InlineCost.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Module.h>

#include "mini.h"
#include "mini-runtime.h"

#include "mono/metadata/class-internals.h"
#include "mono/metadata/debug-helpers.h"

using namespace llvm;

namespace mono {

unsigned
ProfileInliner::depth_limit () const
{
	return inline_depth_limit ();
}

unsigned
ProfileInliner::round_limit () const
{
	return inline_round_limit ();
}

void
ProfileInliner::folded (Function &caller, Function &callee, const InlineCost &cost,
                        uint64_t count)
{
	if (!is_jit_trace_enabled ())
		return;

	MonoMethod *into = marked_method (caller);
	MonoMethod *what = marked_method (callee);

	if (into == nullptr || what == nullptr)
		return;

	char *host = mono_method_full_name (into, TRUE);
	char *folded = mono_method_full_name (what, TRUE);

	MONO_LOCK (jit_trace_mutex ())
	{
		if (cost.isVariable ())
			fprintf (stderr, "[llvm-jit] folding %s into %s at a site counted %"
			                 G_GUINT64_FORMAT " times: costs %d against a budget of %d\n",
			         folded, host, count, cost.getCost (), cost.getThreshold ());
		else
			fprintf (stderr, "[llvm-jit] folding %s into %s at a site counted %"
			                 G_GUINT64_FORMAT " times: %s\n",
			         folded, host, count, cost.getReason ());
	}
	g_free (folded);
	g_free (host);
}

void
ProfileInliner::declined (Function &caller, Function &callee, const InlineCost &cost,
                          uint64_t count)
{
	if (!is_jit_trace_enabled ())
		return;

	MonoMethod *into = marked_method (caller);
	MonoMethod *what = marked_method (callee);

	if (into == nullptr || what == nullptr)
		return;

	char *host = mono_method_full_name (into, TRUE);
	char *declined = mono_method_full_name (what, TRUE);

	MONO_LOCK (jit_trace_mutex ())
	{
		fprintf (stderr, "[llvm-jit] declining %s into %s at a site counted %"
		                 G_GUINT64_FORMAT " times: ",
		         declined, host, count);

		// A cost the model reached is a pair of numbers. The other answers are
		// a verdict it took without weighing, and those carry the words.
		if (cost.isVariable ())
			fprintf (stderr, "costs %d against a budget of %d\n", cost.getCost (),
			         cost.getThreshold ());
		else
			fprintf (stderr, "%s\n", cost.getReason ());
	}
	g_free (declined);
	g_free (host);
}

Error
ProfileInliner::bind_and_resolve (Module &module, size_t from, size_t to)
{
	if (Error err = bind_symbols (module))
		return err;

	timing::Scope timed (timing::Phase::resolve);

	return resolve_externals (
		*target_.jit, target_.domain,
		ArrayRef<ExternalSymbol> (externals_).slice (from, to - from),
		target_.publish_callee, module_symbols_);
}

ArrayRef<uint8_t>
ProfileInliner::profile_for (Function &decl)
{
	MonoMethod *callee = marked_method (decl);

	if (callee == nullptr)
		return {};

	MonoDomainMethod *dm = domain_method_find (target_.domain, callee);

	if (dm == nullptr)
		return {};

	std::optional<ProfileCounters> profile = MonoBackend::profile_of (*dm);

	if (!profile)
		return {};

	profile_scratch_ = build_profile (ArrayRef<ProfileCounters> (*profile));
	return profile_scratch_;
}

/// Prints why materialize () handed back nothing for \p callee, under the trace.
/// The site that asked keeps its call, and nothing else says this happened -
/// StripInlineCopiesPass never saw a body to take back off.
static void
trace_refusal (const InlineScope &scope, MonoMethod *callee, const char *why)
{
	if (!is_jit_trace_enabled () || scope.root == nullptr)
		return;

	char *host = mono_method_full_name (scope.root, TRUE);
	char *refused = mono_method_full_name (callee, TRUE);

	MONO_LOCK (jit_trace_mutex ())
	{
		fprintf (stderr, "[llvm-jit] refusing %s into %s: %s\n", refused, host, why);
	}
	g_free (refused);
	g_free (host);
}

Function *
ProfileInliner::materialize (Function &decl, Module &into, std::optional<SiteHeat> heat)
{
	MonoMethod *callee = marked_method (decl);

	if (callee == nullptr)
		return nullptr;

	/*
	 * The root folded this method already, so hand back the body standing
	 * beside it. Ahead of the tests below because none of them governs a body
	 * that is already translated.
	 *
	 * No cycle to rule out here, unlike the pre-pass. What this returns is a
	 * body the root's own module holds, and the pass folds it at one site under
	 * a depth limit rather than marking it always-inline.
	 */
	bool rebuild = already_folded (scope_, callee);

	if (rebuild) {
		if (callee == scope_.root)
			return nullptr;

		// A copy standing beside the root is what the site should reach.
		// Without one, fall through and build one into the candidate's module.
		if (Function *standing = folded_copy_in (scope_, callee, *decl.getParent ()))
			return standing;
	}

	// The site's heat picks the limit, so a hot site can be allowed a larger
	// body than a cold one - see costed_inline_il_limit_hot/cold ()'s own
	// comments for why the gate is tiered at all.
	uint32_t limit = costed_inline_il_limit ();

	if (heat == SiteHeat::hot)
		limit = costed_inline_il_limit_hot ();
	else if (heat == SiteHeat::cold)
		limit = costed_inline_il_limit_cold ();

	// A folded copy carries no sequence points of its own; see
	// materialize_trivial_callees (). A rebuild is free, so only a method new
	// to this root meets either budget below.
	if (limit == 0 || mini_get_debug_options ()->gen_sdb_seq_points)
		return nullptr;

	if (!rebuild && scope_.budget.costed == 0) {
		trace_refusal (scope_, callee, "the cost model's fold budget is spent");
		return nullptr;
	}

	if (!may_fold (target_.domain, callee)) {
		trace_refusal (scope_, callee, "may_fold () refuses it");
		return nullptr;
	}

	ERROR_DECL (metadata_error);
	MinimalCompile cfg (callee, target_.domain, metadata_error);
	MonoMethodHeader *header = cfg.get ()->header;

	// A callee whose metadata will not load costs the root a call, not a
	// compile.
	if (header == nullptr) {
		mono_error_cleanup (metadata_error);
		return nullptr;
	}

	bool fits = fold_clause_bearing_callees () ? is_small_enough (header, limit)
	                                           : is_small_and_clause_free (header, limit);

	if (!fits) {
		if (is_jit_trace_enabled ()) {
			char why[128];
			const char *at = heat == SiteHeat::hot  ? "hot"
			                 : heat == SiteHeat::cold ? "cold"
			                 : heat.has_value ()      ? "ordinary"
			                                          : "unranked";

			snprintf (why, sizeof why, "%u IL bytes%s over the limit of %u at a %s site",
			          header->code_size, header->num_clauses != 0 ? " with clauses" : "",
			          limit, at);
			trace_refusal (scope_, callee, why);
		}
		return nullptr;
	}

	// A count treats a 5-byte getter the same as a 250-byte body, so this is
	// the other half of the bound: what this root has left to translate, in
	// the unit compile time is actually spent in. Charged below, alongside the
	// count, once translation is committed to rather than merely fits.
	if (!rebuild && header->code_size > scope_.budget.costed_bytes) {
		trace_refusal (scope_, callee, "the cost model's byte budget is spent");
		return nullptr;
	}

	// getInlineCost refuses this callee outright once has_filter_clause () is
	// true (see inline-scope.hpp), so translating a copy would only strand its
	// filter function once StripInlineCopiesPass takes the copy back off.
	if (has_filter_clause (header))
		return nullptr;

	size_t resolved = externals_.size ();
	Function *copy = materialize_inline_copy (into, target_.domain, callee, cfg.get (),
	                                          externals_, types_, scope_,
	                                          Inliner::costed);

	if (copy == nullptr)
		return nullptr;

	// What the candidate itself named. The pre-pass below resolves each body as
	// it builds it, so the range here stays the candidate's own.
	size_t own = externals_.size ();

	// The candidate brings its own getters and forwarders with it, so the cost
	// model never has to weigh one and the shape test stays the only thing that
	// decides them.
	materialize_trivial_callees (into, target_.domain, callee, *copy, externals_,
	                             types_, scope_, [&] (ArrayRef<ExternalSymbol> named) {
		timing::Scope timed (timing::Phase::resolve);

		return resolve_externals (*target_.jit, target_.domain, named,
		                          target_.publish_callee, module_symbols_);
	});

	/*
	 * The module the candidate was built in goes with it when it is dropped,
	 * which is what leaves the root naming only what the compile had already
	 * resolved.
	 */
	if (Error err = bind_and_resolve (into, resolved, own)) {
		consumeError (std::move (err));
		return nullptr;
	}

	return copy;
}

} // namespace mono

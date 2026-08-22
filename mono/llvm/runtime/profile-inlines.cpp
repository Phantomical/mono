#include "runtime-error.hpp"

#include "profile-inlines.hpp"

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

void
ProfileInliner::folded (Function &caller, Function &callee)
{
	if (!is_jit_trace_enabled ())
		return;

	MonoMethod *into = marked_method (caller);
	MonoMethod *what = marked_method (callee);

	if (into == nullptr || what == nullptr)
		return;

	char *host = mono_method_full_name (into, TRUE);
	char *folded = mono_method_full_name (what, TRUE);

	fprintf (stderr, "[llvm-jit] folding %s into %s\n", folded, host);
	g_free (folded);
	g_free (host);
}

Error
ProfileInliner::bind_and_resolve (Module &module, size_t from)
{
	if (Error err = bind_symbols (module))
		return err;

	timing::Scope timed (timing::Phase::resolve);

	return resolve_externals (*target_.jit, target_.domain,
	                          ArrayRef<ExternalSymbol> (externals_).drop_front (from),
	                          target_.publish_callee, module_symbols_);
}

Function *
ProfileInliner::materialize (Function &decl, Module &into)
{
	uint32_t limit = costed_inline_il_limit ();

	// A breakpoint is armed on a method, and a folded copy carries none of the
	// method's sequence points.
	if (limit == 0 || scope_.budget == 0
	    || mini_get_debug_options ()->gen_sdb_seq_points)
		return nullptr;

	MonoMethod *callee = marked_method (decl);

	if (callee == nullptr || is_contained (scope_.folded, callee)
	    || !may_fold (target_.domain, callee))
		return nullptr;

	ERROR_DECL (metadata_error);
	MinimalCompile cfg (callee, target_.domain, metadata_error);
	MonoMethodHeader *header = cfg.get ()->header;

	// A callee whose metadata will not load costs the root a call, not a
	// compile.
	if (header == nullptr) {
		mono_error_cleanup (metadata_error);
		return nullptr;
	}

	// A clause-bearing body folded into a caller needs its clauses merged into
	// the caller's table, which is work of its own.
	if (header->num_clauses != 0 || header->code_size > limit)
		return nullptr;

	if (!loses_its_frame_safely (callee, header))
		return nullptr;

	size_t resolved = externals_.size ();
	Function *copy = materialize_inline_copy (into, target_.domain, callee, cfg.get (),
	                                          externals_, types_, scope_);

	if (copy == nullptr)
		return nullptr;

	// The candidate brings its own getters and forwarders with it, so the cost
	// model never has to weigh one and the shape test stays the only thing that
	// decides them.
	materialize_trivial_callees (into, target_.domain, callee, *copy, externals_,
	                             types_, scope_);

	/*
	 * A candidate on a path the root never runs must not decide what the root
	 * compiles to, so a body whose own callees will not resolve is dropped
	 * whole. The module it was built in goes with it, which is what leaves the
	 * root naming only what the compile had already resolved.
	 */
	if (Error err = bind_and_resolve (into, resolved)) {
		consumeError (std::move (err));
		return nullptr;
	}

	return copy;
}

} // namespace mono

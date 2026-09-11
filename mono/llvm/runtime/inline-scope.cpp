#include "runtime-error.hpp"

#include "inline-scope.hpp"

#include "domain-method.hpp"
#include "method-to-llvm/intrinsics.hpp"
#include "naming.hpp"
#include "options.hpp"
#include "passes/inline-copies.hpp"
#include "timing.hpp"

#include <llvm/ADT/STLExtras.h>
#include <llvm/ADT/SmallPtrSet.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>

#include <algorithm>

#include "mini.h"
#include "mini-runtime.h"

#include "mono/metadata/class-internals.h"
#include "mono/metadata/debug-helpers.h"
#include "mono/metadata/profiler-private.h"
#include "mono/metadata/tabledefs.h"

using namespace llvm;

namespace mono {

bool
folding_off_for_seq_points ()
{
	// A breakpoint is armed on a method, and a folded copy carries none of the
	// method's sequence points.
	if (!mini_get_debug_options ()->gen_sdb_seq_points)
		return false;

	// The debugger agent turns sequence points on when it starts, and an
	// embedding host starts it before it reads MONO_ENV_OPTIONS. Without this
	// line such a run would read as an inliner that found no candidate.
	if (is_jit_trace_enabled ()) {
		static std::once_flag traced;

		std::call_once (traced, [] {
			MONO_LOCK (jit_trace_mutex ())
			{
				fprintf (stderr, "[llvm-jit] inlining is disabled because"
				                 " sequence points are enabled\n");
			}
		});
	}

	return true;
}

bool
is_inlinable (MonoDomain *domain, MonoMethod *callee)
{
	// A dynamic method is freed on its own. A copy of its body folded into a
	// caller would outlive the data its constants point at.
	if (callee->dynamic)
		return false;

	// No IL of its own to translate.
	if (is_external_method (callee))
		return false;

	if ((callee->iflags & METHOD_IMPL_ATTRIBUTE_NOINLINING) != 0)
		return false;

	// The enter and leave events describe a frame, and a folded body has none.
	if (mono_profiler_get_call_instrumentation_flags (callee) != 0)
		return false;

	/*
	 * Native code behind a detour owns the entry, so a call to the method no
	 * longer runs the IL this would copy. This covers a compile that starts
	 * after the install. A copy that already stands is drop_folded_bodies ()'s
	 * to take down.
	 *
	 * Last because it takes the domain's table lock, and every test above reads
	 * a field.
	 */
	if (MonoDomainMethod *dm = domain_method_find (domain, callee))
		if (dm->tier () == MonoTier::detoured)
			return false;

	return true;
}

uint32_t
il_read_u32 (const unsigned char *at)
{
	return at[0] | (at[1] << 8) | (at[2] << 16) | ((uint32_t) at[3] << 24);
}

MonoMethod *
il_call_target (MonoMethod *method, uint32_t token)
{
	ERROR_DECL (metadata_error);
	MonoMethod *target =
		mono_get_method_checked (m_class_get_image (method->klass), token, nullptr,
	                                 mono_method_get_context (method), metadata_error);

	if (target == nullptr)
		mono_error_cleanup (metadata_error);

	return target;
}

bool
is_small_and_clause_free (MonoMethodHeader *header, uint32_t il_limit)
{
	return header->num_clauses == 0 && header->code_size <= il_limit;
}

bool
is_builtin (MonoMethod *method)
{
	return builtin_body_for (method) != nullptr;
}

bool
is_small_enough (MonoMethodHeader *header, uint32_t il_limit)
{
	return header->code_size <= il_limit;
}

bool
has_filter_clause (MonoMethodHeader *header)
{
	for (uint32_t i = 0; i < header->num_clauses; ++i)
		if (header->clauses[i].flags == MONO_EXCEPTION_CLAUSE_FILTER)
			return true;

	return false;
}

bool
already_folded (const InlineScope &scope, MonoMethod *callee)
{
	return any_of (scope.folded, [&] (const InlineScope::Folded &entry) {
		return entry.method == callee;
	});
}

// A copy is the only thing worth following. Every other call leaves the module
// through a published entry, so it cannot come back to a body that has none.
bool
copy_reaches (const Function &from, const Function &to)
{
	SmallPtrSet<const Function *, 8> seen;
	SmallVector<const Function *, 8> pending { &from };

	while (!pending.empty ()) {
		const Function *at = pending.pop_back_val ();

		if (at == &to)
			return true;

		if (!seen.insert (at).second)
			continue;

		for (const Instruction &i : instructions (*at)) {
			const auto *site = dyn_cast<CallBase> (&i);
			const Function *called =
				site != nullptr ? site->getCalledFunction () : nullptr;

			if (called != nullptr && !called->isDeclaration ()
			    && called->hasFnAttribute (inline_copy_attribute))
				pending.push_back (called);
		}
	}

	return false;
}

Function *
folded_copy_in (const InlineScope &scope, MonoMethod *callee, const Module &module)
{
	for (const InlineScope::Folded &entry : scope.folded) {
		if (entry.method != callee)
			continue;

		auto *copy = dyn_cast_or_null<Function> (entry.copy);

		return copy != nullptr && copy->getParent () == &module ? copy : nullptr;
	}

	return nullptr;
}

/// Returns this root's entry for callee, or null when it has not folded it.
static InlineScope::Folded *
find_folded (InlineScope &scope, MonoMethod *callee)
{
	for (InlineScope::Folded &entry : scope.folded)
		if (entry.method == callee)
			return &entry;

	return nullptr;
}

Function *
materialize_inline_copy (Module &module, MonoDomain *domain, MonoMethod *callee,
                         TranslateInput *cfg, std::vector<ExternalSymbol> &externals,
                         ModuleTypes &types, InlineScope &scope, Inliner who)
{
	// The root and the callee together name the copy. A module holds the
	// callee's own body as well, and one copy of it for each root that folds it
	// in.
	std::string suffix = "$copy" + identity_of (scope.root);

	// Read before the translation below records the new body against it. Null
	// says this root has not folded callee before, which is what the budget
	// counts.
	InlineScope::Folded *entry = find_folded (scope, callee);

	/*
	 * The translator declares the method it is asked for under a placeholder of
	 * its own and finds it again by that name. So ask it for the function it
	 * will build into, mark that one, and let the caller move its sites over.
	 */
	MethodLLVMEmitter declarer (&module, cfg, callee, &externals, nullptr,
	                            scope.defined, &types, suffix);
	Expected<Function *> target = declarer.declare (callee);

	if (!target) {
		consumeError (target.takeError ());
		return nullptr;
	}

	// A body already standing under that name says one of the two inliners
	// asked for this copy a second time, and the translation below appends to
	// the body that is there.
	g_assert ((*target)->isDeclaration ());

	// Before the translation rather than after it. A body it gives up halfway
	// through has to read as a copy too, or the sweep leaves a half-written
	// function behind under a name the linker cannot bind.
	mark_inline_copy (**target, stub_symbol (callee));

	Expected<Function *> materialized = [&] {
		timing::Scope timed (timing::Phase::translate);

		return method_to_llvm (&module, cfg, callee, &externals, nullptr, nullptr,
		                       scope.defined, &types, suffix);
	}();

	/*
	 * The budget counts the methods this root takes in, so the second body for
	 * one of them is free. A rebuild is the same method again, asked for
	 * because the copy went with a candidate's module or the pipeline erased
	 * it, and charging for it would spend two counts on one method.
	 *
	 * Spent either way for a method that is new here. A translation that failed
	 * cost as much as one that did not, and the budget is all that stops a
	 * second site from asking for the same body.
	 */
	if (entry == nullptr) {
		--scope.budget.of (who);

		// The byte budget is the costed inliner's own; the trivial pre-pass
		// keeps its candidates small through trivial_inline_il_limit () rather
		// than a running total.
		if (who == Inliner::costed) {
			uint32_t size = (uint32_t) cfg->header->code_size;

			scope.budget.costed_bytes -= std::min (scope.budget.costed_bytes, size);
		}
	}

	if (!materialized) {
		consumeError (materialized.takeError ());

		// A failed translation leaves whatever it built so far behind. Take
		// that back off, or the caller calls a body with no ret.
		(*target)->deleteBody ();
		return nullptr;
	}

	g_assert (*materialized == *target);

	/*
	 * Recorded because the body exists rather than because a fold happened: the
	 * fold is the pipeline's decision, and this is not where it is taken. What
	 * it costs when the body is stripped again is one wasted de-promotion of
	 * the root.
	 */
	if (Expected<MonoDomainMethod *> record = domain_method_get (domain, callee))
		(*record)->note_folded_into (scope.root);
	else
		consumeError (record.takeError ());

	/*
	 * A method this root folded before is here again because the copy it made
	 * then has gone: the pipeline erased it, or it belongs to a candidate's own
	 * module. The entry keeps the method and takes the body that stands now.
	 */
	if (entry != nullptr)
		entry->copy = *target;
	else
		scope.folded.push_back ({ callee, *target });
	return *target;
}

} // namespace mono

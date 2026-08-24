#include "runtime-error.hpp"

#include "inline-scope.hpp"

#include "domain-method.hpp"
#include "method-override.hpp"
#include "naming.hpp"
#include "passes/inline-copies.hpp"
#include "timing.hpp"

#include <llvm/ADT/STLExtras.h>
#include <llvm/ADT/SmallPtrSet.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>

#include <optional>
#include <string_view>

#include "mini.h"

#include "mono/metadata/class-internals.h"
#include "mono/metadata/debug-helpers.h"
#include "mono/metadata/opcodes.h"
#include "mono/metadata/profiler-private.h"
#include "mono/metadata/tabledefs.h"

using namespace llvm;

namespace mono {

bool
may_fold (MonoDomain *domain, MonoMethod *callee)
{
	// A wrapper is a frame the runtime's own walks look for. The stack trace
	// hides it, and an icall reads its caller from it. Several kinds are also
	// entered on terms this caller does not share.
	if (callee->wrapper_type != MONO_WRAPPER_NONE)
		return false;

	// A dynamic method is freed on its own. A copy of its body folded into a
	// caller would outlive the data its constants point at.
	if (callee->dynamic)
		return false;

	// No IL of its own to translate.
	if (implemented_outside_il (callee))
		return false;

	if ((callee->iflags & METHOD_IMPL_ATTRIBUTE_NOINLINING) != 0)
		return false;

	// A shared body is entered with a context this caller has no reason to
	// hold. What is worth folding is the instantiation, and a site naming one
	// is already a direct call.
	if (mono_method_check_context_used (callee) != 0)
		return false;

	// The enter and leave events describe a frame, and a folded body has none.
	if (mono_profiler_get_call_instrumentation_flags (callee) != 0)
		return false;

	/*
	 * A declared override is installed the first time anything asks for the
	 * method's record (domain_method_get ()), and a compile can reach the method
	 * as a callee before that happens. So ask the table rather than the record:
	 * folding the IL of a method something is about to replace bakes in the body
	 * the replacement is there to remove.
	 */
	if (registered_override_for (callee) != nullptr)
		return false;

	/*
	 * Something else owns the entry, so a call to the method no longer runs the
	 * IL this would copy: native code behind a detour, or the replacement behind
	 * an override installed through the icall, which the table above does not
	 * hold. Both cover a compile that starts after the install. A copy that
	 * already stands is drop_folded_bodies ()'s to take down.
	 *
	 * Last because it takes the domain's table lock, and every test above reads
	 * a field.
	 */
	if (MonoDomainMethod *dm = domain_method_find (domain, callee))
		if (dm->tier () == MonoTier::detoured || dm->override_method () != nullptr)
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

bool
reads_the_callers_frame (MonoMethod *target)
{
	MonoClass *klass = target->klass;

	/*
	 * An embedder registers the internal calls it wants, and mono_stack_walk ()
	 * is part of the API it can reach. Corlib is the one image whose set we
	 * have read.
	 */
	if (m_class_get_image (klass) != mono_get_corlib ())
		return true;

	std::string_view name_space (m_class_get_name_space (klass));
	std::string_view class_name (m_class_get_name (klass));
	std::string_view name (target->name);

	/*
	 * Reflection answers questions about the caller. The core-clr security
	 * checks in front of field access, method invoke and delegate creation read
	 * the caller too, and each of those sits on a reflection type.
	 */
	if (name_space == "System.Reflection")
		return true;

	if (name_space == "System.Diagnostics")
		return class_name == "StackTrace" || class_name == "StackFrame";
	if (name_space == "System.Runtime.Loader")
		return class_name == "AssemblyLoadContext" && name == "InternalLoadFile";
	if (name_space == "System.Threading")
		return class_name == "Thread" && name == "GetStackTraces";

	if (name_space == "System") {
		if (class_name == "AppDomain")
			return name == "LoadAssembly";
		if (class_name == "Delegate")
			return name == "CreateDelegate_internal";
		if (class_name == "RuntimeTypeHandle")
			return name == "internal_from_name";
	}

	return false;
}

bool
loses_its_frame_safely (MonoMethod *method, MonoMethodHeader *header)
{
	const unsigned char *code = header->code;
	size_t size = header->code_size;
	size_t at = 0;

	while (at < size) {
		const unsigned char *cursor = code + at;
		MonoOpcodeEnum op = mono_opcode_value (&cursor, code + size);

		if (op == MonoOpcodeEnum_Invalid)
			return false;

		size_t operand = (size_t) (cursor - code) + 1;
		size_t width = 0;

		// A switch carries its own table length, which is the one operand
		// il_operand_size () cannot answer for.
		if (op == MONO_CEE_SWITCH) {
			if (size - operand < 4)
				return false;

			uint32_t targets = il_read_u32 (code + operand);

			if (targets > (size - operand - 4) / 4)
				return false;
			width = 4 + (size_t) targets * 4;
		} else {
			std::optional<size_t> fixed = il_operand_size (op);

			if (!fixed)
				return false;
			width = *fixed;
		}

		if (size - operand < width)
			return false;

		if (op == MONO_CEE_CALLI)
			return false;

		if (op == MONO_CEE_CALL || op == MONO_CEE_CALLVIRT || op == MONO_CEE_NEWOBJ) {
			MonoMethod *target = il_call_target (method, il_read_u32 (code + operand));

			if (target == nullptr)
				return false;

			// A body with no IL keeps no frame of its own, so what it reports
			// comes from this one.
			if (implemented_outside_il (target) && reads_the_callers_frame (target))
				return false;
		}

		at = operand + width;
	}

	return true;
}

Function *
materialize_inline_copy (Module &module, MonoDomain *domain, MonoMethod *callee,
                         MonoCompile *cfg, std::vector<ExternalSymbol> &externals,
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
	if (entry == nullptr)
		--scope.budget.of (who);

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

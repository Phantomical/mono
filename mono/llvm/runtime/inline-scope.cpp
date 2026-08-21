#include "runtime-error.hpp"

#include "inline-scope.hpp"

#include "domain-method.hpp"
#include "naming.hpp"
#include "passes/inline-copies.hpp"
#include "timing.hpp"

#include <llvm/IR/Function.h>
#include <llvm/IR/Module.h>

#include <optional>

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
	// caller will outlive the data its constants point at.
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
	 * Native code owns the entry, so a call to the method no longer runs the IL
	 * this would copy. Last because it takes the domain's table lock, and every
	 * test above reads a field.
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

			// A body with no IL can be any of the runtime's stack walks, which
			// are all icalls. In this corlib GetCurrentMethod,
			// GetExecutingAssembly and GetCallingAssembly carry no NoInlining to
			// be read, so the mark alone does not find them.
			if (target == nullptr || implemented_outside_il (target)
			    || (target->iflags & METHOD_IMPL_ATTRIBUTE_NOINLINING) != 0)
				return false;
		}

		at = operand + width;
	}

	return true;
}

Function *
materialize_inline_copy (Module &module, MonoDomain *domain, MonoMethod *callee,
                         MonoCompile *cfg, Function &decl,
                         std::vector<ExternalSymbol> &externals, ModuleTypes &types,
                         InlineScope &scope)
{
	/*
	 * The translator declares the method it is asked for under a placeholder of
	 * its own and finds it again by that name. A declaration the engine has
	 * already renamed does not answer to it. So ask the translator for the
	 * function it will build into, and move the caller's sites onto that one.
	 */
	MethodLLVMEmitter declarer (&module, cfg, callee, &externals, nullptr,
	                            scope.defined, &types);
	Expected<Function *> target = declarer.declare (callee);

	if (!target) {
		consumeError (target.takeError ());
		return nullptr;
	}

	if (*target != &decl) {
		decl.replaceAllUsesWith (*target);
		decl.eraseFromParent ();
	}

	// Before the translation rather than after it. A body it gives up halfway
	// through has to read as a copy too, or the sweep leaves a half-written
	// function behind under a name the linker cannot bind.
	mark_inline_copy (**target, stub_symbol (callee));

	Expected<Function *> materialized = [&] {
		timing::Scope timed (timing::Phase::translate);

		return method_to_llvm (&module, cfg, callee, &externals, nullptr, nullptr,
		                       scope.defined, &types);
	}();

	// Spent either way. A translation that failed cost as much as one that did
	// not, and nothing else stops a second site from asking for the same body.
	--scope.budget;

	if (!materialized) {
		consumeError (materialized.takeError ());

		// A failed translation leaves whatever it got as far as behind. Take
		// that back off, or the caller calls a body with no ret.
		(*target)->deleteBody ();
		return nullptr;
	}

	g_assert (*materialized == *target);

	/*
	 * Recorded because the body exists rather than because a fold happened: the
	 * fold is the pipeline's decision, and this is not where it is taken. What
	 * it costs when the body is stripped again is one de-promotion of the root
	 * that nothing needed.
	 */
	if (Expected<MonoDomainMethod *> record = domain_method_get (domain, callee))
		(*record)->note_folded_into (scope.root);
	else
		consumeError (record.takeError ());

	scope.defined.push_back (callee);
	return *target;
}

} // namespace mono

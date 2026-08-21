#include "runtime-error.hpp"

#include "trivial-inlines.hpp"

#include "method-symbols.hpp"
#include "method-to-llvm.hpp"
#include "minimal-compile.hpp"
#include "options.hpp"
#include "passes/tier-counter.hpp"
#include "timing.hpp"

#include <llvm/ADT/STLExtras.h>
#include <llvm/ADT/SmallVector.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>

#include <optional>

#include "mini.h"
#include "mini-runtime.h"

#include "mono/metadata/class-internals.h"
#include "mono/metadata/debug-helpers.h"
#include "mono/metadata/opcodes.h"
#include "mono/metadata/profiler-private.h"
#include "mono/metadata/tabledefs.h"

using namespace llvm;

namespace mono {

namespace {

/// Marks a body this pre-pass materialized, so one the inliner did not fold in
/// can be found again.
constexpr StringRef inline_copy_attribute = "mono-inline-copy";

/// The reduced shape of a candidate's IL.
struct Shape {
	/// The method the single call site names, null when the body calls nothing.
	MonoMethod *forwards_to = nullptr;
};

/// Whether an opcode only computes a value, and does so without a branch, a
/// bounds check or a call.
bool
computes_a_value (MonoOpcodeEnum op)
{
	switch (op) {
	case MONO_CEE_NOP:
	case MONO_CEE_LDARG_0:
	case MONO_CEE_LDARG_1:
	case MONO_CEE_LDARG_2:
	case MONO_CEE_LDARG_3:
	case MONO_CEE_LDARG_S:
	case MONO_CEE_LDARG:
	case MONO_CEE_LDARGA_S:
	case MONO_CEE_LDARGA:
	case MONO_CEE_LDLOC_0:
	case MONO_CEE_LDLOC_1:
	case MONO_CEE_LDLOC_2:
	case MONO_CEE_LDLOC_3:
	case MONO_CEE_LDLOC_S:
	case MONO_CEE_LDLOC:
	case MONO_CEE_LDLOCA_S:
	case MONO_CEE_LDLOCA:
	case MONO_CEE_STLOC_0:
	case MONO_CEE_STLOC_1:
	case MONO_CEE_STLOC_2:
	case MONO_CEE_STLOC_3:
	case MONO_CEE_STLOC_S:
	case MONO_CEE_STLOC:
	case MONO_CEE_LDFLD:
	case MONO_CEE_LDFLDA:
	case MONO_CEE_LDSFLD:
	case MONO_CEE_LDSFLDA:
	// A property setter is a field access the same way its getter is. The write
	// barrier a reference field wants is emitted either way, so what the fold
	// takes off is the frame around it.
	case MONO_CEE_STFLD:
	case MONO_CEE_STSFLD:
	case MONO_CEE_STOBJ:
	case MONO_CEE_STIND_I1:
	case MONO_CEE_STIND_I2:
	case MONO_CEE_STIND_I4:
	case MONO_CEE_STIND_I8:
	case MONO_CEE_STIND_I:
	case MONO_CEE_STIND_R4:
	case MONO_CEE_STIND_R8:
	case MONO_CEE_STIND_REF:
	case MONO_CEE_LDIND_I1:
	case MONO_CEE_LDIND_U1:
	case MONO_CEE_LDIND_I2:
	case MONO_CEE_LDIND_U2:
	case MONO_CEE_LDIND_I4:
	case MONO_CEE_LDIND_U4:
	case MONO_CEE_LDIND_I8:
	case MONO_CEE_LDIND_I:
	case MONO_CEE_LDIND_R4:
	case MONO_CEE_LDIND_R8:
	case MONO_CEE_LDIND_REF:
	case MONO_CEE_LDOBJ:
	case MONO_CEE_LDNULL:
	case MONO_CEE_LDSTR:
	case MONO_CEE_LDC_I4_M1:
	case MONO_CEE_LDC_I4_0:
	case MONO_CEE_LDC_I4_1:
	case MONO_CEE_LDC_I4_2:
	case MONO_CEE_LDC_I4_3:
	case MONO_CEE_LDC_I4_4:
	case MONO_CEE_LDC_I4_5:
	case MONO_CEE_LDC_I4_6:
	case MONO_CEE_LDC_I4_7:
	case MONO_CEE_LDC_I4_8:
	case MONO_CEE_LDC_I4_S:
	case MONO_CEE_LDC_I4:
	case MONO_CEE_LDC_I8:
	case MONO_CEE_LDC_R4:
	case MONO_CEE_LDC_R8:
	case MONO_CEE_CONV_I1:
	case MONO_CEE_CONV_I2:
	case MONO_CEE_CONV_I4:
	case MONO_CEE_CONV_I8:
	case MONO_CEE_CONV_R4:
	case MONO_CEE_CONV_R8:
	case MONO_CEE_CONV_U1:
	case MONO_CEE_CONV_U2:
	case MONO_CEE_CONV_U4:
	case MONO_CEE_CONV_U8:
	case MONO_CEE_CONV_I:
	case MONO_CEE_CONV_U:
	case MONO_CEE_CONV_R_UN:
	// An argument a throw helper reports is often a boxed value.
	case MONO_CEE_BOX:
		return true;
	default:
		return false;
	}
}

/// Whether an opcode enters another method and comes back.
bool
enters_a_method (MonoOpcodeEnum op)
{
	return op == MONO_CEE_CALL || op == MONO_CEE_CALLVIRT || op == MONO_CEE_NEWOBJ;
}

/// Reads a four-byte little-endian operand: a metadata token or a displacement.
uint32_t
read_u32 (const unsigned char *at)
{
	return at[0] | (at[1] << 8) | (at[2] << 16) | ((uint32_t) at[3] << 24);
}

/// Whether a branch goes to the instruction behind it, which is a fallthrough
/// written out.
///
/// A C# compiler ends a method that returns a value with `stloc.0`, a branch
/// to the next instruction, then `ldloc.0` and `ret`. Reading that pattern as
/// a straight line is what lets shape_of recognize an ordinary getter or
/// forwarder.
bool
branches_to_the_next (const unsigned char *code, MonoOpcodeEnum op, size_t operand)
{
	// A displacement is counted from the instruction behind the branch, so zero
	// is that instruction.
	if (op == MONO_CEE_BR_S)
		return (int8_t) code[operand] == 0;
	if (op == MONO_CEE_BR)
		return (int32_t) read_u32 (code + operand) == 0;

	return false;
}

/// Returns the method a call site's token names, or null when the metadata
/// does not resolve it.
MonoMethod *
call_target (MonoMethod *method, uint32_t token)
{
	ERROR_DECL (metadata_error);
	MonoMethod *target =
		mono_get_method_checked (m_class_get_image (method->klass), token, nullptr,
	                                 mono_method_get_context (method), metadata_error);

	if (target == nullptr)
		mono_error_cleanup (metadata_error);

	return target;
}

/// Returns the method's shape when its IL is a straight line of value
/// opcodes. The line holds at most one call, and its terminator is the last
/// IL byte. Returns nullopt otherwise.
///
/// These are the shapes worth folding in without weighing them:
///
///   ldc.i4.1                       ret a constant
///   ret
///
///   ldarg.0  ldfld y  ldfld z      ret a chain of fields
///   ret
///
///   ldarg.0  ldarg.1  stfld x      write one
///   ret
///
///   ldarg.0  ldfld y  ldarg.1      forward to one other method
///   call  F
///   ret
///
///   ldarg.1  newobj X::.ctor       throw
///   throw
///
///   ldarg.1  newobj Y::.ctor       make an object and return it
///   ret
///
/// Every opcode on the value list is a load, a store or a conversion, so a
/// cost model has nothing to weigh in a line of them, however they are
/// arranged. Refusing a real branch
/// is what keeps the body to that one line, and it also leaves the method
/// with exactly one terminator.
std::optional<Shape>
shape_of (MonoMethod *method, MonoMethodHeader *header)
{
	const unsigned char *code = header->code;
	size_t size = header->code_size;
	Shape shape;
	size_t at = 0;

	while (at < size) {
		const unsigned char *cursor = code + at;
		MonoOpcodeEnum op = mono_opcode_value (&cursor, code + size);

		if (op == MonoOpcodeEnum_Invalid)
			return std::nullopt;

		size_t operand = (size_t) (cursor - code) + 1;
		std::optional<size_t> width = il_operand_size (op);

		if (!width || operand + *width > size)
			return std::nullopt;

		size_t next = operand + *width;

		if (op == MONO_CEE_RET || op == MONO_CEE_THROW) {
			if (next != size)
				return std::nullopt;

			return shape;
		}

		if (enters_a_method (op)) {
			if (shape.forwards_to != nullptr)
				return std::nullopt;

			shape.forwards_to = call_target (method, read_u32 (code + operand));
			if (shape.forwards_to == nullptr)
				return std::nullopt;
		} else if (!computes_a_value (op)
		           && !branches_to_the_next (code, op, operand)) {
			return std::nullopt;
		}

		at = next;
	}

	// The IL ran off its own end without reaching a terminator.
	return std::nullopt;
}

/// Whether a callee can be folded into its caller without changing what the
/// program does.
bool
may_fold (MonoMethod *callee)
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

	return true;
}

void
trace_inline (MonoMethod *callee, MonoMethod *caller)
{
	char *host = mono_method_full_name (caller, TRUE);
	char *what = mono_method_full_name (callee, TRUE);

	fprintf (stderr, "[llvm-jit] folding %s into %s\n", what, host);
	g_free (what);
	g_free (host);
}

} // namespace

void
materialize_trivial_callees (Module &module, MonoDomain *domain, MonoMethod *root,
                             Function &body, std::vector<ExternalSymbol> &externals,
                             ModuleTypes &types)
{
	uint32_t limit = trivial_inline_il_limit ();

	// A breakpoint is armed on a method, and a folded copy carries none of the
	// method's sequence points.
	if (limit == 0 || mini_get_debug_options ()->gen_sdb_seq_points)
		return;

	uint32_t budget = trivial_inline_budget ();
	SmallVector<MonoMethod *, 8> defined { root };
	// A body the module now holds, and the method it belongs to. A body reached
	// through another one is folded into that one first, so the pair is what a
	// trace has to name.
	SmallVector<std::pair<MonoMethod *, Function *>, 8> pending { { root, &body } };

	while (!pending.empty () && budget > 0) {
		auto [into, caller] = pending.pop_back_val ();
		SmallVector<Function *, 8> called;

		for (Instruction &i : instructions (*caller)) {
			auto *site = dyn_cast<CallBase> (&i);
			Function *decl =
				site != nullptr ? site->getCalledFunction () : nullptr;

			if (decl != nullptr && decl->isDeclaration ()
			    && !is_contained (called, decl))
				called.push_back (decl);
		}

		for (Function *decl : called) {
			if (budget == 0)
				break;

			// Another of this caller's callees forwards here and got there
			// first.
			if (!decl->isDeclaration ())
				continue;

			MonoMethod *callee = marked_method (*decl);

			if (callee == nullptr || is_contained (defined, callee)
			    || !may_fold (callee))
				continue;

			ERROR_DECL (metadata_error);
			MinimalCompile cfg (callee, domain, metadata_error);
			MonoMethodHeader *header = cfg.get ()->header;

			if (header == nullptr) {
				mono_error_cleanup (metadata_error);
				continue;
			}

			// A clause-bearing body folded into a caller needs its clauses
			// merged into the caller's table, which is work of its own.
			if (header->num_clauses != 0 || header->code_size > limit)
				continue;

			std::optional<Shape> shape = shape_of (callee, header);

			if (!shape)
				continue;

			// A helper that reads the frame it was called from carries
			// NoInlining - GetCurrentMethod and the rest do. Folding a
			// forwarder into its caller hands such a helper the caller's
			// frame instead of the forwarder's. The same test catches a body
			// that calls itself, which the inliner cannot fold away.
			if (shape->forwards_to != nullptr
			    && (shape->forwards_to == callee
			        || (shape->forwards_to->iflags
			            & METHOD_IMPL_ATTRIBUTE_NOINLINING) != 0))
				continue;

			if (is_jit_trace_enabled ())
				trace_inline (callee, into);

			// The translator builds the body into the declaration this caller
			// already has, so the call sites need no rewriting.
			Expected<Function *> materialized = [&] {
				timing::Scope timed (timing::Phase::translate);

				return method_to_llvm (&module, cfg.get (), callee, &externals,
				                       nullptr, nullptr, defined, &types);
			}();

			if (!materialized) {
				consumeError (materialized.takeError ());

				// A translation that fails partway can leave a partial body in
				// decl. Delete it, or the caller ends up calling a body with
				// no ret.
				if (!decl->isDeclaration ())
					decl->deleteBody ();
				continue;
			}

			g_assert (*materialized == decl);

			// These attributes tell the tier-counter pass which body to
			// instrument. They belong to the method's own body, not to this
			// copy of it.
			decl->removeFnAttr (tier_counter_attribute);
			decl->removeFnAttr (tier_handle_attribute);

			// Local linkage is what lets the inliner delete the copy once it
			// has folded every call to it.
			decl->setLinkage (GlobalValue::InternalLinkage);
			decl->addFnAttr (Attribute::AlwaysInline);
			decl->addFnAttr (inline_copy_attribute);

			defined.push_back (callee);
			pending.push_back ({ callee, decl });
			--budget;
		}
	}
}

Error
trivial_inlines_landed (const Module &module)
{
	for (const Function &fn : module) {
		if (fn.isDeclaration () || !fn.hasFnAttribute (inline_copy_attribute))
			continue;

		return createStringError (inconvertibleErrorCode (),
		                          "%s was materialized to be folded into its "
		                          "caller and the inliner did not fold it",
		                          fn.getName ().str ().c_str ());
	}

	return Error::success ();
}

} // namespace mono

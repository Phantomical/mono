#include "method-to-llvm.hpp"
#include "runtime-error.hpp"
#include "mono/metadata/class.h"
#include "mono/metadata/class-internals.h"
#include "mono/metadata/debug-helpers.h"
#include "mono/metadata/loader.h"
#include "mono/metadata/metadata-internals.h"
#include <llvm/IR/BasicBlock.h>

#include <string>

namespace mono {

namespace {

/// Returns the instruction at il_offset, in the format mono's disassembler
/// prints: "IL_0012: add". If the method body cannot be read, this returns
/// an empty string.
std::string
disassemble_one (MonoMethod *method, size_t il_offset)
{
	ERROR_DECL (error);
	MonoMethodHeader *header = mono_method_get_header_checked (method, error);
	std::string text;

	if (header == nullptr) {
		mono_error_cleanup (error);
		return text;
	}

	if (il_offset < header->code_size) {
		char *one = mono_disasm_code_one (NULL, method, header->code + il_offset, NULL);

		text = one;
		g_free (one);

		// The disassembler ends every instruction with a newline.
		while (!text.empty () && g_ascii_isspace (text.back ()))
			text.pop_back ();
	}

	mono_metadata_free_mh (header);
	return text;
}

/// Returns how many locals the method declares, or 0 if its body cannot be
/// read.
uint32_t
local_count (MonoMethod *method)
{
	ERROR_DECL (error);
	MonoMethodHeader *header = mono_method_get_header_checked (method, error);

	if (header == nullptr) {
		mono_error_cleanup (error);
		return 0;
	}

	uint32_t count = header->num_locals;

	mono_metadata_free_mh (header);
	return count;
}

} // namespace

bool
MethodLLVMEmitter::in_wrapper () const
{
	return method->wrapper_type != MONO_WRAPPER_NONE;
}

/// Whether index names one of the wrapper's stored data items.
///
/// Slot 0 of the table holds the entry count, not an entry. mono_mb_add_data ()
/// hands out indices starting at 1, so an index of 0, or one past the count, is
/// one the wrapper never filled in.
bool
MethodLLVMEmitter::has_wrapper_data (uint32_t index) const
{
	void **data = static_cast<void **> (((MonoMethodWrapper *) method)->method_data);

	return data != nullptr && index >= 1 && index <= GPOINTER_TO_UINT (data[0]);
}

void *
MethodLLVMEmitter::wrapper_data (uint32_t index) const
{
	if (!has_wrapper_data (index))
		return nullptr;

	return static_cast<void **> (((MonoMethodWrapper *) method)->method_data)[index];
}

/// Refuse the method with an InvalidProgramException that names the method and
/// the offending instruction. The interpreter raises the same exception for
/// invalid IL, so managed code sees one behavior regardless of the engine that
/// finds the problem.
llvm::Error
MethodLLVMEmitter::invalid_il (const llvm::Twine &reason)
{
	char *name = mono_method_full_name (method, TRUE);
	std::string instruction = disassemble_one (method, offset);
	ERROR_DECL (error);

	if (instruction.empty ())
		mono_error_set_invalid_program (error, "Invalid IL code in %s: IL_%04x: %s", name,
		                                static_cast<unsigned> (offset),
		                                reason.str ().c_str ());
	else
		mono_error_set_invalid_program (error, "Invalid IL code in %s: %s: %s", name,
		                                instruction.c_str (), reason.str ().c_str ());

	g_free (name);
	return runtime_error (error);
}

llvm::Error
MethodLLVMEmitter::unbalanced_stack (size_t needed)
{
	const char *what = needed > stack.size () ? "stack underflow, " : "unbalanced stack, ";

	return invalid_il (llvm::Twine (what) + llvm::Twine (needed) + " values expected but "
	                   + llvm::Twine (stack.size ()) + " on the stack");
}

llvm::Error
MethodLLVMEmitter::invalid_local (uint32_t index)
{
	return invalid_il (llvm::Twine ("local index ") + llvm::Twine (index)
	                   + " out of range, the method declares "
	                   + llvm::Twine (local_count (method)) + " locals");
}

llvm::Error
MethodLLVMEmitter::invalid_argument (uint32_t index)
{
	MonoMethodSignature *sig = mono_method_signature_internal (method);
	uint32_t count = sig->param_count + sig->hasthis;

	// Argument 0 of an instance method is the receiver. The signature does not list it.
	return invalid_il (llvm::Twine ("argument index ") + llvm::Twine (index)
	                   + " out of range, the method takes " + llvm::Twine (count)
	                   + (sig->hasthis ? " arguments including this" : " arguments"));
}

/// Give up on a method the backend cannot translate yet.
///
/// This raises an ExecutionEngineException, not the InvalidProgramException
/// above. The IL is valid. This engine has no translation for it yet. No other
/// JIT exists to hand the method to, so this refusal is what managed code sees
/// when it calls the method.
llvm::Error
MethodLLVMEmitter::unsupported_il (const llvm::Twine &what)
{
	char *name = mono_method_full_name (method, TRUE);
	std::string where = disassemble_one (method, offset);
	ERROR_DECL (error);

	if (where.empty ()) {
		char buffer[16];

		g_snprintf (buffer, sizeof (buffer), "IL_%04x", static_cast<unsigned> (offset));
		where = buffer;
	}

	mono_error_set_execution_engine (error, "Cannot translate %s: %s: %s", name,
	                                 where.c_str (), what.str ().c_str ());

	g_free (name);
	return runtime_error (error);
}

/**
 * Three kinds of methods skip the accessibility check.
 *
 * A method marked skip_visibility got that permission from whoever emitted it.
 * Reflection.Emit hands it out, and the runtime's own marshalling builders take
 * it for themselves.
 *
 * A wrapper's body belongs to the runtime, not to an image author, so it can
 * reach whatever the thing it wraps is made of.
 *
 * A corlib-internal assembly is one of the runtime's own halves of corlib, and
 * those are written against each other's private members on purpose.
 */
bool
MethodLLVMEmitter::checks_accessibility () const
{
	if (method->skip_visibility || in_wrapper ())
		return false;

	MonoImage *image = m_class_get_image (method->klass);

	return image->assembly == nullptr || !image->assembly->corlib_internal;
}

/// Refuse a field this method cannot reach.
///
/// The whole method fails to translate, not just this instruction. The
/// FieldAccessException appears only when something calls the method.
llvm::Error
MethodLLVMEmitter::field_access_failure (MonoClassField *field)
{
	char *field_name = mono_field_full_name (field);
	char *method_name = mono_method_full_name (method, TRUE);
	ERROR_DECL (error);

	mono_error_set_generic_error (error, "System", "FieldAccessException",
	                              "Field `%s' is inaccessible from method `%s'\n",
	                              field_name, method_name);

	g_free (method_name);
	g_free (field_name);
	return runtime_error (error);
}

/// Emits a throw of MethodAccessException in place of a call to callee.
///
/// Unlike a field, only the instruction is refused. Emission continues into a
/// block nothing branches to, so the stack keeps its shape. A path through the
/// body that never reaches this instruction still runs.
llvm::Error
MethodLLVMEmitter::emit_method_access_failure (MonoIrBuilder &builder, MonoMethod *callee)
{
	llvm::Expected<llvm::Function *> raise =
		icall_wrapper_decl (MONO_JIT_ICALL_mono_throw_method_access);

	if (!raise)
		return raise.takeError ();

	emit_unwinding_call (builder, *raise,
	                     adapt_to_callee (builder, *raise,
	                                      { method_symbol (method), method_symbol (callee) }));
	builder.SetInsertPoint (create_cold_block ("method_access"));
	return llvm::Error::success ();
}

/// Emits a throw of NotSupportedException in place of a call to callee.
///
/// The callee carries [UnmanagedCallersOnly], so its one published entry is in
/// the C convention. A call written in this engine's convention would arrive
/// with its arguments in the wrong places. Only the instruction is refused, the
/// way emit_method_access_failure () refuses one: a path through the body that
/// never reaches this call still runs.
llvm::Error
MethodLLVMEmitter::emit_unmanaged_callers_only_failure (MonoIrBuilder &builder,
                                                        MonoMethod *callee)
{
	llvm::Expected<llvm::Function *> raise =
		icall_wrapper_decl (MONO_JIT_ICALL_mono_throw_unmanaged_callers_only);

	if (!raise)
		return raise.takeError ();

	emit_unwinding_call (builder, *raise,
	                     adapt_to_callee (builder, *raise,
	                                      { method_symbol (method), method_symbol (callee) }));
	builder.SetInsertPoint (create_cold_block ("unmanaged_callers_only"));
	return llvm::Error::success ();
}

llvm::Error
MethodLLVMEmitter::truncated_il (size_t needed)
{
	return invalid_il (llvm::Twine ("truncated instruction, ") + llvm::Twine (needed)
	                   + " operand bytes needed but " + llvm::Twine (code_size - ip)
	                   + " left in the method body");
}

} // namespace mono

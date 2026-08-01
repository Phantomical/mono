#include "method-to-llvm.hpp"
#include "runtime-error.hpp"
#include "mono/metadata/debug-helpers.h"
#include "mono/metadata/loader.h"

#include <string>

namespace mono {

namespace {

/// The instruction at IL_OFFSET as mono's disassembler prints it - "IL_0012: add"
/// - or an empty string if the body cannot be read.
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

		/* The disassembler ends every instruction with a newline. */
		while (!text.empty () && g_ascii_isspace (text.back ()))
			text.pop_back ();
	}

	mono_metadata_free_mh (header);
	return text;
}

/// How many locals METHOD declares, or 0 if its body cannot be read.
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

/*
 * Slot 0 of the table holds how many entries follow it, so an index past that
 * is one the wrapper never filled in - a malformed body rather than something
 * to read off the end of the array.
 */
void *
MethodLLVMEmitter::wrapper_data (uint32_t index) const
{
	void **data = static_cast<void **> (((MonoMethodWrapper *) method)->method_data);

	if (data == nullptr || index > GPOINTER_TO_UINT (data[0]))
		return nullptr;

	return data[index];
}

/// Refuse the method the way mini refuses IL it cannot translate: an
/// InvalidProgramException naming the method and the offending instruction, so
/// that which JIT was asked to compile it does not change what the caller sees.
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

/// The current instruction wants NEEDED values that the evaluation stack is not
/// holding.
llvm::Error
MethodLLVMEmitter::unbalanced_stack (size_t needed)
{
	const char *what = needed > stack.size () ? "stack underflow, " : "unbalanced stack, ";

	return invalid_il (llvm::Twine (what) + llvm::Twine (needed) + " values expected but "
	                   + llvm::Twine (stack.size ()) + " on the stack");
}

/// The current instruction names a local the method does not declare.
llvm::Error
MethodLLVMEmitter::invalid_local (uint32_t index)
{
	return invalid_il (llvm::Twine ("local index ") + llvm::Twine (index)
	                   + " out of range, the method declares "
	                   + llvm::Twine (local_count (method)) + " locals");
}

/// The current instruction names an argument the method does not take.
llvm::Error
MethodLLVMEmitter::invalid_argument (uint32_t index)
{
	MonoMethodSignature *sig = mono_method_signature_internal (method);
	uint32_t count = sig->param_count + sig->hasthis;

	/* Argument 0 of an instance method is the receiver, which is not in the signature. */
	return invalid_il (llvm::Twine ("argument index ") + llvm::Twine (index)
	                   + " out of range, the method takes " + llvm::Twine (count)
	                   + (sig->hasthis ? " arguments including this" : " arguments"));
}

/// Give up on a method the backend cannot translate yet.
///
/// An ExecutionEngineException rather than the InvalidProgramException above: the
/// IL is well formed, and what went wrong is that this engine has no translation
/// for it. There is no other JIT to hand the method to, so the refusal is what
/// managed code sees when it calls the method.
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

/// The current instruction wants NEEDED more operand bytes than the method body has
/// left to give.
llvm::Error
MethodLLVMEmitter::truncated_il (size_t needed)
{
	return invalid_il (llvm::Twine ("truncated instruction, ") + llvm::Twine (needed)
	                   + " operand bytes needed but " + llvm::Twine (code_size - ip)
	                   + " left in the method body");
}

} // namespace mono

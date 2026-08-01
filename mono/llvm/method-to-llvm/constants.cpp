#include "method-to-llvm.hpp"
#include "runtime-error.hpp"
#include "mono/metadata/class-abi-details.h"
#include "mono/metadata/class-internals.h"
#include "mono/metadata/debug-helpers.h"
#include "mono/metadata/metadata.h"
#include "mono/metadata/tokentype.h"
#include <llvm/ADT/APFloat.h>
#include <llvm/ADT/APInt.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>

namespace mono {

/*
 * III.3.40  ldc.<type> - load numeric constant
 *
 *   Format          Assembly Format   Description
 *   20 <int32>      ldc.i4 num        Push num of type int32 onto the stack as int32.
 *   21 <int64>      ldc.i8 num        Push num of type int64 onto the stack as int64.
 *   22 <float32>    ldc.r4 num        Push num of type float32 onto the stack as F.
 *   23 <float64>    ldc.r8 num        Push num of type float64 onto the stack as F.
 *   16              ldc.i4.0          Push 0 onto the stack as int32.
 *   17              ldc.i4.1          Push 1 onto the stack as int32.
 *   18              ldc.i4.2          Push 2 onto the stack as int32.
 *   19              ldc.i4.3          Push 3 onto the stack as int32.
 *   1A              ldc.i4.4          Push 4 onto the stack as int32.
 *   1B              ldc.i4.5          Push 5 onto the stack as int32.
 *   1C              ldc.i4.6          Push 6 onto the stack as int32.
 *   1D              ldc.i4.7          Push 7 onto the stack as int32.
 *   1E              ldc.i4.8          Push 8 onto the stack as int32.
 *   15              ldc.i4.m1         Push -1 onto the stack as int32.
 *   15              ldc.i4.M1         Push -1 of type int32 onto the stack as int32
 *                                     (alias for ldc.i4.m1).
 *   1F <int8>       ldc.i4.s num      Push num onto the stack as int32, short form.
 *
 * Stack Transition:
 *
 *   ... -> ..., num
 *
 * Description:
 *
 *   The ldc num instruction pushes number num or some constant onto the stack. There
 *   are special short encodings for the integers -128 through 127 (with especially
 *   short encodings for -1 through 8). All short encodings push 4-byte integers on the
 *   stack. Longer encodings are used for 8-byte integers and 4- and 8-byte
 *   floating-point numbers, as well as 4-byte values that do not fit in the short
 *   forms.
 *
 *   There are three ways to push an 8-byte integer constant onto the stack
 *
 *     1. For constants that shall be expressed in more than 32 bits, use the ldc.i8
 *        instruction.
 *     2. For constants that require 9-32 bits, use the ldc.i4 instruction followed by
 *        a conv.i8.
 *     3. For constants that can be expressed in 8 or fewer bits, use a short form
 *        instruction followed by a conv.i8.
 *
 *   There is no way to express a floating-point constant that has a larger range or
 *   greater precision than a 64-bit IEC 60559:1989 number, since these representations
 *   are not portable across architectures.
 *
 * Exceptions:
 *
 *   None.
 *
 * Verifiability:
 *
 *   The ldc instruction is always verifiable.
 */
llvm::Error
MethodLLVMEmitter::emit_ldc_i4 (MonoIrBuilder &builder, int32_t value)
{
	push_stack (builder.getInt32 (static_cast<uint32_t> (value)), mono_get_int32_type ());
	return llvm::Error::success ();
}

llvm::Error
MethodLLVMEmitter::emit_ldc_i8 (MonoIrBuilder &builder, int64_t value)
{
	push_stack (builder.getInt64 (static_cast<uint64_t> (value)),
	            m_class_get_byval_arg (mono_defaults.int64_class));
	return llvm::Error::success ();
}

/*
 * The float constants arrive as the bit patterns the IL stream holds rather than as
 * host floats: an IL float32 is IEC 60559 whatever the machine that reads it does, and
 * an APFloat built from the bits says so without a type pun in between.
 *
 * Both keep the width they were written at. The CLI has one float type, F, but nothing
 * is gained by widening every ldc.r4 to double here - the operand tables already pick
 * the wider of the two when an R4 and an R8 meet.
 */
llvm::Error
MethodLLVMEmitter::emit_ldc_r4 (MonoIrBuilder &builder, uint32_t bits)
{
	llvm::APFloat value (llvm::APFloat::IEEEsingle (), llvm::APInt (32, bits));

	push_stack (llvm::ConstantFP::get (builder.getFloatTy (), value),
	            m_class_get_byval_arg (mono_defaults.single_class));
	return llvm::Error::success ();
}

llvm::Error
MethodLLVMEmitter::emit_ldc_r8 (MonoIrBuilder &builder, uint64_t bits)
{
	llvm::APFloat value (llvm::APFloat::IEEEdouble (), llvm::APInt (64, bits));

	push_stack (llvm::ConstantFP::get (builder.getDoubleTy (), value),
	            m_class_get_byval_arg (mono_defaults.double_class));
	return llvm::Error::success ();
}

/*
 * III.3.45  ldnull - load a null pointer
 *
 *   Format   Assembly Format   Description
 *   14       ldnull            Push a null reference on the stack.
 *
 * Stack Transition:
 *
 *   ... -> ..., null value
 *
 * Description:
 *
 *   The ldnull pushes a null reference (type O) on the stack. This is used to
 *   initialize locations before they become live or when they become dead.
 *
 *   [Rationale: It might be thought that ldnull is redundant: why not use ldc.i4.0 or
 *   ldc.i8.0 instead? The answer is that ldnull provides a size-agnostic null -
 *   analogous to an ldc.i instruction, which does not exist. However, even if CIL were
 *   to include an ldc.i instruction it would still benefit verification algorithms to
 *   retain the ldnull instruction because it makes type tracking easier. end rationale]
 *
 * Exceptions:
 *
 *   None.
 *
 * Verifiability:
 *
 *   The ldnull instruction is always verifiable, and produces a value of the null type
 *   (§III.1.8.1.2) that is assignable-to (§I.8.7.3) any other reference type.
 */
llvm::Error
MethodLLVMEmitter::emit_ldnull (MonoIrBuilder &builder)
{
	llvm::PointerType *ptr = llvm::PointerType::get (context (), 0);

	push_stack (llvm::ConstantPointerNull::get (ptr), mono_get_object_type ());
	return llvm::Error::success ();
}

/*
 * III.4.16  ldstr - load a literal string
 *
 *   Format     Assembly Format   Description
 *   72 <T>     ldstr string      Push a string object for the literal string.
 *
 * Stack Transition:
 *
 *   ..., -> ..., string
 *
 * Description:
 *
 *   The ldstr instruction pushes a new string object representing the literal stored
 *   in the metadata as string (which is a string literal).
 *
 *   By default, the CLI guarantees that the result of two ldstr instructions
 *   referring to two metadata tokens that have the same sequence of characters,
 *   return precisely the same string object (a process known as "string interning").
 *   This behavior can be controlled using the
 *   System.Runtime.CompilerServices.CompilationRelaxationsAttribute and the
 *   System.Runtime.CompilerServices.CompilationRelaxations.NoStringInterning (see
 *   Partition IV).
 *
 * Exceptions:
 *
 *   None.
 *
 * Correctness:
 *
 *   Correct CIL requires that string is a valid string literal metadata token.
 *
 * Verifiability:
 *
 *   There are no additional verification requirements.
 */
llvm::Error
MethodLLVMEmitter::emit_ldstr (MonoIrBuilder &builder, uint32_t token)
{
	/*
	 * A wrapper has no string heap to point into, so what it carries is a plain
	 * C string that the runtime turns into a managed one at the point of use.
	 * A dynamic method is the exception: its operand is already the MonoString.
	 */
	if (in_wrapper ()) {
		void *data = wrapper_data (token);

		if (data == nullptr)
			return invalid_il (llvm::Twine ("wrapper data slot ") + llvm::Twine (token)
			                   + " does not hold a string");

		llvm::Type *ptr = llvm::PointerType::get (context (), 0);
		llvm::Constant *literal = llvm::ConstantExpr::getIntToPtr (
			builder.getInt64 (reinterpret_cast<uint64_t> (data)), ptr);
		llvm::Value *value = literal;

		if (method->wrapper_type != MONO_WRAPPER_DYNAMIC_METHOD) {
			MonoJitICallInfo *info = mono_find_jit_icall_info (
				MONO_JIT_ICALL_mono_string_new_wrapper_internal);
			llvm::FunctionType *type =
				llvm::FunctionType::get (ptr, { ptr }, false);

			value = builder.CreateCall (
				llvm::FunctionCallee (
					type, address_symbol (std::string ("mono_icall_") + info->name,
			                                      const_cast<void *> (info->func))),
				{ literal });
		}

		push_stack (value, m_class_get_byval_arg (mono_defaults.string_class));
		return llvm::Error::success ();
	}

	if ((token & 0xff000000) != MONO_TOKEN_STRING)
		return invalid_il ("ldstr needs a string literal token");

	/*
	 * The interned string is a runtime object, so like a vtable it travels as a
	 * symbol the engine resolves - named by the image and token that intern it,
	 * which is all mono_ldstr itself needs.
	 */
	MonoImage *image = m_class_get_image (method->klass);
	char *symbol = g_strdup_printf ("mono_ldstr_%s_%08x", image->assembly_name, token);
	llvm::Constant *value = extern_symbol (symbol);

	g_free (symbol);
	push_stack (value, m_class_get_byval_arg (mono_defaults.string_class));
	return llvm::Error::success ();
}

/*
 * III.4.17  ldtoken - load the runtime representation of a metadata token
 *
 *   Format     Assembly Format   Description
 *   D0 <T>     ldtoken token     Convert metadata token to its runtime
 *                                representation.
 *
 * Stack Transition:
 *
 *   ... -> ..., RuntimeHandle
 *
 * Description:
 *
 *   The ldtoken instruction pushes a RuntimeHandle for the specified metadata token.
 *   The token shall be one of:
 *
 *     A methoddef, methodref or methodspec: pushes a RuntimeMethodHandle
 *     A typedef, typeref, or typespec: pushes a RuntimeTypeHandle
 *     A fielddef or fieldref: pushes a RuntimeFieldHandle
 *
 *   The value pushed on the stack can be used in calls to reflection methods in the
 *   system class library
 *
 * Exceptions:
 *
 *   None.
 *
 * Correctness:
 *
 *   Correct CIL requires that token describes a valid metadata token of the kinds
 *   listed above
 *
 * Verifiability:
 *
 *   There are no additional verification requirements.
 */
llvm::Error
MethodLLVMEmitter::emit_ldtoken (MonoIrBuilder &builder, uint32_t token)
{
	ERROR_DECL (metadata_error);
	MonoClass *handle_class = nullptr;
	gpointer handle = nullptr;

	if (in_wrapper ()) {
		/*
		 * Two consecutive slots: the handle, and the class saying which of the
		 * three kinds it is. Only the wrappers that can carry a token at all
		 * fill these in.
		 */
		if (method->wrapper_type != MONO_WRAPPER_DYNAMIC_METHOD &&
		    method->wrapper_type != MONO_WRAPPER_SYNCHRONIZED)
			return unsupported_il ("ldtoken in this kind of wrapper");

		handle = wrapper_data (token);
		handle_class = static_cast<MonoClass *> (wrapper_data (token + 1));

		if (handle == nullptr || handle_class == nullptr)
			return invalid_il (llvm::Twine ("wrapper data slot ") + llvm::Twine (token)
			                   + " does not hold a token");

		if (handle_class == mono_defaults.typehandle_class)
			handle = m_class_get_byval_arg (static_cast<MonoClass *> (handle));
	} else {
		handle = mono_ldtoken_checked (m_class_get_image (method->klass), token,
		                               &handle_class, mono_method_get_context (method),
		                               metadata_error);

		if (handle == nullptr)
			return runtime_error (metadata_error);
	}

	/*
	 * The handle is a runtime address - a MonoType, MonoMethod or MonoClassField -
	 * so each kind rides on the matching symbol family. A type's MonoType lives
	 * inside its MonoClass, hence the offset from the class symbol.
	 */
	llvm::Value *address;

	if (handle_class == mono_defaults.typehandle_class) {
		MonoClass *klass =
			mono_class_from_mono_type_internal (static_cast<MonoType *> (handle));

		address = builder.CreateGEP (
			builder.getInt8Ty (), class_symbol (klass, "mono_class_"),
			builder.getInt32 (static_cast<int32_t> (m_class_offsetof_byval_arg ())));
	} else if (handle_class == mono_defaults.methodhandle_class) {
		address = method_symbol (static_cast<MonoMethod *> (handle));
	} else if (handle_class == mono_defaults.fieldhandle_class) {
		address = field_symbol (static_cast<MonoClassField *> (handle));
	} else {
		/*
		 * mono_ldtoken_checked hands back exactly the three handle kinds above
		 * and reports anything else as a bad image before getting here, so this
		 * arm is only reachable if the runtime grows a new kind.
		 */
		return invalid_il ("ldtoken produced an unknown handle kind");
	}

	if (mono_class_value_size (handle_class, NULL) != TARGET_SIZEOF_VOID_P)
		return invalid_il ("a runtime handle is expected to wrap one pointer");

	MonoType *wrapper = m_class_get_byval_arg (handle_class);
	llvm::Expected<llvm::Type *> htype = convert_type (wrapper);
	if (!htype)
		return htype.takeError ();

	/*
	 * A vtype reaches LLVM as opaque bytes, so the handle is wrapped the way any
	 * struct is built: through memory, a pointer stored over the bytes it fills.
	 */
	llvm::Align align = type_alignment (wrapper);
	MonoIrBuilder entry (entry_block, entry_block->begin ());
	llvm::AllocaInst *temp = entry.CreateAlloca (*htype);

	temp->setAlignment (align);
	builder.CreateAlignedStore (address, temp, align);
	push_stack (builder.CreateAlignedLoad (*htype, temp, align), wrapper);
	return llvm::Error::success ();
}

/*
 * III.4.25  sizeof - load the size, in bytes, of a type
 *
 *   Format        Assembly Format   Description
 *   FE 1C <T>     sizeof typeTok    Push the size, in bytes, of a type as an unsigned
 *                                   int32.
 *
 * Stack Transition:
 *
 *   ..., -> ..., size (4 bytes, unsigned)
 *
 * Description:
 *
 *   Returns the size, in bytes, of a type. typeTok can be a generic parameter, a
 *   reference type or a value type.
 *
 *   For a reference type, the size returned is the size of a reference value of the
 *   corresponding type, not the size of the data stored in objects referred to by a
 *   reference value.
 *
 *   [Rationale: The definition of a value type can change between the time the CIL is
 *   generated and the time that it is loaded for execution. Thus, the size of the
 *   type is not always known when the CIL is generated. The sizeof instruction allows
 *   CIL code to determine the size at runtime without the need to call into the
 *   Framework class library. The computation can occur entirely at runtime or at
 *   CIL-to-native-code compilation time. sizeof returns the total size that would be
 *   occupied by each element in an array of this type - including any padding the
 *   implementation chooses to add. Specifically, array elements lie sizeof bytes
 *   apart. end rationale]
 *
 * Exceptions:
 *
 *   None.
 *
 * Correctness:
 *
 *   typeTok shall be a typedef, typeref, or typespec metadata token.
 *
 * Verifiability:
 *
 *   It is always verifiable.
 */
llvm::Error
MethodLLVMEmitter::emit_sizeof (MonoIrBuilder &builder, uint32_t token)
{
	llvm::Expected<MonoType *> type = element_type_from_token (token);
	if (!type)
		return type.takeError ();

	uint32_t size =
		mini_type_is_reference (*type)
			? TARGET_SIZEOF_VOID_P
			: mono_class_value_size (mono_class_from_mono_type_internal (*type), NULL);

	push_stack (builder.getInt32 (size), m_class_get_byval_arg (mono_defaults.uint32_class));
	return llvm::Error::success ();
}

} // namespace mono

#include "method-to-llvm.hpp"
#include "runtime-error.hpp"
#include "mono/metadata/class-abi-details.h"
#include "mono/metadata/class-internals.h"
#include "mono/metadata/debug-helpers.h"
#include "mono/metadata/domain-internals.h"
#include "mono/metadata/gc-internals.h"
#include "mono/metadata/metadata.h"
#include "mono/metadata/object-internals.h"
#include "mono/metadata/opcodes.h"
#include "mono/metadata/reflection-internals.h"
#include "mono/metadata/tokentype.h"
#include <llvm/ADT/APFloat.h>
#include <llvm/ADT/APInt.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>

#include <string>
#include <string_view>

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

// The bit patterns handed to emit_ldc_r4 and emit_ldc_r8 are the IL stream's IEC 60559
// encoding, not a host float value. An APFloat built directly from those bits carries
// the exact value across, with no type pun in between.
//
// Each constant keeps the width it was written at. The CLI has one float type, F, but
// nothing here needs to widen ldc.r4 to double. binary-numeric.cpp already widens to
// double when an R4 value and an R8 value meet in an operation.
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
	// A wrapper has no string heap to point into. It carries a plain C string
	// instead, and the runtime turns that into a managed string when the
	// wrapper runs.
	//
	// A dynamic method is the exception. Its operand is already the MonoString.
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

	// The interned string is a runtime object. Like a vtable, it travels as a
	// symbol the engine resolves.
	//
	// The compiler interns it here, rather than at run time, to match what the
	// interpreter does for the same instruction. mono/interp/transform.c also calls
	// mono_ldstr_checked () at this point, when it transforms a non-wrapper
	// method. Both rest on the same guarantee. An interned string is rooted and
	// never moves, so its address can outlive the compile.
	//
	// The runtime interns the string into the domain the code compiles for, not
	// into the thread's current domain. The root holds only while that domain
	// holds the code.
	MonoImage *image = m_class_get_image (method->klass);
	ERROR_DECL (intern_error);
	MonoString *interned = mono_ldstr_checked (cfg->domain, image,
	                                           mono_metadata_token_index (token),
	                                           intern_error);

	if (interned == nullptr)
		return runtime_error (intern_error);

	char *name = g_strdup_printf ("mono_ldstr_%s_%08x", image->assembly_name, token);
	llvm::Constant *value = address_symbol (identity_symbol (name, image), interned);

	g_free (name);
	push_stack (value, m_class_get_byval_arg (mono_defaults.string_class));
	return llvm::Error::success ();
}

/// Folds `ldtoken` and the `Type::GetTypeFromHandle` call behind it into the
/// System.Type the pair produces, and says whether it did.
///
/// A fold consumes the call, so `ip` moves past it. A refusal leaves `ip` where
/// it was, and the caller emits the handle the ordinary way.
///
/// `type` is what the token named, byref spelling included.
llvm::Expected<bool>
MethodLLVMEmitter::fold_type_from_handle (MonoIrBuilder &builder, MonoType *type)
{
	// One byte of opcode and four of token.
	if (code_size - ip < 5)
		return false;

	const unsigned char *cursor = code + ip;
	MonoOpcodeEnum next = mono_opcode_value (&cursor, code + code_size);

	if (next != MONO_CEE_CALL && next != MONO_CEE_CALLVIRT)
		return false;

	// Control reaches a leader from elsewhere as well, where the ldtoken above
	// did not run.
	if (blocks.count (ip) != 0)
		return false;

	/*
	 * The debugger stops at a statement start, and a fold takes the call's
	 * offset away. Only a symbol file can put a stop there. The other half of
	 * wants_seq_point_at () wants an empty evaluation stack, and the handle
	 * this instruction pushes sits on it.
	 *
	 * That helper cannot answer for the call, because it reads the stack as it
	 * stands now, one entry short of what the call sees.
	 */
	if (sym_seq_points && sym_seq_point_offsets.contains (static_cast<uint32_t> (ip)))
		return false;

	// mono_opcode_value () leaves the cursor on the opcode's last byte, not
	// past it. The token follows.
	size_t at = static_cast<size_t> (cursor - code) + 1;

	if (code_size - at < 4)
		return false;

	uint32_t token = static_cast<uint32_t> (code[at])
	                 | (static_cast<uint32_t> (code[at + 1]) << 8)
	                 | (static_cast<uint32_t> (code[at + 2]) << 16)
	                 | (static_cast<uint32_t> (code[at + 3]) << 24);
	llvm::Expected<MonoMethod *> target = resolve_method (token);

	// emit_call () reports a token that names no method, against its own
	// offset.
	if (!target) {
		llvm::consumeError (target.takeError ());
		return false;
	}

	if ((*target)->klass != mono_defaults.systemtype_class
	    || std::string_view ((*target)->name) != "GetTypeFromHandle")
		return false;

	MonoClass *klass = mono_class_from_mono_type_internal (type);
	llvm::Value *value = nullptr;

	if (depends_on_context (klass)) {
		// mini_get_rgctx_entry_slot () takes the class's byval_arg for a slot
		// that names a class. A byref type has no such spelling, so the site
		// keeps its call.
		if (type->byref)
			return false;

		llvm::Expected<llvm::Value *> fetched =
			rgctx_fetch (builder, MONO_RGCTX_INFO_REFLECTION_TYPE, klass);

		if (!fetched)
			return fetched.takeError ();

		value = *fetched;
	} else {
		/*
		 * mono_type_get_object_checked () allocates the object with
		 * mono_object_new_pinned (), because the runtime already stores it in
		 * vtables and in compiled code. So the address does not move and the
		 * constant stays correct.
		 *
		 * The object belongs to the domain the code compiles for, and the
		 * domain holds it for as long as it holds the code.
		 */
		ERROR_DECL (reflection_error);
		MonoReflectionType *object =
			mono_type_get_object_checked (cfg->domain, type, reflection_error);

		if (object == nullptr)
			return runtime_error (reflection_error);

		/*
		 * A type built through Reflection.Emit answers with the builder's own
		 * object, which is not pinned. mono_class_create_runtime_vtable ()
		 * registers a moving root for the vtable slot that holds one, and a
		 * constant in code can have no such root.
		 *
		 * Only a collector that moves needs that root, which is the same
		 * question MONO_GC_REGISTER_ROOT_IF_MOVING () asks before it registers
		 * one. The object stays reachable either way: the class holds a strong
		 * handle to it (mono_class_set_ref_info ()) until
		 * deregister_reflection_info_roots () drops it, and that is the domain
		 * teardown this code goes with.
		 */
		if (mono_gc_is_moving ()
		    && mono_object_class (object) != mono_defaults.runtimetype_class)
			return false;

		char *name = mono_type_full_name (type);
		std::string symbol =
			identity_symbol (std::string ("mono_typeof_") + name, object);

		g_free (name);
		value = address_symbol (symbol, object);
	}

	ip = at + 4;
	push_stack (value, m_class_get_byval_arg (mono_defaults.systemtype_class));
	return true;
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
		// wrapper_data () holds the handle at token and the class that names
		// its kind at token + 1. Only the wrapper types that can carry a token
		// fill in these two slots.
		if (method->wrapper_type != MONO_WRAPPER_DYNAMIC_METHOD &&
		    method->wrapper_type != MONO_WRAPPER_SYNCHRONIZED)
			return unsupported_il ("ldtoken in this kind of wrapper");

		handle = wrapper_data (token);
		handle_class = static_cast<MonoClass *> (wrapper_data (token + 1));

		if (handle == nullptr || handle_class == nullptr)
			return invalid_il (llvm::Twine ("wrapper data slot ") + llvm::Twine (token)
			                   + " does not hold a token");

		if (handle_class == mono_defaults.typehandle_class) {
			MonoClass *klass = static_cast<MonoClass *> (handle);
			MonoGenericContext *ctx = mono_method_get_context (method);

			// mono_marshal_get_synchronized_wrapper () builds a synchronized
			// wrapper once, over the generic definition, then inflates it per
			// instantiation. Its class token still names Gen<T>, so this code
			// must put the real instantiation back.
			//
			// Without that step, every Gen<X> locks typeof (Gen<>) and locks
			// each other out.
			if (ctx != nullptr && mono_class_is_gtd (klass)) {
				klass = mono_class_inflate_generic_class_checked (klass, ctx,
				                                                  metadata_error);

				if (klass == nullptr)
					return runtime_error (metadata_error);
			}

			handle = m_class_get_byval_arg (klass);
		}
	} else {
		handle = mono_ldtoken_checked (m_class_get_image (method->klass), token,
		                               &handle_class, mono_method_get_context (method),
		                               metadata_error);

		if (handle == nullptr)
			return runtime_error (metadata_error);
	}

	// A C# compiler writes typeof (T) as this instruction and a call to
	// Type::GetTypeFromHandle. The pair has one answer, and it is known here.
	if (handle_class == mono_defaults.typehandle_class) {
		llvm::Expected<bool> folded =
			fold_type_from_handle (builder, static_cast<MonoType *> (handle));

		if (!folded)
			return folded.takeError ();

		if (*folded)
			return llvm::Error::success ();
	}

	// The handle is a runtime address: a MonoType, a MonoMethod or a
	// MonoClassField. Each kind rides on its matching symbol family.
	//
	// A MonoType lives inside its MonoClass, so a type handle reads as an
	// offset from the class symbol.
	llvm::Value *address;

	if (handle_class == mono_defaults.typehandle_class) {
		MonoType *type = static_cast<MonoType *> (handle);
		MonoClass *klass = mono_class_from_mono_type_internal (type);

		// mono_class_from_mono_type_internal () switches on type->type alone,
		// so a typespec's byref flag does not survive the round trip through
		// the class.
		//
		// Every class keeps both spellings of itself: this_arg is byval_arg
		// with byref set. Pick the one the token named.
		size_t offset = type->byref ? m_class_offsetof_this_arg ()
		                            : m_class_offsetof_byval_arg ();

		llvm::Expected<llvm::Value *> cls = class_operand (builder, klass, "mono_class_");

		if (!cls)
			return cls.takeError ();

		address = builder.CreateGEP (builder.getInt8Ty (), *cls,
		                             builder.getInt32 (static_cast<int32_t> (offset)));
	} else if (handle_class == mono_defaults.methodhandle_class) {
		llvm::Expected<llvm::Value *> named =
			method_operand (builder, static_cast<MonoMethod *> (handle));

		if (!named)
			return named.takeError ();

		address = *named;
	} else if (handle_class == mono_defaults.fieldhandle_class) {
		llvm::Expected<llvm::Value *> named =
			field_operand (builder, static_cast<MonoClassField *> (handle));

		if (!named)
			return named.takeError ();

		address = *named;
	} else {
		// mono_ldtoken_checked () only ever returns one of the three handle
		// kinds above. It reports anything else as a bad image before this
		// code runs, so this arm is unreachable unless the runtime adds a new
		// kind.
		return invalid_il ("ldtoken produced an unknown handle kind");
	}

	if (mono_class_value_size (handle_class, NULL) != TARGET_SIZEOF_VOID_P)
		return invalid_il ("a runtime handle is expected to wrap one pointer");

	MonoType *wrapper = m_class_get_byval_arg (handle_class);
	llvm::Expected<llvm::Type *> htype = convert_type (wrapper);
	if (!htype)
		return htype.takeError ();

	// The handle is one pointer wide. The code below stores that pointer into
	// the slot it occupies on the stack.
	llvm::Align align = type_alignment (wrapper);
	MonoIrBuilder entry (entry_block, entry_block->begin ());
	llvm::AllocaInst *temp = entry.CreateAlloca (*htype);

	temp->setAlignment (align);
	builder.CreateAlignedStore (address, temp, align);
	push_stack (temp, wrapper);
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

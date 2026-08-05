#include "method-to-llvm.hpp"
#include "mono/metadata/abi-details.h"
#include "mono/metadata/class-abi-details.h"
#include "mono/metadata/class-internals.h"
#include "mono/metadata/metadata.h"
#include "mono/metadata/object-internals.h"
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Type.h>

namespace mono {

/// The evaluation-stack value at DEPTH spilled to a fresh slot, so its fields can
/// be addressed. A typed reference only ever travels as a value, and both of its
/// readers start by putting it back in memory.
llvm::Value *
MethodLLVMEmitter::spill_to_temporary (MonoIrBuilder &builder, MonoType *type)
{
	StackValue value = get_stack (0);
	MonoIrBuilder entry (entry_block, entry_block->begin ());
	llvm::AllocaInst *temp = entry.CreateAlloca (value.value->getType ());

	temp->setAlignment (type_alignment (type));
	builder.CreateAlignedStore (value.value, temp, temp->getAlign ());
	return temp;
}

/*
 * III.3.4  arglist - get argument list
 *
 *   Format   Assembly Format   Description
 *   FE 00    arglist           Return argument list handle for the current method.
 *
 * Stack Transition:
 *
 *   ... -> ..., argListHandle
 *
 * Description:
 *
 *   The arglist instruction returns an opaque handle (having type
 *   System.RuntimeArgumentHandle) representing the argument list of the current
 *   method. This handle is valid only during the lifetime of the current method. The
 *   handle can, however, be passed to other methods as long as the current method is
 *   on the thread of control. The arglist instruction can only be executed within a
 *   method that takes a variable number of arguments.
 *
 *   [Rationale: This instruction is needed to implement the C 'va_*' macros used to
 *   implement procedures like 'printf'. It is intended for use with the class library
 *   implementation of System.ArgIterator. end rationale]
 *
 * Exceptions:
 *
 *   None.
 *
 * Correctness:
 *
 *   It is incorrect CIL generation to emit this instruction except in the body of a
 *   method whose signature indicates it accepts a variable number of arguments.
 *
 * Verifiability:
 *
 *   Its use is verifiable within the body of a method whose signature indicates it
 *   accepts a variable number of arguments, but verification requires that the result
 *   be an instance of the System.RuntimeArgumentHandle class.
 */
llvm::Error
MethodLLVMEmitter::emit_arglist (MonoIrBuilder &builder)
{
	if (sig_cookie == nullptr) {
		/* A filter runs as its own function and is not handed the cookie. */
		if (filter_mode)
			return unsupported_il ("arglist in a filter clause of a vararg method");

		return invalid_il ("arglist outside a vararg method");
	}

	/*
	 * A RuntimeArgumentHandle is a struct wrapping the buffer address, and what
	 * ArgIterator's constructor takes is the struct, so the pointer goes back
	 * through memory to come out in whatever shape the field converted to.
	 */
	MonoType *handle = m_class_get_byval_arg (mono_defaults.argumenthandle_class);
	llvm::Expected<llvm::Type *> slot = convert_type (handle);
	if (!slot)
		return slot.takeError ();

	MonoIrBuilder entry (entry_block, entry_block->begin ());
	llvm::AllocaInst *temp = entry.CreateAlloca (*slot);

	temp->setAlignment (type_alignment (handle));
	builder.CreateAlignedStore (sig_cookie, temp, temp->getAlign ());
	push_stack (builder.CreateAlignedLoad (*slot, temp, temp->getAlign ()), handle);
	return llvm::Error::success ();
}

/*
 * III.4.19  mkrefany - push a typed reference on the stack
 *
 *   Format     Assembly Format   Description
 *   C6 <T>     mkrefany class    Push a typed reference to ptr of type class onto the
 *                                stack.
 *
 * Stack Transition:
 *
 *   ..., ptr -> ..., typedRef
 *
 * Description:
 *
 *   The mkrefany instruction supports the passing of dynamically typed references.
 *   ptr shall be a pointer (type &, or native int) that holds the address of a piece
 *   of data. class is the class token (a typeref, typedef or typespec; see Partition
 *   II) describing the type of ptr. mkrefany pushes a typed reference on the stack,
 *   that is an opaque descriptor of ptr and class. This instruction enables the
 *   passing of dynamically typed references as arguments. The callee can use the
 *   refanytype and refanyval instructions to retrieve the type (class) and address
 *   (ptr) respectively of the parameter.
 *
 * Exceptions:
 *
 *   System.TypeLoadException is thrown if class cannot be found. This is typically
 *   detected when CIL is converted to native code rather than at runtime.
 *
 * Correctness:
 *
 *   Correct CIL ensures that class is a valid typeref or typedef or typespec token
 *   describing some type and that ptr is a pointer to exactly that type.
 *
 * Verifiability:
 *
 *   Verification additionally requires that ptr be a managed pointer. Verification
 *   will fail if it cannot deduce that ptr is a pointer to an instance of class.
 */
llvm::Error
MethodLLVMEmitter::emit_mkrefany (MonoIrBuilder &builder, uint32_t token)
{
	llvm::Expected<MonoType *> type = element_type_from_token (token);
	if (!type)
		return type.takeError ();

	MonoClass *klass = mono_class_from_mono_type_internal (*type);

	if (stack.empty ())
		return unbalanced_stack (1);

	llvm::Value *ptr = get_stack (0).value;

	if (!ptr->getType ()->isPointerTy ())
		ptr = builder.CreateIntToPtr (ptr, llvm::PointerType::get (context (), 0));

	MonoType *tref = m_class_get_byval_arg (mono_defaults.typed_reference_class);
	llvm::Expected<llvm::Type *> slot = convert_type (tref);
	if (!slot)
		return slot.takeError ();

	MonoIrBuilder entry (entry_block, entry_block->begin ());
	llvm::AllocaInst *temp = entry.CreateAlloca (*slot);

	temp->setAlignment (type_alignment (tref));

	/*
	 * The descriptor is the class, a pointer to the class's own MonoType - which
	 * lives inside the MonoClass, so it is the same symbol at an offset - and the
	 * address being described.
	 */
	llvm::Constant *cls = class_symbol (klass, "mono_class_");
	llvm::Align align (TARGET_SIZEOF_VOID_P);
	auto field = [&] (size_t offset) {
		return builder.CreateGEP (builder.getInt8Ty (), temp,
		                          builder.getInt32 (static_cast<int32_t> (offset)));
	};

	builder.CreateAlignedStore (cls, field (MONO_STRUCT_OFFSET (MonoTypedRef, klass)),
	                            align);
	builder.CreateAlignedStore (
		builder.CreateGEP (builder.getInt8Ty (), cls,
	                           builder.getInt32 (static_cast<int32_t> (
					   m_class_offsetof_byval_arg ()))),
		field (MONO_STRUCT_OFFSET (MonoTypedRef, type)), align);
	builder.CreateAlignedStore (ptr, field (MONO_STRUCT_OFFSET (MonoTypedRef, value)),
	                            align);

	pop_stack (1);
	push_stack (builder.CreateAlignedLoad (*slot, temp, temp->getAlign ()), tref);
	return llvm::Error::success ();
}

/*
 * III.4.23  refanyval - load the address out of a typed reference
 *
 *   Format     Assembly Format   Description
 *   C2 <T>     refanyval type    Push the address stored in a typed reference.
 *
 * Stack Transition:
 *
 *   ..., TypedRef -> ..., address
 *
 * Description:
 *
 *   Retrieves the address (of type &) embedded in TypedRef. The type of reference in
 *   TypedRef shall match the type specified by type (a metadata token, either a
 *   typedef, typeref or a typespec; see Partition II). See the mkrefany instruction.
 *
 * Exceptions:
 *
 *   System.InvalidCastException is thrown if type is not identical to the type
 *   stored in the TypedRef (ie, the class supplied to the mkrefany instruction that
 *   constructed that TypedRef)
 *
 *   System.TypeLoadException is thrown if type cannot be found.
 *
 * Correctness:
 *
 *   Correct CIL ensures that TypedRef is a valid typed reference (created by a
 *   previous call to mkrefany).
 *
 * Verifiability:
 *
 *   The refanyval instruction is always verifiable.
 */
llvm::Error
MethodLLVMEmitter::emit_refanyval (MonoIrBuilder &builder, uint32_t token)
{
	llvm::Expected<MonoType *> type = element_type_from_token (token);
	if (!type)
		return type.takeError ();

	MonoClass *klass = mono_class_from_mono_type_internal (*type);

	if (stack.empty ())
		return unbalanced_stack (1);

	MonoType *tref = m_class_get_byval_arg (mono_defaults.typed_reference_class);
	llvm::Value *temp = spill_to_temporary (builder, tref);
	llvm::Type *ptr = llvm::PointerType::get (context (), 0);
	llvm::Align align (TARGET_SIZEOF_VOID_P);

	llvm::Value *held = builder.CreateAlignedLoad (
		ptr,
		builder.CreateGEP (builder.getInt8Ty (), temp,
	                           builder.getInt32 (MONO_STRUCT_OFFSET (MonoTypedRef, klass))),
		align);

	emit_cond_exception (builder,
	                     builder.CreateICmpNE (held, class_symbol (klass, "mono_class_")),
	                     "InvalidCastException");

	llvm::Value *value = builder.CreateAlignedLoad (
		ptr,
		builder.CreateGEP (builder.getInt8Ty (), temp,
	                           builder.getInt32 (MONO_STRUCT_OFFSET (MonoTypedRef, value))),
		align);

	pop_stack (1);
	push_stack (value, m_class_get_this_arg (klass));
	return llvm::Error::success ();
}

/*
 * III.4.22  refanytype - load the type out of a typed reference
 *
 *   Format   Assembly Format   Description
 *   FE 1D    refanytype        Push the type token stored in a typed reference.
 *
 * Stack Transition:
 *
 *   ..., TypedRef -> ..., type
 *
 * Description:
 *
 *   Retrieves the type token embedded in TypedRef. See the mkrefany instruction.
 *
 * Exceptions:
 *
 *   None.
 *
 * Correctness:
 *
 *   Correct CIL ensures that TypedRef is a valid typed reference (created by a
 *   previous call to mkrefany).
 *
 * Verifiability:
 *
 *   The refanytype instruction is always verifiable.
 */
llvm::Error
MethodLLVMEmitter::emit_refanytype (MonoIrBuilder &builder)
{
	if (stack.empty ())
		return unbalanced_stack (1);

	MonoType *tref = m_class_get_byval_arg (mono_defaults.typed_reference_class);
	llvm::Value *temp = spill_to_temporary (builder, tref);

	/*
	 * What gets pushed is a RuntimeTypeHandle, a struct wrapping the MonoType
	 * pointer, so the field reads back out as that struct.
	 */
	MonoType *handle = m_class_get_byval_arg (mono_defaults.typehandle_class);
	llvm::Expected<llvm::Type *> slot = convert_type (handle);
	if (!slot)
		return slot.takeError ();

	llvm::Value *value = builder.CreateAlignedLoad (
		*slot,
		builder.CreateGEP (builder.getInt8Ty (), temp,
	                           builder.getInt32 (MONO_STRUCT_OFFSET (MonoTypedRef, type))),
		llvm::Align (TARGET_SIZEOF_VOID_P));

	pop_stack (1);
	push_stack (value, handle);
	return llvm::Error::success ();
}

} // namespace mono

#include "method-to-llvm.hpp"
#include "mono/metadata/class-internals.h"
#include "mono/metadata/metadata.h"
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Type.h>

#include <cstring>

namespace mono {

/*
 * III.4.21  newobj - create a new object
 *
 *   Format     Assembly Format   Description
 *   73 <T>     newobj ctor       Allocate an uninitialized object or value type and
 *                                call ctor.
 *
 * Stack Transition:
 *
 *   ..., arg1, ... argN -> ..., obj
 *
 * Description:
 *
 *   The newobj instruction creates a new object or a new instance of a value type.
 *   ctor is a metadata token (a methodref or methodef that shall be marked as a
 *   constructor; see Partition II) that indicates the name, class, and signature of
 *   the constructor to call. If a constructor exactly matching the indicated name,
 *   class and signature cannot be found, MissingMethodException is thrown.
 *
 *   The newobj instruction allocates a new instance of the class associated with
 *   ctor and initializes all the fields in the new instance to 0 (of the proper
 *   type) or null as appropriate. It then calls the constructor with the given
 *   arguments along with the newly created instance. After the constructor has been
 *   called, the now initialized object reference is pushed on the stack.
 *
 *   From the constructor's point of view, the uninitialized object is argument 0 and
 *   the other arguments passed to newobj follow in order.
 *
 *   All zero-based, one-dimensional arrays are created using newarr, not newobj. On
 *   the other hand, all other arrays (more than one dimension, or one-dimensional
 *   but not zero-based) are created using newobj.
 *
 *   Value types are not usually created using newobj. They are usually allocated
 *   either as arguments or local variables, using newarr (for zero-based,
 *   one-dimensional arrays), or as fields of objects. Once allocated, they are
 *   initialized using initobj. However, the newobj instruction can be used to create
 *   a new instance of a value type on the stack, that can then be passed as an
 *   argument, stored in a local, etc.
 *
 * Exceptions:
 *
 *   System.InvalidOperationException is thrown if ctor's class is abstract.
 *
 *   System.MethodAccessException is thrown if ctor is inaccessible.
 *
 *   System.OutOfMemoryException is thrown if there is insufficient memory to satisfy
 *   the request.
 *
 *   System.MissingMethodException is thrown if a constructor method with the
 *   indicated name, class, and signature could not be found. This is typically
 *   detected when CIL is converted to native code, rather than at runtime.
 *
 * Correctness:
 *
 *   Correct CIL ensures that ctor is a valid methodref or methoddef token, and that
 *   the arguments on the stack are assignable-to (§I.8.7.3) the parameters of the
 *   constructor.
 */
llvm::Error
MethodLLVMEmitter::emit_newobj (MonoIrBuilder &builder, uint32_t token)
{
	llvm::Expected<MonoMethod *> target = resolve_method (token);
	if (!target)
		return target.takeError ();

	MonoMethodSignature *sig = mono_method_signature_internal (*target);

	if (sig == nullptr)
		return invalid_il ("the constructor has no signature");
	if (!sig->hasthis || strcmp ((*target)->name, ".ctor") != 0)
		return invalid_il ("newobj needs an instance constructor");

	MonoClass *klass = (*target)->klass;

	/* Multi-dimensional and non-zero-based arrays construct through newobj. */
	if (m_class_get_rank (klass) != 0)
		return unsupported_il ("newobj on an array type");
	/* A string constructor computes its length first and returns the string. */
	if ((*target)->string_ctor)
		return unsupported_il ("newobj on a string constructor");

	llvm::Expected<llvm::Function *> declaration = create_method_decl (*target);
	if (!declaration)
		return declaration.takeError ();

	size_t count = sig->param_count;

	if (stack.size () < count)
		return unbalanced_stack (count);

	/*
	 * The constructor sees the fresh instance as argument 0 and the operands
	 * follow, so the stack unwinds into positions 1..N.
	 */
	std::vector<llvm::Value *> args (count + 1);

	for (size_t i = 0; i < count; ++i) {
		llvm::Expected<llvm::Value *> converted =
			coerce_to_location (builder, get_stack (count - 1 - i), sig->params[i]);

		if (!converted)
			return converted.takeError ();
		args[i + 1] = *converted;
	}

	MonoType *pushed = m_class_get_byval_arg (klass);
	llvm::Value *created = nullptr;
	llvm::AllocaInst *temp = nullptr;
	llvm::Type *slot = nullptr;
	llvm::Align align = type_alignment (pushed);

	if (m_class_is_valuetype (klass)) {
		/*
		 * A value type constructs in place, so the instance is a zeroed slot
		 * and what gets pushed afterwards is the value read back out of it.
		 */
		llvm::Expected<llvm::Type *> type = convert_type (pushed);
		if (!type)
			return type.takeError ();

		MonoIrBuilder entry (entry_block, entry_block->begin ());

		slot = *type;
		temp = entry.CreateAlloca (slot);
		temp->setAlignment (align);
		builder.CreateMemSet (temp, builder.getInt8 (0),
		                      mono_class_value_size (klass, NULL), align);
		args[0] = temp;
	} else {
		created = emit_protected_call (builder, object_new_decl (),
		                               {class_symbol (klass, "mono_vtable_")});
		args[0] = created;
	}

	emit_protected_call (builder, *declaration, args);
	pop_stack (count);

	if (temp != nullptr)
		push_stack (builder.CreateAlignedLoad (slot, temp, align), pushed);
	else
		push_stack (created, pushed);

	return llvm::Error::success ();
}

} // namespace mono

#include "method-to-llvm.hpp"
#include "mono/metadata/class-internals.h"
#include "mono/metadata/metadata.h"
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Type.h>

namespace mono {

/// Pushes argument `index` onto the evaluation stack, which is what ECMA-335
/// III.3.38 calls ldarg. The pushed value has the argument's intermediate
/// type, not its declared type.
llvm::Error
MethodLLVMEmitter::emit_ldarg (MonoIrBuilder &builder, uint32_t index)
{
	if (index >= args.size ())
		return invalid_argument (index);

	const Entry &argument = args[index];

	return push_from_location (builder, argument.alloca, argument.type, argument.native);
}

/// Pushes the address of argument `index` onto the stack as a managed
/// pointer, which is what ECMA-335 III.3.39 calls ldarga.
llvm::Error
MethodLLVMEmitter::emit_ldarga (MonoIrBuilder &builder, uint32_t index)
{
	if (index >= args.size ())
		return invalid_argument (index);

	const Entry &argument = args[index];
	MonoClass *klass = mono_class_from_mono_type_internal (argument.type);

	// The alloca already uses the argument's natural alignment. That is
	// the alignment ldarga's address must have.
	push_stack (argument.alloca, m_class_get_this_arg (klass));
	return llvm::Error::success ();
}

/// Pops a value off the stack and stores it into argument slot `index`,
/// which is what ECMA-335 III.3.61 calls starg.
llvm::Error
MethodLLVMEmitter::emit_starg (MonoIrBuilder &builder, uint32_t index)
{
	if (index >= args.size ())
		return invalid_argument (index);
	if (stack.empty ())
		return unbalanced_stack (1);

	const Entry &argument = args[index];
	llvm::Expected<llvm::Value *> value =
		coerce_to_location (builder, get_stack (0), argument.type, argument.native);
	if (!value)
		return value.takeError ();

	pop_stack (1);
	if (held_in_memory (argument.type))
		copy_vtype (builder, argument.alloca, *value, argument.type, argument.native);
	else
		builder.CreateAlignedStore (*value, argument.alloca,
		                            type_alignment (argument.type, argument.native));
	return llvm::Error::success ();
}

} // namespace mono

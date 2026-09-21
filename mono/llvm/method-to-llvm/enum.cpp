/**
 * \file
 * \brief Compiling Enum.HasFlag () as a symbolic call the pipeline settles or
 * lowers back to the method itself.
 */

#include "method-to-llvm.hpp"

#include "passes/enum-flag.hpp"

#include <llvm/IR/Function.h>
#include <llvm/IR/Value.h>

namespace mono {

llvm::Error
MethodLLVMEmitter::emit_enum_has_flag (MonoIrBuilder &builder, MonoMethod *callee_method,
                                       MonoMethodSignature *sig)
{
	llvm::Expected<llvm::Function *> fallback = create_method_decl (callee_method);

	if (!fallback)
		return fallback.takeError ();

	llvm::Expected<std::vector<llvm::Value *>> args = pop_call_arguments (builder, sig);

	if (!args)
		return args.takeError ();

	llvm::Value *call_args[] = { (*args)[0], (*args)[1], *fallback };
	llvm::Value *result =
		emit_protected_call (builder, enum_hasflag_decl (*module), call_args);

	pop_stack (sig->param_count + sig->hasthis);
	return push_produced (builder, builder.CreateZExt (result, builder.getInt8Ty ()),
	                      sig->ret);
}

} // namespace mono

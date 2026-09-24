/**
 * \file
 * \brief Compiling a System.Enum operation as a symbolic call the pipeline
 * settles or lowers back to the method itself.
 */

#include "method-to-llvm.hpp"

#include "passes/enum.hpp"

#include <llvm/IR/Function.h>
#include <llvm/IR/Value.h>

namespace mono {

llvm::Error
MethodLLVMEmitter::emit_enum_builtin (MonoIrBuilder &builder, llvm::StringRef name,
                                      MonoMethod *callee_method, MonoMethodSignature *sig)
{
	llvm::Expected<llvm::Function *> fallback =
		create_method_decl (icall_wrapper_target (callee_method));

	if (!fallback)
		return fallback.takeError ();

	llvm::Expected<std::vector<llvm::Value *>> args = pop_call_arguments (builder, sig);

	if (!args)
		return args.takeError ();

	args->push_back (*fallback);
	llvm::Value *result =
		emit_protected_call (builder, enum_builtin_decl (*module, name), *args);

	pop_stack (sig->param_count + sig->hasthis);
	return push_produced (builder, result, sig->ret);
}

} // namespace mono

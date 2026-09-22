#include "method-to-llvm.hpp"

#include "../runtime/options.hpp"

#include "mono/metadata/metadata.h"

namespace mono {

llvm::Error
MethodLLVMEmitter::emit_assume (MonoIrBuilder &builder, MonoMethodSignature *sig)
{
	llvm::Expected<std::vector<llvm::Value *>> args = pop_call_arguments (builder, sig);
	if (!args)
		return args.takeError ();

	if (assume_hints ())
		builder.CreateAssumption (builder.CreateIsNotNull ((*args)[0]));

	pop_stack (sig->param_count);
	return llvm::Error::success ();
}

} // namespace mono

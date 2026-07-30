#include "method-to-llvm.hpp"
#include <llvm/IR/Attributes.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Type.h>
#include <llvm/Support/ErrorHandling.h>

namespace mono {


llvm::Expected<llvm::Function *>
method_to_llvm (llvm::Module *module, MonoCompile *cfg, MonoMethod *method)
{
	auto emitter = MethodLLVMEmitter (module, cfg, method);
	return emitter.emit ();
}

llvm::Expected<llvm::Function *>
MethodLLVMEmitter::emit ()
{
	llvm::report_fatal_error("not implemented");
}

llvm::LLVMContext &
MethodLLVMEmitter::context () const
{
	return module->getContext ();
}

} // namespace mono

#ifndef MONO_LLVM_METHOD_TO_LLVM_HPP
#define MONO_LLVM_METHOD_TO_LLVM_HPP

#include "mini.h"
#include "mono/metadata/metadata.h"
#include "mono/metadata/object-forward.h"

// This breaks some LLVM headers
#undef PIC

#include <llvm/ADT/DenseMap.h>
#include <llvm/Support/Error.h>
#include <llvm/IR/Module.h>

namespace mono {

struct MonoLLVMMethod {
	std::unique_ptr<llvm::Module> module;
	llvm::Function *function;
};

/// An error describing something the converter could not express in LLVM IR.
inline llvm::Error
conversion_error (const llvm::Twine &reason)
{
	return llvm::createStringError (llvm::inconvertibleErrorCode (), reason);
}

class MethodLLVMEmitter {
public:
	llvm::Module *module;
	llvm::Function *function;

	MonoCompile *cfg;
	MonoMethod *method;

	llvm::DenseMap<MonoMethod *, llvm::Function *> declarations;
	llvm::DenseMap<MonoClass *, llvm::Type *> vtypes;

public:
	MethodLLVMEmitter (llvm::Module *module, MonoCompile *cfg, MonoMethod *method)
	    : module (module), function (nullptr), cfg (cfg), method (method)
	{
	}

	llvm::Expected<llvm::Function *> emit ();

private:
	llvm::LLVMContext &context () const;

	llvm::Expected<llvm::Function *> create_method_decl (MonoMethod *method);
	llvm::Expected<llvm::FunctionType *> convert_method_signature (MonoMethodSignature *sig);

	llvm::Expected<llvm::Type *> convert_type (MonoType *t);
	llvm::Expected<llvm::Type *> convert_vtype (MonoType *t);
};

/// Convert an IL method definition to the corresponding LLVM method.
llvm::Expected<llvm::Function *> method_to_llvm (llvm::Module *module, MonoCompile *cfg, MonoMethod *method);

} // namespace mono

#endif

/**
 * \file
 * \brief The IR-to-object compiler every module goes through.
 */

#ifndef MONO_LLVM_COMPILER_HPP
#define MONO_LLVM_COMPILER_HPP

#include <llvm/ExecutionEngine/Orc/IRCompileLayer.h>
#include <llvm/ExecutionEngine/Orc/JITTargetMachineBuilder.h>

#include <memory>

namespace mono {

/// The stock codegen pipeline, opened up so the exception machinery can ride
/// along: the clause gather runs over the final machine function, and the side
/// tables the runtime reads back - the clause table and the frame description -
/// are written into the object next to the code they describe.
///
/// A fresh TargetMachine is built per call, so the compiler carries no mutable
/// cross-call state and stays safe under concurrent compiles.
class MethodObjectCompiler : public llvm::orc::IRCompileLayer::IRCompiler {
public:
	explicit MethodObjectCompiler (llvm::orc::JITTargetMachineBuilder jtmb);

	llvm::Expected<std::unique_ptr<llvm::MemoryBuffer>>
	operator() (llvm::Module &m) override;

private:
	llvm::orc::JITTargetMachineBuilder jtmb_;
};

} // namespace mono

#endif /* MONO_LLVM_COMPILER_HPP */

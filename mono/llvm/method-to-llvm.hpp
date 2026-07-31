#ifndef MONO_LLVM_METHOD_TO_LLVM_HPP
#define MONO_LLVM_METHOD_TO_LLVM_HPP

#include "mini.h"
#include "mono/metadata/metadata.h"
#include "mono/metadata/object-forward.h"
#include <llvm-18/llvm/ADT/Twine.h>
#include <llvm-18/llvm/IR/IRBuilder.h>
#include <llvm-18/llvm/IR/Value.h>

// This breaks some LLVM headers
#undef PIC

#include <llvm/ADT/DenseMap.h>
#include <llvm/Support/Error.h>
#include <llvm/IR/Module.h>

namespace mono {

/// The instructions covered by ECMA-335 III.1.5, Table III.2.
enum class BinaryNumericOp { Add, Div, Mul, Rem, Sub };

struct MonoLLVMMethod {
	std::unique_ptr<llvm::Module> module;
	llvm::Function *function;
};

class MethodLLVMEmitter {
private:
	struct Entry {
		llvm::Value *alloca;
		MonoType *type;
	};

	/// A value on the evaluation stack, with the type the CLI tracks it as.
	///
	/// LLVM's own type is not enough to decide what an instruction may do with it:
	/// i64 covers both int64 and native int, and a pointer covers both a managed
	/// pointer and an object reference, which are the distinctions the operand
	/// tables in ECMA-335 III.1.5 turn on.
	struct StackValue {
		llvm::Value *value;
		MonoType *type;
	};

	llvm::Module *module;
	llvm::Function *function;
	llvm::IRBuilder<> builder;

	MonoCompile *cfg;
	MonoMethod *method;

	llvm::DenseMap<MonoMethod *, llvm::Function *> declarations;
	llvm::DenseMap<MonoClass *, llvm::Type *> vtypes;
	std::vector<Entry> args;
	std::vector<Entry> locals;
	std::vector<StackValue> stack;

	size_t offset = 0;

public:
	MethodLLVMEmitter (llvm::Module *module, MonoCompile *cfg, MonoMethod *method)
	    : module (module),
	      function (nullptr),
	      builder (module->getContext ()),
	      cfg (cfg),
	      method (method)
	{
	}

	llvm::Expected<llvm::Function *> emit ();

private:
	typedef llvm::IRBuilder<> MonoIrBuilder;

	llvm::LLVMContext &context () const;

	llvm::Expected<llvm::Function *> create_method_decl (MonoMethod *method);
	llvm::Expected<llvm::FunctionType *> convert_method_signature (MonoMethodSignature *sig);

	llvm::Expected<llvm::Type *> convert_type (MonoType *t);
	llvm::Expected<llvm::Type *> convert_vtype (MonoType *t);
	llvm::Align type_alignment (MonoType *t);

	llvm::Error invalid_il (const llvm::Twine &reason);
	llvm::Error unbalanced_stack (size_t needed);
	llvm::Error invalid_local (uint32_t index);
	llvm::Error invalid_argument (uint32_t index);

	llvm::Expected<MonoType *> binary_numeric_result (BinaryNumericOp op, MonoType *lhs,
	                                                  MonoType *rhs);

	llvm::Error emit_arg_allocas (MonoIrBuilder &builder);
	llvm::Error emit_local_allocas (MonoIrBuilder &builder);

	llvm::Error emit_add (MonoIrBuilder &builder);

private:
	StackValue get_stack (size_t index) const
	{
		if (index >= stack.size ())
			llvm::report_fatal_error ("stack index out of bounds ("
			                          + llvm::Twine (index)
			                          + " >= " + llvm::Twine (stack.size ()) + ")");

		return stack[stack.size () - 1 - index];
	}

	void push_stack (llvm::Value *value, MonoType *type)
	{
		stack.push_back ({ value, type });
	}

	void pop_stack (size_t count)
	{
		if (count > stack.size ())
			llvm::report_fatal_error ("stack pop count out of bounds ("
			                          + llvm::Twine (count)
			                          + " >= " + llvm::Twine (stack.size ()) + ")");

		stack.resize (stack.size () - count);
	}
};

/// Convert an IL method definition to the corresponding LLVM method.
llvm::Expected<llvm::Function *> method_to_llvm (llvm::Module *module, MonoCompile *cfg,
                                                 MonoMethod *method);

} // namespace mono

#endif

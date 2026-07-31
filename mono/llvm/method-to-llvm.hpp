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

/// The binary arithmetic instructions, in the order of the three operand tables in
/// ECMA-335 III.1.5 that say what each one accepts: Table III.2 binary numeric,
/// Table III.5 integer, Table III.7 overflow arithmetic.
enum class BinaryOp {
	Add,
	Div,
	Mul,
	Rem,
	Sub,

	DivUn,
	RemUn,

	AddOvf,
	AddOvfUn,
	MulOvf,
	MulOvfUn,
	SubOvf,
	SubOvfUn,
};

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

	/// The two operands of a binary numeric operation and the type it leaves behind.
	struct BinaryOperands {
		StackValue value1;
		StackValue value2;
		MonoType *result;
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

	/// The method's IL, the offset of the instruction being emitted, and how far into
	/// that instruction its operands have been read.
	///
	/// `offset` stays at the start of the instruction while `ip` walks its operands,
	/// so that a refusal names the instruction that caused it rather than the one
	/// after it.
	const unsigned char *code = nullptr;
	size_t code_size = 0;
	size_t offset = 0;
	size_t ip = 0;

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
	llvm::Error truncated_il (size_t needed);

	llvm::Expected<MonoType *> binary_result (BinaryOp op, MonoType *lhs, MonoType *rhs);
	llvm::Expected<BinaryOperands> pop_binary_operands (BinaryOp op);

	llvm::Error emit_arg_allocas (MonoIrBuilder &builder);
	llvm::Error emit_local_allocas (MonoIrBuilder &builder);

	void emit_throw_corlib_exception (MonoIrBuilder &builder, const char *name);
	void emit_cond_exception (MonoIrBuilder &builder, llvm::Value *condition,
	                          const char *name);

	void emit_division_guards (MonoIrBuilder &builder, llvm::Value *lhs, llvm::Value *rhs,
	                           bool is_signed);
	llvm::Value *emit_checked (MonoIrBuilder &builder, llvm::Intrinsic::ID intrinsic,
	                           llvm::Value *lhs, llvm::Value *rhs);
	llvm::Value *emit_checked_pointer_offset (MonoIrBuilder &builder, llvm::Value *base,
	                                          llvm::Value *index, bool subtract);

	llvm::Error emit_add (MonoIrBuilder &builder);
	llvm::Error emit_sub (MonoIrBuilder &builder);
	llvm::Error emit_mul (MonoIrBuilder &builder);
	llvm::Error emit_div (MonoIrBuilder &builder);
	llvm::Error emit_rem (MonoIrBuilder &builder);

	llvm::Error emit_div_un (MonoIrBuilder &builder);
	llvm::Error emit_rem_un (MonoIrBuilder &builder);

	llvm::Error emit_add_ovf (MonoIrBuilder &builder, bool is_unsigned);
	llvm::Error emit_mul_ovf (MonoIrBuilder &builder, bool is_unsigned);
	llvm::Error emit_sub_ovf (MonoIrBuilder &builder, bool is_unsigned);

	llvm::Expected<llvm::Value *> coerce_to_location (MonoIrBuilder &builder, StackValue value,
	                                                  MonoType *destination);

	llvm::Error emit_ldloc (MonoIrBuilder &builder, uint32_t index);
	llvm::Error emit_ldloca (MonoIrBuilder &builder, uint32_t index);
	llvm::Error emit_stloc (MonoIrBuilder &builder, uint32_t index);

private:
	/// The next byte of the IL stream, or a refusal if the instruction runs off the
	/// end of the method body.
	llvm::Expected<uint8_t> read_u8 ()
	{
		if (code_size - ip < 1)
			return truncated_il (1);

		return code[ip++];
	}

	/// The next two bytes, little-endian - which is how IL stores them whatever the
	/// machine running it does.
	llvm::Expected<uint16_t> read_u16 ()
	{
		if (code_size - ip < 2)
			return truncated_il (2);

		uint16_t value = static_cast<uint16_t> (code[ip] | (code[ip + 1] << 8));

		ip += 2;
		return value;
	}

	/// The next four bytes, little-endian.
	llvm::Expected<uint32_t> read_u32 ()
	{
		if (code_size - ip < 4)
			return truncated_il (4);

		uint32_t value = static_cast<uint32_t> (code[ip])
		                 | (static_cast<uint32_t> (code[ip + 1]) << 8)
		                 | (static_cast<uint32_t> (code[ip + 2]) << 16)
		                 | (static_cast<uint32_t> (code[ip + 3]) << 24);

		ip += 4;
		return value;
	}

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

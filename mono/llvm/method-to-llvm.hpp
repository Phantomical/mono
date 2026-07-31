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

#include <string>

namespace mono {

/// The instructions that take two operands from one of the tables in ECMA-335 III.1.5,
/// grouped by the table that says what each one accepts: Table III.2 binary numeric,
/// Table III.5 integer, Table III.6 shift, Table III.7 overflow arithmetic.
enum class BinaryOp {
	Add,
	Div,
	Mul,
	Rem,
	Sub,

	DivUn,
	RemUn,
	And,
	Or,
	Xor,

	Shl,
	Shr,
	ShrUn,

	AddOvf,
	AddOvfUn,
	MulOvf,
	MulOvfUn,
	SubOvf,
	SubOvfUn,
};

/// The six types the CLI tracks on the evaluation stack (ECMA-335 III.1.5), and a
/// seventh for everything that cannot appear as an operand of one.
///
/// This is the axis every operand table in III.1.5 is indexed by, so the arithmetic and
/// conversion tables are both laid out along it.
enum StackType { Int32, Int64, NativeInt, Float, ManagedPtr, ObjectRef, Invalid };

constexpr size_t STACK_TYPE_COUNT = ObjectRef + 1;

/// The type a conv instruction converts to, from the opcode tables in ECMA-335
/// III.3.27 through III.3.29.
enum class ConvType {
	I1,
	U1,
	I2,
	U2,
	I4,
	U4,
	I8,
	U8,
	I,
	U,
	R4,
	R8,
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
	llvm::Error unsupported_il (const llvm::Twine &what);

	static StackType stack_type (MonoType *t);
	static std::string describe (MonoType *t, StackType type);
	static llvm::Value *coerce (MonoIrBuilder &builder, llvm::Value *value,
	                            llvm::Type *type);

	llvm::Expected<MonoType *> binary_result (BinaryOp op, MonoType *lhs, MonoType *rhs);
	llvm::Expected<BinaryOperands> pop_binary_operands (BinaryOp op);

	llvm::Error emit_arg_allocas (MonoIrBuilder &builder);
	llvm::Error emit_local_allocas (MonoIrBuilder &builder);

	llvm::Error emit_instruction (MonoIrBuilder &builder);
	llvm::Error emit_ret (MonoIrBuilder &builder);

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

	llvm::Error emit_and (MonoIrBuilder &builder);
	llvm::Error emit_or (MonoIrBuilder &builder);
	llvm::Error emit_xor (MonoIrBuilder &builder);
	llvm::Error emit_not (MonoIrBuilder &builder);
	llvm::Error emit_shift (MonoIrBuilder &builder, BinaryOp op);

	llvm::Error check_conversion (ConvType type, MonoType *source);
	llvm::Value *emit_checked_int_conv (MonoIrBuilder &builder, llvm::Value *value,
	                                    ConvType type, bool source_unsigned);
	llvm::Value *emit_checked_float_conv (MonoIrBuilder &builder, llvm::Value *value,
	                                      ConvType type);

	llvm::Error emit_conv (MonoIrBuilder &builder, ConvType type);
	llvm::Error emit_conv_ovf (MonoIrBuilder &builder, ConvType type, bool source_unsigned);
	llvm::Error emit_conv_r_un (MonoIrBuilder &builder);

	llvm::Error emit_ldc_i4 (MonoIrBuilder &builder, int32_t value);
	llvm::Error emit_ldc_i8 (MonoIrBuilder &builder, int64_t value);
	llvm::Error emit_ldc_r4 (MonoIrBuilder &builder, uint32_t bits);
	llvm::Error emit_ldc_r8 (MonoIrBuilder &builder, uint64_t bits);

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

	/// The next eight bytes, little-endian.
	llvm::Expected<uint64_t> read_u64 ()
	{
		if (code_size - ip < 8)
			return truncated_il (8);

		uint64_t value = 0;

		for (size_t i = 0; i < 8; ++i)
			value |= static_cast<uint64_t> (code[ip + i]) << (8 * i);

		ip += 8;
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

/**
 * \file
 * \brief Conversion between CIL and LLVM IR.
 *
 * This file implements the lowering of CIL to LLVM IR. The implementation
 * details are within the MethodLLVMEmitter class, which is implemented across
 * a number of different source files in the method-to-llvm directory.
 */

#ifndef MONO_LLVM_METHOD_TO_LLVM_HPP
#define MONO_LLVM_METHOD_TO_LLVM_HPP

#include "arch/arch.hpp"

#include "mini.h"
#include "mono/metadata/metadata.h"
#include "mono/metadata/object-forward.h"
#include <llvm/ADT/STLFunctionalExtras.h>
#include <llvm/ADT/Twine.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Value.h>

// This breaks some LLVM headers
#undef PIC

#include <llvm/ADT/DenseMap.h>
#include <llvm/Support/Error.h>
#include <llvm/IR/Module.h>

#include <memory>
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

	Beq,
	Bge,
	Bgt,
	Ble,
	Blt,
	BneUn,
	BgeUn,
	BgtUn,
	BleUn,
	BltUn,

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

/// A symbol the emitted module leaves for the engine to resolve, and the runtime
/// object behind it.
///
/// The names are built out of metadata (a class's full name, a method's signature),
/// and taking one apart again to find what it was built from is neither cheap nor
/// reliable. So the translator says what it meant as it goes, and the engine looks
/// nothing up by name.
struct ExternalSymbol {
	enum class Kind {
		Class,   ///< `object` is the MonoClass this names
		VTable,  ///< the MonoVTable of the MonoClass in `object`
		Statics, ///< the static field block of the MonoClass in `object`
		Method,  ///< `object` is the MonoMethod this names
		Field,   ///< `object` is the MonoClassField this names
		Code,    ///< the entry point of the MonoMethod in `object`
		Address, ///< `object` is the address itself, which the name stands for
	};

	std::string name;
	Kind kind;
	void *object;
};

class MethodLLVMEmitter {
private:
	struct Entry {
		llvm::Value *alloca;
		MonoType *type;
		/// Whether the slot holds the marshalled layout rather than the managed
		/// one, which an argument of a pinvoke signature does.
		bool native = false;
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

	/// One evaluation-stack slot, held in memory so that it survives a branch.
	struct Slot {
		llvm::AllocaInst *alloca;
		MonoType *type;
	};

	/*
	 * A block the IL branches to, and the evaluation stack it is entered holding.
	 *
	 * Values that are live across a branch go through memory rather than through phis
	 * this translator builds itself: the arguments and the locals already work that
	 * way, so mem2reg is already what turns this function's stores into SSA, and
	 * spilling needs to know only how deep the stack is at a join - never what is on
	 * it. Building phis directly would need the types up front, which is a whole
	 * dataflow pass over the method before a single instruction could be emitted.
	 */
	struct Block {
		llvm::BasicBlock *block = nullptr;
		std::vector<Slot> entry;
		bool entry_known = false;
		/// Whether control can actually get here. A block nothing reaches has
		/// no entry stack, so its body is never translated.
		bool reachable = false;
	};

	/// Where control can go from a single instruction: the offset just past it, and
	/// every branch target it names.
	struct Flow {
		MonoOpcodeEnum opcode = MonoOpcodeEnum_Invalid;
		size_t next = 0;
		llvm::SmallVector<size_t, 4> targets;

		/// Whether control can carry on into the instruction after this one.
		bool falls_through () const;
	};

	llvm::Module *module;
	llvm::Function *function;
	llvm::IRBuilder<> builder;

	MonoCompile *cfg;
	MonoMethod *method;

	/// Where the symbols this method leaves unresolved are reported, or null when
	/// nothing is collecting them.
	std::vector<ExternalSymbol> *externals = nullptr;

	llvm::DenseMap<MonoMethod *, llvm::Function *> declarations;
	llvm::DenseMap<MonoClass *, llvm::Type *> vtypes;
	/// The same classes in the layout marshalling gives them, which is a
	/// different struct whenever it moves a field or changes its width.
	llvm::DenseMap<MonoClass *, llvm::Type *> native_vtypes;
	/*
	 * What an exception clause needs on the LLVM side.
	 *
	 * A finally block is entered from several places and has to carry on differently
	 * for each, so which one is in progress is written to `resume_at` before it is
	 * entered and switched on by its endfinally. Index 0 means the block was entered
	 * by unwinding, where carrying on means resuming the unwind rather than going
	 * anywhere in this method.
	 *
	 * A handler may have more than one endfinally, and any of them can be the one
	 * reached, so each gets its own switch and they all get the same set of cases.
	 */
	struct Clause {
		llvm::BasicBlock *pad = nullptr;
		llvm::AllocaInst *resume_at = nullptr;
		/*
		 * The byte another thread's abort request flags a running finally through,
		 * so that the abort lands after the handler instead of inside it. Written
		 * from outside this thread, hence read volatile. See
		 * emit_finally_abort_check.
		 */
		llvm::AllocaInst *abort_guard = nullptr;
		std::vector<llvm::SwitchInst *> resume;
		std::vector<std::pair<uint32_t, llvm::BasicBlock *>> continuations;
		/*
		 * The exception a catch or filter handler was entered with, as loaded at
		 * the handler's entry. rethrow reaches for it long after the body has
		 * taken the stack apart, and a handler is only enterable at its start, so
		 * the entry value dominates every use.
		 */
		llvm::Value *caught = nullptr;
	};

	MonoExceptionClause *clauses = nullptr;
	uint32_t num_clauses = 0;
	uint32_t next_continuation = 1;
	std::vector<Clause> clause_state;

	llvm::DenseMap<size_t, Block> blocks;
	llvm::DenseMap<std::pair<size_t, llvm::Type *>, llvm::AllocaInst *> spills;
	llvm::BasicBlock *entry_block = nullptr;
	std::vector<Entry> args;
	std::vector<Entry> locals;
	std::vector<StackValue> stack;

	/// This frame's LMF and where the thread's chain head lives, when the
	/// method keeps one (a save_lmf wrapper); null everywhere else.
	llvm::Value *lmf_slot = nullptr;
	llvm::Value *lmf_addr = nullptr;

	/// The prefixes seen since the last real instruction. They apply to the next
	/// instruction only and are cleared once it has been emitted, whether or not it
	/// was one the prefix means anything to.
	struct Prefixes {
		bool volatile_ = false;
		bool readonly_ = false;
		bool tail = false;
		uint8_t unaligned = 0;
		uint32_t constrained = 0;
	};

	Prefixes prefixes;

	/// Set by mono_save_last_error, consumed by the next call emitted: unlike a
	/// prefix an address push may sit between the two.
	bool pending_save_last_error = false;

	/// Translating a filter body into a function of its own: locals and
	/// arguments resolve into the parent frame through llvm.localrecover, and
	/// nothing in the range is protected.
	bool filter_mode = false;

	/// A vararg method's trailing parameter: the buffer holding the call-site
	/// signature and the variable arguments, which is what arglist pushes.
	llvm::Value *sig_cookie = nullptr;

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
	MethodLLVMEmitter (llvm::Module *module, MonoCompile *cfg, MonoMethod *method,
	                   std::vector<ExternalSymbol> *externals = nullptr)
	    : module (module),
	      function (nullptr),
	      builder (module->getContext ()),
	      cfg (cfg),
	      method (method),
	      externals (externals)
	{
	}

	llvm::Expected<llvm::Function *> emit ();
	llvm::Expected<llvm::Function *> emit_filter (llvm::Function *parent,
	                                              uint32_t clause_index);

	/// The declaration of METHOD in this emitter's module, for callers outside
	/// the translation itself (the runtime builds interop thunks against it).
	llvm::Expected<llvm::Function *> declare (MonoMethod *method)
	{
		return create_method_decl (method);
	}

private:
	typedef llvm::IRBuilder<> MonoIrBuilder;

	llvm::LLVMContext &context () const {
		return module->getContext ();
	}

	llvm::Expected<llvm::Function *> create_method_decl (MonoMethod *method);
	llvm::Expected<llvm::Function *> icall_wrapper_decl (MonoJitICallId id);
	std::vector<llvm::Value *> adapt_to_callee (MonoIrBuilder &builder,
	                                            llvm::Function *callee,
	                                            llvm::ArrayRef<llvm::Value *> args);
	llvm::Expected<llvm::FunctionType *> convert_method_signature (MonoMethodSignature *sig,
	                                                               bool native = false);
	static void mark_legacy_call (llvm::CallBase *call, MonoMethodSignature *sig);
	static void mark_legacy_entry_call (llvm::CallBase *call, MonoMethod *method,
	                                    MonoMethodSignature *sig);

	llvm::Expected<llvm::Type *> convert_type (MonoType *t, bool native = false);
	llvm::Expected<llvm::Type *> convert_vtype (MonoType *t, bool native = false);
	llvm::Expected<llvm::Type *> convert_native_vtype (MonoClass *klass);
	llvm::Expected<llvm::Type *> native_field_type (MonoType *t, MonoMarshalSpec *mspec,
	                                                int size);
	/// Whether this method is itself the native face of something - a
	/// native-to-managed wrapper - so that its arguments arrive marshalled and
	/// its return value has to leave the same way.
	bool native_signature () const;

	llvm::Align type_alignment (MonoType *t, bool native = false);

	/// Whether this body was generated by the runtime rather than loaded from
	/// metadata, which changes what its operands mean - see wrapper_data ().
	bool in_wrapper () const;

	/// Whether INDEX names a slot the wrapper filled in - which says nothing
	/// about what the slot holds, since a wrapper may bake in a null.
	bool has_wrapper_data (uint32_t index) const;

	/// What a generated body's operand refers to.
	///
	/// A wrapper's IL carries indices into a table the runtime filled in while
	/// building it, not metadata tokens: there is no metadata to point at.
	/// Returns null if INDEX is not one the wrapper filled in - so a caller
	/// that would accept a null has to ask has_wrapper_data () instead.
	void *wrapper_data (uint32_t index) const;

	llvm::Error invalid_il (const llvm::Twine &reason);
	llvm::Error unbalanced_stack (size_t needed);
	llvm::Error invalid_local (uint32_t index);
	llvm::Error invalid_argument (uint32_t index);
	llvm::Error truncated_il (size_t needed);
	llvm::Error unsupported_il (const llvm::Twine &what);
	llvm::Error emit_bad_image_call (MonoIrBuilder &builder, MonoMethodSignature *sig);

	/// Whether the CLI's accessibility rules bind what this body may reach.
	bool checks_accessibility () const;
	llvm::Error field_access_failure (MonoClassField *field);
	llvm::Error emit_method_access_failure (MonoIrBuilder &builder, MonoMethod *callee);

	static StackType stack_type (MonoType *t);
	static std::string describe (MonoType *t, StackType type);
	static llvm::Value *coerce (MonoIrBuilder &builder, llvm::Value *value,
	                            llvm::Type *type);
	static llvm::Value *widen_to_stack (MonoIrBuilder &builder, llvm::Value *value,
	                                    MonoType *t);
	static MonoType *stack_slot_type (MonoType *t);

	llvm::Expected<MonoType *> binary_result (BinaryOp op, MonoType *lhs, MonoType *rhs);
	llvm::Expected<BinaryOperands> pop_binary_operands (BinaryOp op);

	llvm::Error emit_arg_allocas (MonoIrBuilder &builder);
	llvm::Error emit_local_allocas (MonoIrBuilder &builder);
	llvm::Error emit_push_lmf (MonoIrBuilder &builder);
	void emit_pop_lmf (MonoIrBuilder &builder);

	llvm::Error emit_instruction (MonoIrBuilder &builder);
	llvm::Error emit_prefix (int opcode, uint64_t operand);
	llvm::Align access_alignment (MonoType *location);
	llvm::Value *emit_memory_load (MonoIrBuilder &builder, llvm::Type *type,
	                               llvm::Value *address, MonoType *location);
	void emit_memory_store (MonoIrBuilder &builder, llvm::Value *value,
	                        llvm::Value *address, MonoType *location);

	llvm::Expected<Flow> decode_flow (size_t at);
	llvm::Error find_block_leaders ();
	void mark_reachable_blocks ();
	llvm::Expected<size_t> branch_target (int32_t displacement);
	llvm::Error translate_range (MonoIrBuilder &builder, size_t begin, size_t end);
	void finish_function ();
	llvm::AllocaInst *spill_slot (size_t depth, llvm::Type *type);
	std::vector<Slot> spill_stack (MonoIrBuilder &builder);
	llvm::Error enter_block (MonoIrBuilder &builder, size_t target,
	                         const std::vector<Slot> &slots);
	void reload_stack (MonoIrBuilder &builder, const Block &block);

	int innermost_try (size_t at) const;
	int innermost_handler (size_t at) const;
	std::vector<uint32_t> covering_chain (uint32_t clause) const;
	llvm::Constant *clause_marker (uint32_t clause);
	llvm::Constant *resume_marker (uint32_t clause);
	llvm::BasicBlock *handler_entry (uint32_t clause, llvm::Value *exc);
	llvm::BasicBlock *landing_pad (uint32_t clause);
	void emit_resume_exit (MonoIrBuilder &builder, uint32_t clause);
	void emit_unwinding_call (MonoIrBuilder &builder, llvm::FunctionCallee callee,
	                          llvm::ArrayRef<llvm::Value *> args);
	void enter_finally (MonoIrBuilder &builder, uint32_t clause, uint32_t continuation);
	void emit_finally_body_marker (MonoIrBuilder &builder, uint32_t clause, bool opening);
	llvm::Error emit_finally_abort_check (MonoIrBuilder &builder, uint32_t clause,
	                                      llvm::Value *which);
	void resolve_finally_switches ();

	llvm::Error emit_ret (MonoIrBuilder &builder);
	llvm::Error emit_leave (MonoIrBuilder &builder, int32_t displacement);
	llvm::Error emit_endfinally (MonoIrBuilder &builder);
	llvm::Error emit_endfilter (MonoIrBuilder &builder);
	llvm::Error emit_throw (MonoIrBuilder &builder);
	llvm::Error emit_rethrow (MonoIrBuilder &builder);
	llvm::Error emit_br (MonoIrBuilder &builder, int32_t displacement);
	llvm::Error emit_brcond (MonoIrBuilder &builder, int32_t displacement, bool branch_if_true);
	llvm::Expected<llvm::Value *> emit_comparison (MonoIrBuilder &builder, BinaryOp op);
	llvm::Error emit_branch_compare (MonoIrBuilder &builder, BinaryOp op,
	                                 int32_t displacement);
	llvm::Error emit_compare (MonoIrBuilder &builder, BinaryOp op);
	llvm::Error emit_switch (MonoIrBuilder &builder);

	void emit_throw_corlib_exception (MonoIrBuilder &builder, const char *name);
	llvm::BranchInst *emit_cond_exception (MonoIrBuilder &builder, llvm::Value *condition,
	                                       const char *name);
	void emit_null_check (MonoIrBuilder &builder, llvm::Value *pointer);

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
	llvm::Error emit_neg (MonoIrBuilder &builder);

	llvm::Error emit_add_ovf (MonoIrBuilder &builder, bool is_unsigned);
	llvm::Error emit_mul_ovf (MonoIrBuilder &builder, bool is_unsigned);
	llvm::Error emit_sub_ovf (MonoIrBuilder &builder, bool is_unsigned);

	llvm::Expected<llvm::Value *> coerce_to_location (MonoIrBuilder &builder, StackValue value,
	                                                  MonoType *destination,
	                                                  bool native = false);
	llvm::Expected<llvm::Value *> coerce_to_argument (MonoIrBuilder &builder, StackValue value,
	                                                  MonoType *destination,
	                                                  bool native = false);

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
	llvm::Error emit_ldnull (MonoIrBuilder &builder);

	llvm::Error emit_dup ();
	llvm::Error emit_pop ();

	llvm::Value *emit_protected_call (MonoIrBuilder &builder, llvm::FunctionCallee callee,
	                                  llvm::ArrayRef<llvm::Value *> args);

	llvm::Expected<MonoMethod *> resolve_method (uint32_t token);
	llvm::Expected<MonoMethodSignature *> call_site_signature (MonoMethod *target,
	                                                           uint32_t token);
	llvm::Expected<llvm::Value *> build_sig_cookie (MonoIrBuilder &builder,
	                                                MonoMethodSignature *sig,
	                                                llvm::ArrayRef<llvm::Value *> args);
	llvm::Value *coerce_to_receiver (MonoIrBuilder &builder, llvm::Value *value);
	llvm::Expected<std::vector<llvm::Value *>>
	pop_call_arguments (MonoIrBuilder &builder, MonoMethodSignature *sig,
	                    bool native = false);
	llvm::Value *vtable_entry (MonoIrBuilder &builder, llvm::Value *receiver,
	                           int32_t offset);
	llvm::Value *virtual_callee (MonoIrBuilder &builder, llvm::Value *receiver,
	                             MonoMethod *target);
	llvm::Value *interface_callee (MonoIrBuilder &builder, llvm::Value *receiver,
	                               MonoMethod *target);
	llvm::Constant *method_symbol (MonoMethod *target);
	llvm::Expected<llvm::Constant *> code_address_symbol (MonoMethod *target);
	MonoMethod *synchronized_target (MonoMethod *target);
	bool is_own_this (llvm::Value *value);
	llvm::CallInst::TailCallKind should_tail_call (MonoMethodSignature *callee_sig,
	                                               MonoMethod *callee_method,
	                                               llvm::FunctionType *callee_type);
	bool matching_call_abi (MonoMethodSignature *callee_sig, llvm::FunctionType *callee_type);
	llvm::Error emit_jmp (MonoIrBuilder &builder, uint32_t token);
	llvm::Error emit_tail_call (MonoIrBuilder &builder, llvm::FunctionCallee callee,
	                            llvm::ArrayRef<llvm::Value *> args,
	                            llvm::CallInst::TailCallKind kind, size_t arg_slots,
	                            llvm::Function *declaration,
	                            llvm::function_ref<void (llvm::CallBase *)> describe_site);
	llvm::Error emit_call (MonoIrBuilder &builder, uint32_t token, bool is_virtual);
	llvm::Error emit_ldftn (MonoIrBuilder &builder, uint32_t token);
	llvm::Error emit_ldvirtftn (MonoIrBuilder &builder, uint32_t token);
	llvm::Error emit_calli (MonoIrBuilder &builder, uint32_t token);
	llvm::Error emit_dynamic_native_calli (MonoIrBuilder &builder,
	                                       MonoMethodSignature *sig);

	llvm::Value *spill_to_temporary (MonoIrBuilder &builder, MonoType *type);
	llvm::Error emit_arglist (MonoIrBuilder &builder);
	llvm::Error emit_mkrefany (MonoIrBuilder &builder, uint32_t token);
	llvm::Error emit_refanyval (MonoIrBuilder &builder, uint32_t token);
	llvm::Error emit_refanytype (MonoIrBuilder &builder);

	llvm::FunctionCallee wbarrier_decl ();
	llvm::Expected<MonoClassField *> resolve_field (uint32_t token, bool want_static,
	                                                bool *out_is_static = nullptr);
	llvm::Constant *extern_symbol (const std::string &name);
	void record_external (const std::string &name, ExternalSymbol::Kind kind,
	                      void *object);
	std::string identity_symbol (const std::string &name, const void *object);
	llvm::Constant *class_symbol (MonoClass *klass, const char *prefix);
	llvm::Constant *field_symbol (MonoClassField *field);
	llvm::Constant *address_symbol (const std::string &name, void *address);

	llvm::Error emit_mono_icall (MonoIrBuilder &builder, uint32_t id);
	llvm::Error emit_mono_objaddr (MonoIrBuilder &builder);
	llvm::Error emit_mono_vtaddr (MonoIrBuilder &builder);
	llvm::Error emit_mono_get_sp (MonoIrBuilder &builder);
	llvm::Error emit_mono_rethrow (MonoIrBuilder &builder);
	llvm::Error emit_mono_newobj (MonoIrBuilder &builder, uint32_t token);
	llvm::Error emit_mono_ldnativeobj (MonoIrBuilder &builder, uint32_t token);
	llvm::Error emit_mono_retobj (MonoIrBuilder &builder, uint32_t token);
	llvm::Error emit_mono_ldptr (MonoIrBuilder &builder, uint32_t token);
	llvm::Error emit_mono_lddomain (MonoIrBuilder &builder);
	llvm::Error emit_mono_classconst (MonoIrBuilder &builder, uint32_t token);
	llvm::Error emit_mono_methodconst (MonoIrBuilder &builder, uint32_t token);
	llvm::Error emit_mono_jit_icall_addr (MonoIrBuilder &builder, uint32_t id);
	llvm::Error emit_mono_icall_addr (MonoIrBuilder &builder, uint32_t token);
	llvm::Error emit_mono_tls (MonoIrBuilder &builder, uint32_t key);
	llvm::Error emit_mono_atomic_store_i4 (MonoIrBuilder &builder, uint32_t barrier);
	llvm::Error emit_mono_ld_delegate_method_ptr (MonoIrBuilder &builder);
	llvm::Error emit_mono_calli_extra_arg (MonoIrBuilder &builder, uint32_t token);
	llvm::Error emit_ldstr (MonoIrBuilder &builder, uint32_t token);
	llvm::Error emit_ldtoken (MonoIrBuilder &builder, uint32_t token);
	llvm::Error emit_class_init (MonoIrBuilder &builder, MonoClass *klass);
	llvm::Expected<llvm::Value *> static_field_address (MonoIrBuilder &builder,
	                                                    MonoClassField *field);
	static MonoType *builtin_element_type (int opcode);
	llvm::Expected<MonoType *> element_type_from_token (uint32_t token);
	llvm::Expected<llvm::Value *> array_length (MonoIrBuilder &builder, StackValue array);
	llvm::Expected<llvm::Value *> element_address (MonoIrBuilder &builder, StackValue array,
	                                               StackValue index, MonoType *element);
	llvm::Error emit_ldlen (MonoIrBuilder &builder);
	llvm::Error emit_newarr (MonoIrBuilder &builder, uint32_t token);
	llvm::Error emit_ldelema (MonoIrBuilder &builder, uint32_t token);
	llvm::Error emit_ldelem (MonoIrBuilder &builder, MonoType *element);
	llvm::Error emit_stelem (MonoIrBuilder &builder, MonoType *element);
	void emit_array_type_check (MonoIrBuilder &builder, llvm::Value *array,
	                            MonoClass *array_class);
	void consume_save_last_error (MonoIrBuilder &builder);
	llvm::Expected<llvm::Value *> array_accessor_address (MonoIrBuilder &builder,
	                                                      MonoClass *klass,
	                                                      llvm::Value *array,
	                                                      llvm::ArrayRef<llvm::Value *> indices);
	llvm::Error emit_array_accessor_call (MonoIrBuilder &builder, MonoMethod *accessor,
	                                      MonoMethodSignature *sig);
	llvm::Error emit_unsafe_mov (MonoIrBuilder &builder, MonoMethodSignature *sig);

	llvm::Expected<llvm::Value *> indirect_address (MonoIrBuilder &builder,
	                                                StackValue address);
	llvm::Error emit_ldind (MonoIrBuilder &builder, MonoType *element);
	llvm::Error emit_stind (MonoIrBuilder &builder, MonoType *element);
	llvm::FunctionCallee value_copy_decl ();
	llvm::Expected<llvm::Function *> object_new_decl ();
	llvm::Value *unbox_payload (MonoIrBuilder &builder, llvm::Value *obj,
	                            MonoClass *klass);
	llvm::Error call_nullable_helper (MonoIrBuilder &builder, MonoClass *klass,
	                                  const char *name);
	llvm::Expected<llvm::Value *> box_value (MonoIrBuilder &builder, MonoClass *klass, MonoType *type,
	                        llvm::Value *value);
	llvm::Error emit_newobj (MonoIrBuilder &builder, uint32_t token);
	llvm::Error emit_array_newobj (MonoIrBuilder &builder, MonoMethod *ctor,
	                               MonoMethodSignature *sig);
	llvm::Expected<llvm::Value *> emit_creator (MonoIrBuilder &builder, MonoMethod *ctor,
	                                            llvm::ArrayRef<llvm::Value *> args);
	llvm::Error emit_creator_call (MonoIrBuilder &builder, MonoMethod *ctor,
	                               MonoMethodSignature *sig);
	llvm::Error emit_box (MonoIrBuilder &builder, uint32_t token);
	llvm::Error emit_unbox (MonoIrBuilder &builder, uint32_t token);
	llvm::Error emit_unbox_any (MonoIrBuilder &builder, uint32_t token);
	llvm::Error emit_cast (MonoIrBuilder &builder, uint32_t token, bool throw_on_fail);
	llvm::Error emit_castclass (MonoIrBuilder &builder, uint32_t token);
	llvm::Error emit_isinst (MonoIrBuilder &builder, uint32_t token);
	llvm::Expected<llvm::Value *> block_size (MonoIrBuilder &builder, StackValue size);
	llvm::Error emit_cpblk (MonoIrBuilder &builder);
	llvm::Error emit_initblk (MonoIrBuilder &builder);
	llvm::Error emit_ldobj (MonoIrBuilder &builder, uint32_t token);
	llvm::Error emit_stobj (MonoIrBuilder &builder, uint32_t token);
	llvm::Error emit_cpobj (MonoIrBuilder &builder, uint32_t token);
	llvm::Error emit_initobj (MonoIrBuilder &builder, uint32_t token);

	llvm::Error emit_ldsfld (MonoIrBuilder &builder, uint32_t token);
	llvm::Error emit_ldsflda (MonoIrBuilder &builder, uint32_t token);
	llvm::Error emit_stsfld (MonoIrBuilder &builder, uint32_t token);
	llvm::Expected<llvm::Value *> field_address (MonoIrBuilder &builder, StackValue object,
	                                             MonoClassField *field,
	                                             bool null_check = true);
	bool remote_field_access (StackValue receiver, MonoClassField *field);
	void push_field_wrapper_operands (MonoIrBuilder &builder, MonoClassField *field);
	llvm::Error emit_ldfld (MonoIrBuilder &builder, uint32_t token);
	llvm::Error emit_ldflda (MonoIrBuilder &builder, uint32_t token);
	llvm::Error emit_stfld (MonoIrBuilder &builder, uint32_t token);

	llvm::Error emit_ldarg (MonoIrBuilder &builder, uint32_t index);
	llvm::Error emit_ldarga (MonoIrBuilder &builder, uint32_t index);
	llvm::Error emit_starg (MonoIrBuilder &builder, uint32_t index);

	llvm::Error emit_ldloc (MonoIrBuilder &builder, uint32_t index);
	llvm::Error emit_ldloca (MonoIrBuilder &builder, uint32_t index);
	llvm::Error emit_stloc (MonoIrBuilder &builder, uint32_t index);
	llvm::Error emit_localloc (MonoIrBuilder &builder);
	llvm::Error emit_sizeof (MonoIrBuilder &builder, uint32_t token);
	llvm::Error emit_ckfinite (MonoIrBuilder &builder);
	llvm::Error emit_break (MonoIrBuilder &builder);

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
/// How a narrow integer argument or return value is widened to fill its register,
/// as an SExt/ZExt attribute, or None for everything else.
llvm::Attribute::AttrKind integer_extension (MonoType *t);

/// Whether METHOD's code comes from somewhere other than IL - an icall, a
/// pinvoke, or a method the runtime implements itself - so what stands behind
/// its symbol is whatever mini compiles for it, never this backend's fastcc
/// code.
bool implemented_outside_il (MonoMethod *method);

/// The number of SIG's parameters that are ordinary ones, which for a vararg
/// signature means the fixed part ahead of the sentinel.
///
/// A vararg method's own signature carries its sentinel past the last
/// parameter, so a declaration and every call site that names it agree on this
/// count - which is what lets both convert to one function type.
int vararg_fixed_params (MonoMethodSignature *sig);

/// The legacy-boundary flavor of a call through SIG: native signatures keep
/// the C classification, managed ones mini's, with the hidden return pointer
/// behind the first argument whenever the runtime's trampolines insist on
/// finding a receiver there.
arch::LegacyFlavor legacy_call_flavor (MonoMethodSignature *sig);

/// The flavor of the code the runtime publishes for METHOD, whose signature is
/// SIG.
///
/// A method implemented outside IL declares a pinvoke signature - the loader
/// sets that flag for every icall and every DllImport - but the address behind
/// its symbol is almost never a C function: it is the marshaling wrapper the
/// runtime builds around it, a managed method that speaks mini's convention.
/// The one exception is an icall registered as needing no wrapper at all, whose
/// published address really is the C function. So the method, not its
/// signature, is what says which convention its entry speaks.
arch::LegacyFlavor legacy_entry_flavor (MonoMethod *method, MonoMethodSignature *sig);

/// EXTERNALS, when given, collects the symbols the emitted module leaves for the
/// engine to resolve.
llvm::Expected<llvm::Function *> method_to_llvm (llvm::Module *module, MonoCompile *cfg,
                                                 MonoMethod *method,
                                                 std::vector<ExternalSymbol> *externals
                                                 = nullptr);

} // namespace mono

#endif

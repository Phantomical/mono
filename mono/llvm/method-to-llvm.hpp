/**
 * \file
 * \brief Conversion between CIL and LLVM IR.
 *
 * MethodLLVMEmitter translates one method's IL into LLVM IR. Its
 * implementation spans multiple files in the method-to-llvm directory, split
 * by opcode family.
 */

#ifndef MONO_LLVM_METHOD_TO_LLVM_HPP
#define MONO_LLVM_METHOD_TO_LLVM_HPP

#include "arch/arch.hpp"

#include "il-line-table.hpp"
#include "method-symbols.hpp"
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
#include <llvm/ADT/DenseSet.h>
#include <llvm/Support/Error.h>
#include <llvm/IR/Module.h>

#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace mono {

/// A method the translator will not share between reference instantiations.
///
/// This is not a compile failure. The method is translated again against the
/// instantiation that was asked for, which is what every method got before
/// sharing existed, so a caller that gets one has a body to fall back on.
class SharingRefusal : public llvm::ErrorInfo<SharingRefusal> {
public:
	static char ID;

	explicit SharingRefusal (std::string what) : what_ (std::move (what)) {}

	void log (llvm::raw_ostream &os) const override { os << what_; }
	std::error_code convertToErrorCode () const override
	{
		return llvm::inconvertibleErrorCode ();
	}

private:
	std::string what_;
};

/// For each sequence point a body emitted, the sequence points control can
/// reach from it without passing another one - all as IL offsets. The soft
/// debugger single-steps by breakpointing a point's successors, so this is the
/// graph it steps over.
using SeqPointGraph = std::map<uint32_t, std::vector<uint32_t>>;

/// CIL instructions that take two operands from a table in ECMA-335 III.1.5.
/// Table III.2 covers binary numeric operations, Table III.5 integer
/// operations, Table III.6 shift operations, and Table III.7 overflow
/// arithmetic.
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

/// The six types the CLI tracks on the evaluation stack, listed in ECMA-335
/// III.1.5. A seventh, Invalid, covers everything that cannot appear as an
/// operand of one.
///
/// This is the axis every operand table in III.1.5 indexes by, so the
/// arithmetic and conversion tables are both laid out along it.
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

/// A symbol the emitted module leaves for the engine to resolve, and the
/// runtime object behind it.
///
/// The names are built from metadata, such as a class's full name or a
/// method's signature. It is neither cheap nor reliable to parse a name back
/// into what built it, so the translator states what it meant as it goes. The
/// engine never looks anything up by name.
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

/// The named struct types a module already holds, by class.
///
/// Every emitter writing into one module has to share this. LLVM uniques a
/// struct name within a module, so a second emitter that builds its own
/// `System.RuntimeTypeHandle` gets `System.RuntimeTypeHandle.0` instead - a type
/// of its own. A call it then makes to a declaration the first emitter typed
/// passes an argument of the wrong type.
struct ModuleTypes {
	llvm::DenseMap<MonoClass *, llvm::Type *> vtypes;
	/// The same classes in the layout marshalling gives them, which is a
	/// different struct whenever it moves a field or changes its width.
	llvm::DenseMap<MonoClass *, llvm::Type *> native_vtypes;
};

/// What a call to a System.Math or System.MathF method compiles to in place of
/// the call.
struct MathIntrinsic {
	enum class Emit {
		/// A call to the intrinsic in `intrinsic`.
		Intrinsic,
		/// The frem instruction.
		Remainder,
		/// A call to the libm function named in `libm`.
		Libm,
		/// llvm.modf, whose integral half is stored through the call's last
		/// argument and whose fractional half is the result.
		Modf,
	};

	Emit emit;
	/// The intrinsic to call. Meaningful only for Emit::Intrinsic.
	llvm::Intrinsic::ID intrinsic;
	/// The libm function to call. Meaningful only for Emit::Libm.
	const char *libm;
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
	/// LLVM's own type does not say what an instruction can do with a value.
	/// An i64 covers both int64 and native int, and a pointer covers both a
	/// managed pointer and an object reference. Those are the distinctions
	/// the operand tables in ECMA-335 III.1.5 turn on.
	///
	/// A value class is not here at all. `value` is the address of the frame
	/// slot that holds it - see held_in_memory (). `native` says that slot is
	/// in the marshalled layout rather than the managed one, which the
	/// MonoType alone cannot say.
	struct StackValue {
		llvm::Value *value;
		MonoType *type;
		bool native = false;
	};

	struct BinaryOperands {
		StackValue value1;
		StackValue value2;
		MonoType *result;
	};

	/// One evaluation-stack slot, held in memory so that it survives a branch.
	struct Slot {
		llvm::AllocaInst *alloca;
		MonoType *type;
		bool native = false;
	};

	/// A block the IL branches to, and the evaluation stack it is entered holding.
	///
	/// Values that are live across a branch go through memory, not through phis
	/// this translator builds itself. The arguments and the locals already work
	/// this way, so mem2reg already turns this function's stores into SSA.
	/// Spilling needs to know only how deep the stack is at a join, never what
	/// value sits on it. Building phis directly needs the types up front. That
	/// needs a whole dataflow pass over the method before the translator emits
	/// a single instruction.
	struct Block {
		llvm::BasicBlock *block = nullptr;
		std::vector<Slot> entry;
		bool entry_known = false;
		/// A block nothing reaches has no entry stack, so its body is never
		/// translated.
		bool reachable = false;
	};

	/// Where control can go from a single instruction: the offset immediately
	/// past it, and every branch target it names.
	struct Flow {
		MonoOpcodeEnum opcode = MonoOpcodeEnum_Invalid;
		size_t next = 0;
		llvm::SmallVector<size_t, 4> targets;

		bool falls_through () const;
	};

	llvm::Module *module;
	llvm::Function *function;
	llvm::IRBuilder<> builder;

	MonoCompile *cfg;
	MonoMethod *method;

	/// Where this emitter reports the symbols it leaves unresolved, or null
	/// when nothing collects them.
	std::vector<ExternalSymbol> *externals = nullptr;

	llvm::DenseMap<MonoMethod *, llvm::Function *> declarations;

	ModuleTypes own_types;
	ModuleTypes &types;
	/// What an exception clause needs on the LLVM side.
	///
	/// A finally block is entered from several places and must carry on
	/// differently for each. Which one is in progress is written to
	/// `resume_at` before entry, and each endfinally in the block reads it
	/// back through its own switch. Index 0 means the block was entered by
	/// unwinding. There, the finally must resume the unwind rather than jump
	/// anywhere else in this method.
	///
	/// A handler can have more than one endfinally, and any of them can be
	/// the one reached. So each gets its own switch, and every switch gets
	/// the same set of cases.
	struct Clause {
		llvm::BasicBlock *pad = nullptr;
		llvm::AllocaInst *resume_at = nullptr;
		/// The byte another thread's abort request flags a running finally through,
		/// so that the abort lands after the handler instead of inside it. Written
		/// from outside this thread, hence read volatile. See
		/// emit_finally_abort_check.
		llvm::AllocaInst *abort_guard = nullptr;
		std::vector<llvm::SwitchInst *> resume;
		std::vector<std::pair<uint32_t, llvm::BasicBlock *>> continuations;
		/// The exception a catch or filter handler was entered with, as loaded at
		/// the handler's entry. rethrow reaches for it long after the body has
		/// taken the stack apart. A handler is only enterable at its start, so the
		/// entry value dominates every use.
		llvm::Value *caught = nullptr;
	};

	MonoExceptionClause *clauses = nullptr;
	uint32_t num_clauses = 0;
	uint32_t next_continuation = 1;
	std::vector<Clause> clause_state;

	llvm::DenseMap<size_t, Block> blocks;
	llvm::DenseMap<std::pair<size_t, llvm::Type *>, llvm::AllocaInst *> spills;
	llvm::BasicBlock *entry_block = nullptr;

	std::vector<llvm::BasicBlock *> cold_blocks;

	std::vector<Entry> args;
	std::vector<Entry> locals;
	std::vector<StackValue> stack;

	/// The objects newobj and newarr allocated in this body, so their class is
	/// exactly the one their stack type names.
	///
	/// A value that reached its use through a spill is not in here. That costs
	/// a devirtualization or an array store's covariance test, and never
	/// correctness.
	llvm::DenseSet<llvm::Value *> allocated_here;

	/// This frame's LMF and where the thread's chain head lives.
	///
	/// The method keeps these only when it is a save_lmf wrapper. Both are
	/// null everywhere else.
	llvm::Value *lmf_slot = nullptr;
	llvm::Value *lmf_addr = nullptr;

	/// Which of the method's type parameters its body depends on, as
	/// MONO_GENERIC_CONTEXT_USED_*. Zero for a body compiled against one
	/// instantiation, which is every body that is not a shared one.
	int context_used = 0;

	/// The runtime generic context this shared body was entered with, read in
	/// the prologue. Null in a body that is not a shared one.
	llvm::Value *rgctx = nullptr;

	/// Whether this body pinned to a frame slot what it was entered with, so a
	/// stack walk can say which instantiation the frame is running as.
	bool pinned_receiver = false;

	/// What stopped this body from being shared, empty while nothing has.
	///
	/// A refusal is recorded rather than raised so that a translation that
	/// cannot be shared still names the first thing that stopped it. The
	/// engine compiles the concrete method instead.
	std::string sharing_refusal;

	/// The prefixes seen since the last real instruction.
	///
	/// They apply only to the next instruction. The translator clears them
	/// once it emits that instruction, whether or not the prefix means
	/// anything to it.
	struct Prefixes {
		bool volatile_ = false;
		bool readonly_ = false;
		bool tail = false;
		uint8_t unaligned = 0;
		uint32_t constrained = 0;
	};

	Prefixes prefixes;

	/// Set by mono_save_last_error, consumed by the next call emitted:
	/// unlike a prefix, an address push can sit between the two.
	bool pending_save_last_error = false;

	/// Whether this emitter translates a filter body as a function of
	/// its own.
	///
	/// Locals and arguments resolve into the parent frame through
	/// llvm.localrecover, and nothing in the range is protected.
	bool filter_mode = false;

	/// Which of enter, leave and tail call a profiler asked to be told
	/// about for this method. MONO_PROFILER_CALL_INSTRUMENTATION_NONE, the
	/// default when no profiler is attached, keeps the instrumentation out
	/// of every ordinary compile.
	MonoProfilerCallInstrumentationFlags prof_flags =
		MONO_PROFILER_CALL_INSTRUMENTATION_NONE;

	/// A vararg method's trailing parameter: the buffer holding the call-site
	/// signature and the variable arguments, which is what arglist pushes.
	llvm::Value *sig_cookie = nullptr;

	/// This body's soft-debugger breakpoint switch, allocated on the first
	/// sequence point emitted and null when the method got none. The runtime
	/// hangs it off the jit info so that mono_arch_set_breakpoint () can arm
	/// it.
	MonoLLVMBreakpointSwitch *bp_switch = nullptr;

	/// The IL offsets this body emitted a sequence point at, in code order.
	/// This list leaves out the two synthetic ones, method entry and exit.
	/// Neither is a place the IL can branch to or from, and the debugger's
	/// stepper leaves them out of its graph for the same reason.
	std::vector<uint32_t> seq_point_offsets;

	/// seq_point_offsets as a graph, built once the body has been translated.
	SeqPointGraph seq_point_graph;

	/// The offset of the statement being translated - the most recent one
	/// wants_seq_point_at () accepted. A sequence point marker carries an offset
	/// of its own, so the code after one goes back to this.
	size_t statement_offset = 0;

	/// The marker of the sequence point emitted after the most recent call, and
	/// the IL offset it stands for.
	///
	/// The translator tags every call in an argument list except the outermost
	/// as a nested call. It only learns which call is outermost once the next
	/// call appears, so it applies the tag retroactively, through this marker.
	llvm::Instruction *call_seq_point_marker = nullptr;
	uint32_t call_seq_point_offset = 0;

	bool call_seq_point_run = false;

	/// The IL offsets the symbol file names as sequence points, and whether it
	/// had anything to say about this method at all. When it did, these are the
	/// only places an ordinary sequence point goes. A stop that is not the start
	/// of a statement reports the same source line twice.
	llvm::DenseSet<uint32_t> sym_seq_point_offsets;
	bool sym_seq_points = false;

	/// The method's IL, the offset of the instruction being emitted, and how far into
	/// that instruction its operands have been read.
	///
	/// `offset` stays at the start of the instruction while `ip` walks its operands.
	/// A refusal then names the instruction that caused it, not the one after it.
	const unsigned char *code = nullptr;
	size_t code_size = 0;
	size_t offset = 0;
	size_t ip = 0;

	/// The compile's debug info, and this function's scope within it. The map a
	/// stack trace reads back is built from the locations these stamp on the
	/// emitted instructions. Both are null when the caller wants none.
	IlDebugModule *il_debug = nullptr;
	IlDebugScope *il_scope = nullptr;

	/// The other methods this module defines, when several are translated into
	/// one. A call to one of them has to reach its published entry rather than
	/// the body beside it, so create_method_decl () declares it under a name of
	/// its own.
	llvm::ArrayRef<MonoMethod *> siblings;

	/// The suffix on the name of the function being emitted, empty when it takes
	/// the plain one. A module holds one body under a method's own name, so a
	/// copy folded into a caller beside it needs a name of its own.
	std::string body_suffix;

	void set_il_location (llvm::IRBuilder<> &builder, size_t offset)
	{
		il_debug_set_location (il_scope, &builder, (uint32_t) offset);
	}

	bool is_handler_start (size_t offset) const
	{
		for (uint32_t i = 0; i < num_clauses; ++i)
			if (clauses[i].handler_offset == offset
			    || (clauses[i].flags == MONO_EXCEPTION_CLAUSE_FILTER
			        && clauses[i].data.filter_offset == offset))
				return true;

		return false;
	}

public:
	/// shared_types is the struct-type cache of the module. Emitters that write
	/// into one module must be given one and the same cache. The emitter keeps
	/// its own only when it has the module to itself.
	MethodLLVMEmitter (llvm::Module *module, MonoCompile *cfg, MonoMethod *method,
	                   std::vector<ExternalSymbol> *externals = nullptr,
	                   IlDebugModule *il_debug = nullptr,
	                   llvm::ArrayRef<MonoMethod *> siblings = {},
	                   ModuleTypes *shared_types = nullptr,
	                   llvm::StringRef body_suffix = {})
	    : module (module),
	      function (nullptr),
	      builder (module->getContext ()),
	      cfg (cfg),
	      method (method),
	      externals (externals),
	      types (shared_types != nullptr ? *shared_types : own_types),
	      il_debug (il_debug),
	      siblings (siblings),
	      body_suffix (body_suffix)
	{
	}

	llvm::Expected<llvm::Function *> emit ();
	llvm::Expected<llvm::Function *> emit_filter (llvm::Function *parent,
	                                              uint32_t clause_index);

	/// The declaration of method in this emitter's module, for callers outside
	/// the translation itself. The runtime builds interop thunks against it.
	llvm::Expected<llvm::Function *> declare (MonoMethod *method)
	{
		return create_method_decl (method);
	}

	/// The breakpoint switch this body's sequence points call through, or null
	/// when it was translated without any.
	MonoLLVMBreakpointSwitch *breakpoint_switch () const { return bp_switch; }

	/// Which sequence points can execute next after each of this body's, empty
	/// when it was translated without any.
	const SeqPointGraph &sequence_points () const { return seq_point_graph; }

private:
	typedef llvm::IRBuilder<> MonoIrBuilder;

	llvm::LLVMContext &context () const {
		return module->getContext ();
	}

	llvm::Expected<llvm::Function *> create_method_decl (MonoMethod *method,
	                                                     bool by_context = false);
	llvm::Expected<llvm::Function *> icall_wrapper_decl (MonoJitICallId id);
	std::vector<llvm::Value *> adapt_to_callee (MonoIrBuilder &builder,
	                                            llvm::Function *callee,
	                                            llvm::ArrayRef<llvm::Value *> args);
	llvm::Expected<llvm::FunctionType *> convert_method_signature (MonoMethodSignature *sig,
	                                                               bool native = false);
	static void mark_mono_call (llvm::CallBase *call);

	llvm::Expected<llvm::Type *> convert_type (MonoType *t, bool native = false);
	llvm::Expected<llvm::Type *> convert_vtype (MonoType *t, bool native = false);
	llvm::Expected<llvm::Type *> convert_native_vtype (MonoClass *klass);
	llvm::Expected<llvm::Type *> native_field_type (MonoType *t, MonoMarshalSpec *mspec,
	                                                int size);
	/// Whether this method is itself a native-to-managed wrapper, so its
	/// arguments arrive marshalled and its return value must leave the same
	/// way.
	bool native_signature () const;

	llvm::Align type_alignment (MonoType *t, bool native = false);

	/// Whether this body was generated by the runtime rather than loaded from
	/// metadata, which changes what its operands mean - see wrapper_data ().
	bool in_wrapper () const;

	/// Whether index names a slot the wrapper filled in - which says nothing
	/// about what the slot holds, since a wrapper can bake in a null.
	bool has_wrapper_data (uint32_t index) const;

	/// What a generated body's operand refers to.
	///
	/// A wrapper's IL carries indices into a table the runtime filled in while
	/// building it, not metadata tokens: there is no metadata to point at.
	/// Returns null if index is not one the wrapper filled in, so a caller
	/// that accepts a null must ask has_wrapper_data () instead.
	void *wrapper_data (uint32_t index) const;

	llvm::Error invalid_il (const llvm::Twine &reason);
	llvm::Error unbalanced_stack (size_t needed);
	llvm::Error invalid_local (uint32_t index);
	llvm::Error invalid_argument (uint32_t index);
	llvm::Error truncated_il (size_t needed);
	llvm::Error unsupported_il (const llvm::Twine &what);
	llvm::Error emit_bad_image_call (MonoIrBuilder &builder, MonoMethodSignature *sig);

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

	/// Whether a value of type t rides the evaluation stack as the address of
	/// its storage rather than as an SSA value.
	bool held_in_memory (MonoType *t);

	llvm::Expected<llvm::Value *> vtype_slot (MonoType *t, bool native = false);

	unsigned vtype_size (MonoType *t, bool native);

	void copy_vtype (MonoIrBuilder &builder, llvm::Value *destination,
	                 llvm::Value *source, MonoType *t, bool native);

	/// Push what a location of type t holds at address, as the CLI tracks it.
	llvm::Error push_from_location (MonoIrBuilder &builder, llvm::Value *address,
	                                MonoType *t, bool native = false);

	/// Push value, which was produced in t's own LLVM type, as the CLI tracks it.
	llvm::Error push_produced (MonoIrBuilder &builder, llvm::Value *value, MonoType *t,
	                           bool native = false);

	/// What coerce_to_location produced for a location of type t, as an SSA value
	/// of that location's own LLVM type.
	llvm::Expected<llvm::Value *> materialize (MonoIrBuilder &builder, llvm::Value *value,
	                                           MonoType *t, bool native = false);

	llvm::Expected<MonoType *> binary_result (BinaryOp op, MonoType *lhs, MonoType *rhs);
	llvm::Expected<BinaryOperands> pop_binary_operands (BinaryOp op);

	/// Splits an add's operands into the managed pointer and the integer to
	/// index it by. Either side of the add can be the pointer.
	///
	/// A managed pointer plus an integer stays a managed pointer, so an add
	/// indexes the pointer rather than doing the arithmetic on it. That keeps
	/// the result something the collector still recognizes as pointing into its
	/// object.
	static std::pair<llvm::Value *, llvm::Value *>
	pointer_and_index (const BinaryOperands &operands);

	llvm::Error emit_arg_allocas (MonoIrBuilder &builder);
	llvm::Error emit_local_allocas (MonoIrBuilder &builder);
	llvm::Error emit_push_lmf (MonoIrBuilder &builder);
	void emit_pop_lmf (MonoIrBuilder &builder);

	bool debug_var_slots_wanted () const;
	bool emit_debug_var_marker (MonoIrBuilder &builder);

	void resolve_call_instrumentation ();
	bool instrumented (MonoProfilerCallInstrumentationFlags flag) const;
	void emit_profiler_event (MonoIrBuilder &builder, const char *raise, void *address,
	                          llvm::ArrayRef<llvm::Value *> args);
	void emit_profiler_enter (MonoIrBuilder &builder);
	void emit_profiler_leave (MonoIrBuilder &builder);
	void emit_profiler_frame_handover (MonoIrBuilder &builder, MonoMethod *target);

	llvm::Instruction *emit_seq_point (MonoIrBuilder &builder, uint32_t encoded_il,
	                                   uint8_t flags = 0);
	void emit_after_call_seq_point (MonoIrBuilder &builder, bool nests);
	void collect_sym_seq_points ();
	bool wants_seq_point_at (size_t offset) const;
	void build_seq_point_graph ();

	llvm::Error emit_instruction (MonoIrBuilder &builder);
	llvm::Error emit_prefix (int opcode, uint64_t operand);
	llvm::Align access_alignment (MonoType *location);
	bool can_access_atomically (llvm::Type *type, llvm::Align align);
	llvm::Value *emit_memory_load (MonoIrBuilder &builder, llvm::Type *type,
	                               llvm::Value *address, MonoType *location);
	llvm::Error emit_memory_store (MonoIrBuilder &builder, llvm::Value *value,
	                        llvm::Value *address, MonoType *location);

	llvm::Expected<Flow> decode_flow (size_t at);
	llvm::Error find_block_leaders ();
	void mark_reachable_blocks ();
	llvm::Expected<size_t> branch_target (int32_t displacement);
	llvm::Error skip_to_next_reachable_block (size_t end);
	llvm::Error translate_range (MonoIrBuilder &builder, size_t begin, size_t end);
	void finish_function ();
	void mark_for_tier2_instrumentation ();
	llvm::Error seed_handler_entry_stacks (MonoIrBuilder &builder);
	llvm::BasicBlock *create_cold_block (const llvm::Twine &name);
	llvm::AllocaInst *entry_alloca (llvm::Type *type, const llvm::Twine &name);
	llvm::AllocaInst *spill_slot (size_t depth, llvm::Type *type);
	std::vector<Slot> spill_stack (MonoIrBuilder &builder);
	llvm::Error enter_block (MonoIrBuilder &builder, size_t target,
	                         const std::vector<Slot> &slots);
	void reload_stack (MonoIrBuilder &builder, const Block &block);

	int innermost_try (size_t at) const;
	int innermost_handler (size_t at) const;
	std::vector<uint32_t> covering_chain (uint32_t clause) const;
	std::vector<uint32_t> finally_chain_to (size_t target) const;
	llvm::Error emit_undeniable_exception_rethrow (MonoIrBuilder &builder);
	llvm::Error check_delegate_invoke (MonoClass *klass);
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

	llvm::Error emit_dup (MonoIrBuilder &builder);
	llvm::Error emit_pop ();

	/// Where a call through a pointer returns a value too wide for the
	/// registers, hidden is the type behind that pointer. at is which
	/// argument carries it.
	///
	/// Both are given only for a site entered in this backend's own
	/// convention, since only the emitter knows which sites those are. A
	/// call still bound for MonoAbiPass must arrive without the pointer,
	/// and signals that by leaving hidden null.
	///
	/// A direct callee answers for itself and ignores both.
	llvm::Value *emit_protected_call (
		MonoIrBuilder &builder, llvm::FunctionCallee callee,
		llvm::ArrayRef<llvm::Value *> args,
		llvm::function_ref<void (llvm::CallBase *)> describe_site = {},
		llvm::Type *hidden = nullptr, unsigned at = 0);

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
	llvm::Value *delegate_invoke_callee (MonoIrBuilder &builder, llvm::Value *receiver,
	                                     MonoMethod *target);
	llvm::Constant *method_symbol (MonoMethod *target);
	llvm::Expected<llvm::Constant *> code_address_symbol (MonoMethod *target);
	MonoMethod *synchronized_target (MonoMethod *target);
	MonoClass *exact_receiver_class (const StackValue &receiver);
	MonoMethod *exact_virtual_target (const StackValue &receiver, MonoMethod *callee);
	bool is_own_this (llvm::Value *value);
	llvm::CallInst::TailCallKind should_tail_call (MonoMethodSignature *callee_sig,
	                                               MonoMethod *callee_method,
	                                               llvm::FunctionType *callee_type,
	                                               llvm::Type *callee_hidden);
	bool matching_call_abi (MonoMethodSignature *callee_sig, llvm::FunctionType *callee_type,
	                        llvm::Type *callee_hidden);
	llvm::Error emit_jmp (MonoIrBuilder &builder, uint32_t token);
	llvm::Error emit_tail_call (MonoIrBuilder &builder, llvm::FunctionCallee callee,
	                            llvm::ArrayRef<llvm::Value *> args,
	                            llvm::CallInst::TailCallKind kind, size_t arg_slots,
	                            llvm::Function *declaration,
	                            llvm::function_ref<void (llvm::CallBase *)> describe_site,
	                            bool natural = false);
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

	bool sharing () const { return context_used != 0; }

	/// Whether this body is entered with its runtime generic context in a
	/// register, because it has no receiver to read one out of.
	///
	/// This is upstream's own split, and it decides both the body's prototype
	/// and how the instantiation's entry is published, so the two have to ask
	/// the same question of the same method.
	bool takes_context_argument () const;

	/// Keeps in the frame, at an offset the jit info records, what this shared
	/// body was entered with - a receiver, or the context itself.
	void pin_context_slot (MonoIrBuilder &builder, llvm::Value *slot);

	/// Works out whether this body is a shared one, and reads the context it
	/// runs as. Call it from the prologue, once the arguments have their slots.
	void open_sharing (MonoIrBuilder &builder);

	/// Records that this body cannot be shared, because of \p what.
	///
	/// Translation carries on. The first refusal is the one reported, and the
	/// body it produces is thrown away.
	void cannot_share (const llvm::Twine &what);

	/// Whether \p klass, \p field or \p target names a type parameter, and so
	/// stands for a different runtime object in each instantiation.
	bool depends_on_context (MonoClass *klass);
	bool depends_on_context (MonoClassField *field);
	bool depends_on_context (MonoMethod *target);

	/// Whether a call to \p target has to read the entry to call out of the
	/// context, because the callee is compiled once per instantiation.
	///
	/// Records a refusal for a callee that depends on the context and cannot be
	/// reached that way at all, so the answer is also what says a direct call
	/// is safe.
	bool calls_through_context (MonoMethod *target);

	/// Appends the context \p callee declared it is entered with, and returns
	/// whether it declared one. The site then has to mark that argument nest.
	bool pass_context_to (llvm::Function *callee, std::vector<llvm::Value *> &args);

	/// The runtime object an rgctx entry of \p info_type over \p data resolves
	/// to in the instantiation this body is running as.
	///
	/// \p data is a MonoClass, a MonoMethod or a MonoClassField, whichever
	/// \p info_type is about.
	llvm::Expected<llvm::Value *> rgctx_fetch (MonoIrBuilder &builder,
	                                           MonoRgctxInfoType info_type, void *data);

	/// The per-class run-time structure \p prefix names, fetched from the
	/// context when \p klass names a type parameter and burned in otherwise.
	llvm::Expected<llvm::Value *> class_operand (MonoIrBuilder &builder, MonoClass *klass,
	                                             const char *prefix);
	llvm::Expected<llvm::Value *> field_operand (MonoIrBuilder &builder,
	                                             MonoClassField *field);
	llvm::Expected<llvm::Value *> method_operand (MonoIrBuilder &builder,
	                                              MonoMethod *target);

	llvm::Expected<llvm::Value *> code_operand (MonoIrBuilder &builder,
	                                            MonoMethod *target);

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
	llvm::Expected<bool> fold_type_from_handle (MonoIrBuilder &builder, MonoType *type);
	bool cctor_already_ran (MonoClass *klass);
	llvm::Error emit_class_init (MonoIrBuilder &builder, MonoClass *klass);
	llvm::Expected<llvm::Value *> static_field_address (MonoIrBuilder &builder,
	                                                    MonoClassField *field);
	static MonoType *builtin_element_type (int opcode);
	llvm::Expected<MonoClass *> resolve_class (uint32_t token);
	llvm::Expected<MonoType *> element_type_from_token (uint32_t token);
	llvm::Expected<llvm::Value *> array_length (MonoIrBuilder &builder, StackValue array);
	llvm::Expected<llvm::Value *> element_address (MonoIrBuilder &builder, StackValue array,
	                                               StackValue index, MonoType *element);
	llvm::Error emit_ldlen (MonoIrBuilder &builder);
	llvm::Expected<llvm::Value *> emit_vector_alloc (MonoIrBuilder &builder, MonoClass *array,
	                                                 llvm::Value *length);
	llvm::Error emit_newarr (MonoIrBuilder &builder, uint32_t token);
	llvm::Error emit_ldelema (MonoIrBuilder &builder, uint32_t token);
	llvm::Error emit_ldelem (MonoIrBuilder &builder, MonoType *element);
	llvm::Error emit_stelem (MonoIrBuilder &builder, MonoType *element);
	MonoClass *exact_element_class (const StackValue &array);
	bool is_always_an_instance_of (const StackValue &value, MonoClass *element);
	llvm::Error emit_stelem_ref_check (MonoIrBuilder &builder, const StackValue &array,
	                                   const StackValue &value, llvm::Value *stored);
	llvm::Error emit_array_type_check (MonoIrBuilder &builder, llvm::Value *array,
	                                   MonoClass *array_class);
	void consume_save_last_error (MonoIrBuilder &builder);
	llvm::Expected<llvm::Value *> array_accessor_address (MonoIrBuilder &builder,
	                                                      MonoClass *klass,
	                                                      llvm::Value *array,
	                                                      llvm::ArrayRef<llvm::Value *> indices);
	llvm::Error emit_array_accessor_call (MonoIrBuilder &builder, MonoMethod *accessor,
	                                      MonoMethodSignature *sig);
	llvm::Error emit_unsafe_mov (MonoIrBuilder &builder, MonoMethodSignature *sig);
	llvm::Error emit_array_rank (MonoIrBuilder &builder);
	llvm::Error emit_array_total_length (MonoIrBuilder &builder);
	llvm::Error emit_array_dimension (MonoIrBuilder &builder, bool lower_bound);
	llvm::Error emit_string_length (MonoIrBuilder &builder);
	llvm::Error emit_get_type (MonoIrBuilder &builder, bool receiver_by_reference);
	llvm::FunctionCallee libm_decl (const char *name, llvm::Type *type, size_t arity);
	llvm::Error emit_math_call (MonoIrBuilder &builder, const MathIntrinsic &math,
	                            MonoMethodSignature *sig);

	llvm::Expected<llvm::Value *> indirect_address (MonoIrBuilder &builder,
	                                                StackValue address);
	llvm::Error emit_ldind (MonoIrBuilder &builder, MonoType *element);
	llvm::Error emit_stind (MonoIrBuilder &builder, MonoType *element);
	llvm::FunctionCallee value_copy_decl ();
	llvm::Expected<llvm::Function *> object_new_decl ();
	llvm::Expected<llvm::Value *> emit_object_alloc (MonoIrBuilder &builder,
	                                                 MonoClass *klass, bool for_box);
	llvm::Value *unbox_payload (MonoIrBuilder &builder, llvm::Value *obj,
	                            MonoClass *klass);
	llvm::Error call_nullable_helper (MonoIrBuilder &builder, MonoClass *klass,
	                                  const char *name);
	llvm::Expected<llvm::Value *> box_nullable (MonoIrBuilder &builder, MonoClass *klass,
	                                            StackValue value);
	llvm::Expected<llvm::Value *> box_value (MonoIrBuilder &builder, MonoClass *klass, MonoType *type,
	                        llvm::Value *value);
	llvm::Error emit_newobj (MonoIrBuilder &builder, uint32_t token);
	llvm::Error emit_array_newobj (MonoIrBuilder &builder, MonoMethod *ctor,
	                               MonoMethodSignature *sig);
	llvm::Expected<llvm::Value *> emit_string_constructor (MonoIrBuilder &builder,
	                                                       MonoMethod *ctor,
	                                                       llvm::ArrayRef<llvm::Value *> args);
	llvm::Error emit_string_constructor_call (MonoIrBuilder &builder, MonoMethod *ctor,
	                                          MonoMethodSignature *sig);
	llvm::Error emit_box (MonoIrBuilder &builder, uint32_t token);
	llvm::Error emit_unbox (MonoIrBuilder &builder, uint32_t token);
	llvm::Error emit_unbox_any (MonoIrBuilder &builder, uint32_t token);
	llvm::Error emit_cast (MonoIrBuilder &builder, uint32_t token, bool throw_on_fail);
	void emit_subtype_test (MonoIrBuilder &builder, MonoClass *klass, llvm::Value *obj,
	                        llvm::Value *target, llvm::BasicBlock *yes,
	                        llvm::BasicBlock *otherwise);
	void emit_interface_test (MonoIrBuilder &builder, MonoClass *klass, llvm::Value *obj,
	                          llvm::BasicBlock *yes, llvm::BasicBlock *otherwise);
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
	llvm::Error emit_user_break (MonoIrBuilder &builder);

private:
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

	void push_stack (llvm::Value *value, MonoType *type, bool native = false)
	{
		stack.push_back ({ value, type, native });
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

/// The SExt/ZExt attribute a narrow integer argument or return value needs
/// to fill its register, or None for everything else.
llvm::Attribute::AttrKind integer_extension (MonoType *t);

/// How many bytes of operand an opcode carries, or nothing for a switch, whose
/// length is in its own operand.
///
/// Anything walking IL needs this, and a second copy of the table disagrees
/// with this one the first time an operand kind is added.
std::optional<size_t> il_operand_size (MonoOpcodeEnum opcode);

/// Whether method has no IL body of its own to translate: an icall, a
/// pinvoke declaration, or a method the runtime implements itself. A
/// wrapper is never one of these, even when it wraps such a method,
/// because it always carries IL of its own.
bool implemented_outside_il (MonoMethod *method);

/// Whether this method is implemented entirely by the backend, with its
/// actual IL ignored.
///
/// This is true only for System.ByReference`1.
bool is_intrinsic (MonoMethod *method);

/// The method a direct call to method enters, which for an internal call is
/// the marshalling wrapper the runtime publishes in its place.
///
/// An icall has no body of its own. mini answers a request to compile one
/// with the wrapper it builds around the registered C function
/// (compile_special, mini-runtime.c). That wrapper is a method this backend
/// compiles like any other. Naming it here lets a call site reach the same
/// code naturally rather than through a C-convention entry.
///
/// Anything else comes back unchanged, including the two kinds of icall
/// that have no such wrapper. One is registered as needing none, so its
/// published address really is the C function. The other is an array
/// accessor, which every call site lowers inline instead.
MonoMethod *icall_wrapper_target (MonoMethod *method);

/// What to emit for a call to method, or nothing when the backend has no
/// arithmetic for it. sig is the signature the call site was written against.
std::optional<MathIntrinsic> math_intrinsic_for (MonoMethod *method, MonoMethodSignature *sig);

/// The number of sig's parameters that are ordinary ones, which for a vararg
/// signature means the fixed part ahead of the sentinel.
///
/// A vararg method's signature carries its sentinel past the last parameter,
/// so a declaration and every call site that names it agree on this count.
/// That agreement is what lets both convert to one function type.
int vararg_fixed_params (MonoMethodSignature *sig);

/// Whether the address the runtime publishes for method is a C function
/// this backend did not generate.
///
/// A method implemented outside IL declares a pinvoke signature. The loader
/// sets that flag for every icall and every DllImport. But what stands
/// behind its symbol is almost always the marshaling wrapper the runtime
/// builds around it. That wrapper is a method this backend compiles and
/// publishes like any other.
///
/// One exception is an icall registered as needing no wrapper at all, whose
/// published address really is the C function. So the method, not its
/// signature, says which convention its entry speaks.
bool entered_in_c (MonoMethod *method);

/// Translates method's IL into the corresponding LLVM function, within
/// module.
///
/// externals, when given, collects the symbols the emitted module leaves
/// for the engine to resolve. bp_switch, when given, receives the body's
/// soft-debugger breakpoint switch, which is null unless the body emitted
/// sequence points. seq_points, when given, receives the body's
/// sequence-point graph, empty for the same reason.
///
/// siblings names the other methods translated into this module. Give it the
/// whole batch: a call to a method named there leaves through that method's
/// published entry rather than reaching its body in this module.
///
/// types is the module's struct-type cache, and every translation into one
/// module has to be handed the same one. Null makes a cache for this
/// translation alone, which is right only when the module holds nothing else.
///
/// body_suffix goes on the end of the emitted function's name. Give it a value
/// when the module already holds a body for method and this one is a copy.
llvm::Expected<llvm::Function *> method_to_llvm (llvm::Module *module, MonoCompile *cfg,
                                                 MonoMethod *method,
                                                 std::vector<ExternalSymbol> *externals
                                                 = nullptr,
                                                 MonoLLVMBreakpointSwitch **bp_switch
                                                 = nullptr,
                                                 SeqPointGraph *seq_points = nullptr,
                                                 llvm::ArrayRef<MonoMethod *> siblings = {},
                                                 ModuleTypes *types = nullptr,
                                                 llvm::StringRef body_suffix = {});

} // namespace mono

#endif

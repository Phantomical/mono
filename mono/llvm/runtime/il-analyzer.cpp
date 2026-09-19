#include "runtime-error.hpp"

#include "il-analyzer.hpp"

#include "inline-scope.hpp"
#include "method-to-llvm.hpp"
#include "method-to-llvm/intrinsics.hpp"

#include <llvm/ADT/ArrayRef.h>
#include <llvm/ADT/SmallVector.h>

#include <cstring>
#include <optional>
#include <vector>

#include "mini.h"
#include "mini-runtime.h"

#include "mono/metadata/class-internals.h"
#include "mono/metadata/opcodes.h"

using llvm::ArrayRef;
using llvm::SmallVector;

namespace mono {

namespace {

// These names match the opcode table's Pop and Push columns. Variable stack
// effects are derived from signatures where supported.
constexpr int Pop0 = 0, Pop1 = 1, PopI = 1, PopI8 = 1, PopR4 = 1, PopR8 = 1, PopRef = 1;
constexpr int Push0 = 0, Push1 = 1, PushI = 1, PushI8 = 1, PushR4 = 1, PushR8 = 1, PushRef = 1;
constexpr int VarPop = -1, VarPush = -1;

struct StackEffect {
	int8_t pops;
	int8_t pushes;
};

#define OPDEF(a, b, c, d, e, f, g, h, i, j) { static_cast<int8_t> (c), static_cast<int8_t> (d) },
constexpr StackEffect stack_effects[] = {
#include "mono/cil/opcode.def"
};
#undef OPDEF

static_assert (sizeof (stack_effects) / sizeof (stack_effects[0]) == MONO_CEE_LAST,
               "the stack-effect table and MonoOpcodeEnum are read from the same def");

struct Instr {
	uint32_t offset;

	/// Offset of the operand.
	uint32_t operand;

	/// Offset of the next instruction.
	uint32_t next;

	MonoOpcodeEnum op;
};

std::optional<Instr>
decode (const unsigned char *code, uint32_t size, uint32_t at)
{
	const unsigned char *cursor = code + at;
	MonoOpcodeEnum op = mono_opcode_value (&cursor, code + size);

	if (op == MonoOpcodeEnum_Invalid)
		return std::nullopt;

	// mono_opcode_value () leaves the cursor on the opcode's last byte.
	uint32_t operand = static_cast<uint32_t> (cursor - code) + 1;
	std::optional<size_t> width = il_operand_size (op);

	if (width) {
		if (operand + *width > size)
			return std::nullopt;

		return Instr { at, operand, static_cast<uint32_t> (operand + *width), op };
	}

	// A switch carries a count, then that many four-byte displacements.
	if (size - operand < 4)
		return std::nullopt;

	uint64_t next = operand + 4 + static_cast<uint64_t> (il_read_u32 (code + operand)) * 4;

	if (next > size)
		return std::nullopt;

	return Instr { at, operand, static_cast<uint32_t> (next), op };
}

/// Whether control can continue at the instruction after instr.
bool
falls_through (const Instr &instr)
{
	// Debugger breaks resume at the next instruction, and mono_ldnativeobj only
	// pushes a value, despite their opcode-table flow classifications.
	if (instr.op == MONO_CEE_BREAK || instr.op == MONO_CEE_MONO_LDNATIVEOBJ)
		return true;

	switch (mono_opcodes[instr.op].flow_type) {
	case MONO_FLOW_BRANCH:
	case MONO_FLOW_RETURN:
	case MONO_FLOW_ERROR:
		return false;
	case MONO_FLOW_CALL:
		return instr.op != MONO_CEE_JMP;
	default:
		return true;
	}
}

/// The offsets instr can transfer control to, other than by falling through.
SmallVector<uint32_t, 2>
branch_targets (const unsigned char *code, const Instr &instr)
{
	SmallVector<uint32_t, 2> targets;

	// A displacement is counted from the instruction after the branch.
	switch (mono_opcodes[instr.op].argument) {
	case MonoShortInlineBrTarget:
		targets.push_back (instr.next + static_cast<int8_t> (code[instr.operand]));
		break;
	case MonoInlineBrTarget:
		targets.push_back (instr.next
		                   + static_cast<int32_t> (il_read_u32 (code + instr.operand)));
		break;
	case MonoInlineSwitch:
		for (uint32_t at = instr.operand + 4; at < instr.next; at += 4)
			targets.push_back (instr.next + static_cast<int32_t> (il_read_u32 (code + at)));
		break;
	default:
		break;
	}

	return targets;
}

bool
is_branch (const Instr &instr)
{
	switch (mono_opcodes[instr.op].argument) {
	case MonoShortInlineBrTarget:
	case MonoInlineBrTarget:
	case MonoInlineSwitch:
		return true;
	default:
		return false;
	}
}

/// Abstract value tracked for an evaluation-stack, local or argument slot.
struct Value {
	enum class Kind : uint8_t {
		unknown,
		integer,
		null,
		non_null,
		type_handle,
		type_object,
	};

	Kind kind = Kind::unknown;

	/// Whether an integer is 64-bit or native-sized rather than int32.
	bool wide = false;

	/// Integer value. Int32 values are stored sign-extended.
	int64_t integer = 0;

	/// Type represented by a type_handle or type_object value.
	MonoType *type = nullptr;

	static Value int32 (int64_t v)
	{
		Value value;

		value.kind = Kind::integer;
		value.integer = static_cast<int32_t> (v);
		return value;
	}

	static Value int64 (int64_t v)
	{
		Value value;

		value.kind = Kind::integer;
		value.wide = true;
		value.integer = v;
		return value;
	}

	static Value boolean (bool v) { return int32 (v ? 1 : 0); }

	static Value of_kind (Kind kind)
	{
		Value value;

		value.kind = kind;
		return value;
	}

	static Value typed (Kind kind, MonoType *type)
	{
		Value value;

		value.kind = kind;
		value.type = type;
		return value;
	}

	bool is (Kind k) const { return kind == k; }

	/// Whether lowering can replace this value with a constant. Unlike
	/// non_null, these kinds identify the exact value.
	bool is_constant () const
	{
		return kind == Kind::integer || kind == Kind::type_handle
		       || kind == Kind::type_object || kind == Kind::null;
	}

	bool operator== (const Value &other) const
	{
		if (kind != other.kind)
			return false;

		switch (kind) {
		case Kind::integer:
			return wide == other.wide && integer == other.integer;
		case Kind::type_handle:
		case Kind::type_object:
			return type == other.type;
		default:
			return true;
		}
	}
};

Value
meet (const Value &a, const Value &b)
{
	return a == b ? a : Value ();
}

/// Abstract interpreter state at a control-flow edge.
struct State {
	SmallVector<Value, 8> stack;
	SmallVector<Value, 8> locals;
	SmallVector<Value, 8> args;
};

enum class Meet {
	unchanged,
	changed,

	/// Incoming evaluation stacks have different heights.
	mismatch,
};

bool
meet_slots (SmallVector<Value, 8> &into, const SmallVector<Value, 8> &from)
{
	bool changed = false;

	for (size_t i = 0; i < into.size (); ++i) {
		Value met = meet (into[i], from[i]);

		if (!(met == into[i])) {
			into[i] = met;
			changed = true;
		}
	}

	return changed;
}

Meet
meet_into (State &into, const State &from)
{
	if (into.stack.size () != from.stack.size ())
		return Meet::mismatch;

	bool changed = meet_slots (into.stack, from.stack);

	changed |= meet_slots (into.locals, from.locals);
	changed |= meet_slots (into.args, from.args);
	return changed ? Meet::changed : Meet::unchanged;
}

/// Whether type is a generic parameter in a body shared by reference-type
/// instantiations.
bool
is_shared_reference_parameter (MonoType *type)
{
	return mono_type_is_generic_parameter (type) && mini_type_is_reference (type);
}

/// Compares two types when their identities are known in this instantiation.
std::optional<bool>
compare_types (MonoType *a, MonoType *b, bool sharing)
{
	MonoClass *ka = mono_class_from_mono_type_internal (a);
	MonoClass *kb = mono_class_from_mono_type_internal (b);
	bool open_a = class_depends_on_context (ka, sharing);
	bool open_b = class_depends_on_context (kb, sharing);

	if (!open_a && !open_b)
		return ka == kb && a->byref == b->byref;

	if (open_a != open_b) {
		MonoType *open = open_a ? a : b;
		MonoType *closed = open_a ? b : a;
		MonoClass *closed_class = open_a ? kb : ka;

		if (is_shared_reference_parameter (open) && !closed->byref
		    && m_class_is_valuetype (closed_class))
			return false;
	}

	return std::nullopt;
}

/// Evaluates Type.IsValueType when the type is known in this instantiation.
std::optional<bool>
is_value_type (MonoType *type, bool sharing)
{
	if (type->byref)
		return std::nullopt;

	if (is_shared_reference_parameter (type))
		return false;

	MonoClass *klass = mono_class_from_mono_type_internal (type);

	if (class_depends_on_context (klass, sharing))
		return std::nullopt;

	return m_class_is_valuetype (klass) != 0;
}

/// Rule for folding a call result from its abstract arguments.
struct CallRule {
	MonoClass *(*klass) ();
	const char *name;
	Value (*fold) (ArrayRef<Value> args, bool sharing);
};

MonoClass *
system_type ()
{
	return mono_defaults.systemtype_class;
}

Value
from_optional (std::optional<bool> answer)
{
	return answer ? Value::boolean (*answer) : Value ();
}

/// Calls folded by the analyzer, matched by declaring class and method name.
/// A rule returns unknown when its arguments are insufficient.
const CallRule known_calls[] = {
	{ system_type, "GetTypeFromHandle",
	  [] (ArrayRef<Value> args, bool) {
		  return args[0].is (Value::Kind::type_handle)
		                 ? Value::typed (Value::Kind::type_object, args[0].type)
		                 : Value ();
	  } },
	{ system_type, "op_Equality",
	  [] (ArrayRef<Value> args, bool sharing) {
		  if (!args[0].is (Value::Kind::type_object) || !args[1].is (Value::Kind::type_object))
			  return Value ();

		  return from_optional (compare_types (args[0].type, args[1].type, sharing));
	  } },
	{ system_type, "op_Inequality",
	  [] (ArrayRef<Value> args, bool sharing) {
		  if (!args[0].is (Value::Kind::type_object) || !args[1].is (Value::Kind::type_object))
			  return Value ();

		  std::optional<bool> equal = compare_types (args[0].type, args[1].type, sharing);

		  return equal ? Value::boolean (!*equal) : Value ();
	  } },
	{ system_type, "get_IsValueType",
	  [] (ArrayRef<Value> args, bool sharing) {
		  return args[0].is (Value::Kind::type_object)
		                 ? from_optional (is_value_type (args[0].type, sharing))
		                 : Value ();
	  } },
};

Value
fold_call (MonoMethod *target, ArrayRef<Value> args, bool sharing)
{
	// Use the value emitted by the backend when determining which IL is reachable.
	if (is_vector_hardware_accelerated_getter (target))
		return Value::boolean (true);

	for (const CallRule &rule : known_calls) {
		if (target->klass != rule.klass () || strcmp (target->name, rule.name) != 0)
			continue;

		return rule.fold (args, sharing);
	}

	return Value ();
}

/// Converts value to a branch condition when known.
std::optional<bool>
truth (const Value &value)
{
	switch (value.kind) {
	case Value::Kind::integer:
		return value.integer != 0;
	case Value::Kind::null:
		return false;
	case Value::Kind::non_null:
	case Value::Kind::type_object:
		return true;
	default:
		return std::nullopt;
	}
}

/// Compares reference identity when known.
std::optional<bool>
same_reference (const Value &a, const Value &b, bool sharing)
{
	using Kind = Value::Kind;

	if (a.is (Kind::type_object) && b.is (Kind::type_object))
		return compare_types (a.type, b.type, sharing);
	if (a.is (Kind::null) && b.is (Kind::null))
		return true;
	if ((a.is (Kind::null) && b.is (Kind::non_null)) || (a.is (Kind::non_null) && b.is (Kind::null)))
		return false;
	if ((a.is (Kind::null) && b.is (Kind::type_object)) || (a.is (Kind::type_object) && b.is (Kind::null)))
		return false;

	return std::nullopt;
}

/// The comparison a compare opcode or a comparing branch makes.
enum class Compare {
	eq,
	ne,
	gt,
	gt_un,
	ge,
	ge_un,
	lt,
	lt_un,
	le,
	le_un,
};

std::optional<Compare>
compare_of (MonoOpcodeEnum op)
{
	switch (op) {
	case MONO_CEE_CEQ:
	case MONO_CEE_BEQ:
	case MONO_CEE_BEQ_S:
		return Compare::eq;
	case MONO_CEE_BNE_UN:
	case MONO_CEE_BNE_UN_S:
		return Compare::ne;
	case MONO_CEE_CGT:
	case MONO_CEE_BGT:
	case MONO_CEE_BGT_S:
		return Compare::gt;
	case MONO_CEE_CGT_UN:
	case MONO_CEE_BGT_UN:
	case MONO_CEE_BGT_UN_S:
		return Compare::gt_un;
	case MONO_CEE_BGE:
	case MONO_CEE_BGE_S:
		return Compare::ge;
	case MONO_CEE_BGE_UN:
	case MONO_CEE_BGE_UN_S:
		return Compare::ge_un;
	case MONO_CEE_CLT:
	case MONO_CEE_BLT:
	case MONO_CEE_BLT_S:
		return Compare::lt;
	case MONO_CEE_CLT_UN:
	case MONO_CEE_BLT_UN:
	case MONO_CEE_BLT_UN_S:
		return Compare::lt_un;
	case MONO_CEE_BLE:
	case MONO_CEE_BLE_S:
		return Compare::le;
	case MONO_CEE_BLE_UN:
	case MONO_CEE_BLE_UN_S:
		return Compare::le_un;
	default:
		return std::nullopt;
	}
}

/// Evaluates a comparison when both operands are known. Integers use the wider
/// operand width; references support identity comparisons only.
std::optional<bool>
evaluate_compare (Compare compare, const Value &a, const Value &b, bool sharing)
{
	if (!a.is (Value::Kind::integer) || !b.is (Value::Kind::integer)) {
		std::optional<bool> same = same_reference (a, b, sharing);

		if (!same)
			return std::nullopt;
		if (compare == Compare::eq)
			return *same;
		// C# spells a reference's != null as cgt.un against ldnull.
		if (compare == Compare::ne
		    || (compare == Compare::gt_un && b.is (Value::Kind::null)))
			return !*same;

		return std::nullopt;
	}

	bool wide = a.wide || b.wide;
	int64_t x = a.integer, y = b.integer;
	uint64_t ux = wide ? static_cast<uint64_t> (x) : static_cast<uint32_t> (x);
	uint64_t uy = wide ? static_cast<uint64_t> (y) : static_cast<uint32_t> (y);

	switch (compare) {
	case Compare::eq:
		return x == y;
	case Compare::ne:
		return x != y;
	case Compare::gt:
		return x > y;
	case Compare::gt_un:
		return ux > uy;
	case Compare::ge:
		return x >= y;
	case Compare::ge_un:
		return ux >= uy;
	case Compare::lt:
		return x < y;
	case Compare::lt_un:
		return ux < uy;
	case Compare::le:
		return x <= y;
	case Compare::le_un:
		return ux <= uy;
	}

	return std::nullopt;
}

/// Folds non-throwing integer arithmetic. Results wrap at the wider operand
/// width; division and overflow-checking operations remain unknown.
std::optional<Value>
fold_arithmetic (MonoOpcodeEnum op, const Value &a, const Value &b)
{
	if (!a.is (Value::Kind::integer) || !b.is (Value::Kind::integer))
		return std::nullopt;

	bool wide = a.wide || b.wide;
	uint64_t x = static_cast<uint64_t> (a.integer);
	uint64_t y = static_cast<uint64_t> (b.integer);
	unsigned width = wide ? 64 : 32;
	uint64_t result;

	switch (op) {
	case MONO_CEE_ADD:
		result = x + y;
		break;
	case MONO_CEE_SUB:
		result = x - y;
		break;
	case MONO_CEE_MUL:
		result = x * y;
		break;
	case MONO_CEE_AND:
		result = x & y;
		break;
	case MONO_CEE_OR:
		result = x | y;
		break;
	case MONO_CEE_XOR:
		result = x ^ y;
		break;
	case MONO_CEE_SHL:
	case MONO_CEE_SHR:
	case MONO_CEE_SHR_UN:
		// ECMA-335 leaves a shift by the width or more unspecified.
		if (b.integer < 0 || b.integer >= static_cast<int64_t> (width))
			return std::nullopt;
		if (op == MONO_CEE_SHL)
			result = x << y;
		else if (op == MONO_CEE_SHR)
			result = wide ? static_cast<uint64_t> (a.integer >> y)
			              : static_cast<uint64_t> (static_cast<int32_t> (a.integer) >> y);
		else
			result = wide ? x >> y : static_cast<uint32_t> (x) >> y;
		break;
	default:
		return std::nullopt;
	}

	return wide ? Value::int64 (static_cast<int64_t> (result))
	            : Value::int32 (static_cast<int64_t> (result));
}

/// Folds an integer conversion when its operand is known.
std::optional<Value>
fold_conversion (MonoOpcodeEnum op, const Value &a)
{
	if (!a.is (Value::Kind::integer))
		return std::nullopt;

	int64_t x = a.integer;
	uint64_t ux = a.wide ? static_cast<uint64_t> (x) : static_cast<uint32_t> (x);

	switch (op) {
	case MONO_CEE_CONV_I1:
		return Value::int32 (static_cast<int8_t> (x));
	case MONO_CEE_CONV_I2:
		return Value::int32 (static_cast<int16_t> (x));
	case MONO_CEE_CONV_I4:
		return Value::int32 (static_cast<int32_t> (x));
	case MONO_CEE_CONV_U1:
		return Value::int32 (static_cast<uint8_t> (x));
	case MONO_CEE_CONV_U2:
		return Value::int32 (static_cast<uint16_t> (x));
	case MONO_CEE_CONV_U4:
		return Value::int32 (static_cast<uint32_t> (x));
	case MONO_CEE_CONV_I8:
	case MONO_CEE_CONV_I:
		return Value::int64 (x);
	case MONO_CEE_CONV_U8:
	case MONO_CEE_CONV_U:
		return Value::int64 (static_cast<int64_t> (ux));
	default:
		return std::nullopt;
	}
}

/// Returns the slot referenced by an argument or local opcode.
std::optional<uint32_t>
slot_of (const unsigned char *code, const Instr &instr)
{
	switch (instr.op) {
	case MONO_CEE_LDARG_0:
	case MONO_CEE_LDLOC_0:
	case MONO_CEE_STLOC_0:
		return 0;
	case MONO_CEE_LDARG_1:
	case MONO_CEE_LDLOC_1:
	case MONO_CEE_STLOC_1:
		return 1;
	case MONO_CEE_LDARG_2:
	case MONO_CEE_LDLOC_2:
	case MONO_CEE_STLOC_2:
		return 2;
	case MONO_CEE_LDARG_3:
	case MONO_CEE_LDLOC_3:
	case MONO_CEE_STLOC_3:
		return 3;
	case MONO_CEE_LDARG_S:
	case MONO_CEE_LDARGA_S:
	case MONO_CEE_STARG_S:
	case MONO_CEE_LDLOC_S:
	case MONO_CEE_LDLOCA_S:
	case MONO_CEE_STLOC_S:
		return code[instr.operand];
	case MONO_CEE_LDARG:
	case MONO_CEE_LDARGA:
	case MONO_CEE_STARG:
	case MONO_CEE_LDLOC:
	case MONO_CEE_LDLOCA:
	case MONO_CEE_STLOC:
		return code[instr.operand] | (code[instr.operand + 1] << 8);
	default:
		return std::nullopt;
	}
}

enum class SlotOp {
	load_arg,
	load_arg_address,
	store_arg,
	load_local,
	load_local_address,
	store_local,
};

std::optional<SlotOp>
slot_op_of (MonoOpcodeEnum op)
{
	switch (op) {
	case MONO_CEE_LDARG_0:
	case MONO_CEE_LDARG_1:
	case MONO_CEE_LDARG_2:
	case MONO_CEE_LDARG_3:
	case MONO_CEE_LDARG_S:
	case MONO_CEE_LDARG:
		return SlotOp::load_arg;
	case MONO_CEE_LDARGA_S:
	case MONO_CEE_LDARGA:
		return SlotOp::load_arg_address;
	case MONO_CEE_STARG_S:
	case MONO_CEE_STARG:
		return SlotOp::store_arg;
	case MONO_CEE_LDLOC_0:
	case MONO_CEE_LDLOC_1:
	case MONO_CEE_LDLOC_2:
	case MONO_CEE_LDLOC_3:
	case MONO_CEE_LDLOC_S:
	case MONO_CEE_LDLOC:
		return SlotOp::load_local;
	case MONO_CEE_LDLOCA_S:
	case MONO_CEE_LDLOCA:
		return SlotOp::load_local_address;
	case MONO_CEE_STLOC_0:
	case MONO_CEE_STLOC_1:
	case MONO_CEE_STLOC_2:
	case MONO_CEE_STLOC_3:
	case MONO_CEE_STLOC_S:
	case MONO_CEE_STLOC:
		return SlotOp::store_local;
	default:
		return std::nullopt;
	}
}

/// A basic block.
struct Block {
	/// Indices into the instruction list, [first, end).
	uint32_t first;
	uint32_t end;

	uint32_t offset;
	uint32_t next_offset;

	/// State merged from incoming edges. An unset state means unreachable.
	std::optional<State> in;

	bool queued = false;
};

/// Abstract interpreter for one method body.
class ILAnalyzer {
public:
	ILAnalyzer (MonoMethod *method, MonoMethodHeader *header)
		: method_ (method), header_ (header), code_ (header->code),
		  size_ (header->code_size), sharing_ (mono_method_check_context_used (method) != 0)
	{
	}

	std::optional<ILReachability> run ();

private:
	bool decode_all ();
	bool build_blocks ();
	bool mark_leader (uint32_t offset, std::vector<bool> &leaders);
	void scan_addresses ();
	bool entry_state (State &state);

	bool reach (uint32_t offset, const State &state);
	bool reach_block (uint32_t block, const State &state);
	bool reach_handlers_of (const Block &block);

	bool evaluate (uint32_t block);
	bool visit (const Instr &instr, State &state);
	bool visit_call (const Instr &instr, State &state);
	bool visit_slot (const Instr &instr, SlotOp slot_op, State &state);
	bool terminate (const Instr &instr, State &state, uint32_t block);

	MonoClass *resolve_class (uint32_t token);

	MonoMethod *method_;
	MonoMethodHeader *header_;
	const unsigned char *code_;
	uint32_t size_;
	bool sharing_;

	std::vector<Instr> instrs_;

	/// The block starting at each offset, or -1 where none does.
	std::vector<int32_t> block_at_;
	std::vector<Block> blocks_;
	std::vector<uint32_t> worklist_;

	/// Whether each block terminator resolved to one successor.
	std::vector<bool> decided_;

	/// Instructions expected to disappear during constant folding.
	std::vector<bool> skip_bytes_;

	/// Whether the current call pushes a result. Call opcodes use VarPush, so
	/// visit_call () determines this from the resolved signature.
	bool pushed_call_result_ = false;

	/// Address-taken slots, whose contents must remain unknown.
	std::vector<bool> pinned_locals_;
	std::vector<bool> pinned_args_;

	/// Whether any instruction writes an argument slot.
	bool writes_args_ = false;

	/// Whether each clause's handler has been reached.
	std::vector<bool> handler_reached_;
};

bool
ILAnalyzer::decode_all ()
{
	for (uint32_t at = 0; at < size_;) {
		std::optional<Instr> instr = decode (code_, size_, at);

		if (!instr)
			return false;

		// These instructions require stack or control-flow semantics that the
		// analyzer does not model.
		if (instr->op >= MONO_CEE_MONO_ICALL || instr->op == MONO_CEE_CALLI
		    || instr->op == MONO_CEE_JMP)
			return false;

		instrs_.push_back (*instr);
		at = instr->next;
	}

	return true;
}

/// Marks a block leader and rejects targets inside instructions.
bool
ILAnalyzer::mark_leader (uint32_t offset, std::vector<bool> &leaders)
{
	if (offset >= size_ || block_at_[offset] < 0)
		return false;

	leaders[offset] = true;
	return true;
}

bool
ILAnalyzer::build_blocks ()
{
	// Use block_at_ as an instruction-start map until blocks are created.
	block_at_.assign (size_ + 1, -1);
	for (const Instr &instr : instrs_)
		block_at_[instr.offset] = 0;

	std::vector<bool> leaders (size_ + 1, false);

	leaders[0] = true;

	for (const Instr &instr : instrs_) {
		for (uint32_t target : branch_targets (code_, instr))
			if (!mark_leader (target, leaders))
				return false;

		// Split after every branch or terminator, including unreachable code.
		if ((is_branch (instr) || !falls_through (instr)) && instr.next < size_)
			leaders[instr.next] = true;
	}

	// Split at exception-region boundaries so membership is block-aligned.
	for (unsigned i = 0; i < header_->num_clauses; ++i) {
		const MonoExceptionClause &clause = header_->clauses[i];
		uint32_t ends[] = { clause.try_offset + clause.try_len,
			                clause.handler_offset + clause.handler_len };

		if (!mark_leader (clause.try_offset, leaders)
		    || !mark_leader (clause.handler_offset, leaders))
			return false;
		if (clause.flags == MONO_EXCEPTION_CLAUSE_FILTER
		    && !mark_leader (clause.data.filter_offset, leaders))
			return false;

		for (uint32_t end : ends) {
			if (end > size_)
				return false;
			if (end < size_ && !mark_leader (end, leaders))
				return false;
		}
	}

	std::fill (block_at_.begin (), block_at_.end (), -1);

	for (uint32_t i = 0; i < instrs_.size (); ++i) {
		if (leaders[instrs_[i].offset]) {
			block_at_[instrs_[i].offset] = static_cast<int32_t> (blocks_.size ());
			blocks_.push_back (Block { i, i + 1, instrs_[i].offset, instrs_[i].next });
		} else {
			blocks_.back ().end = i + 1;
			blocks_.back ().next_offset = instrs_[i].next;
		}
	}

	decided_.assign (blocks_.size (), false);
	skip_bytes_.assign (instrs_.size (), false);
	handler_reached_.assign (header_->num_clauses, false);
	return true;
}

void
ILAnalyzer::scan_addresses ()
{
	for (const Instr &instr : instrs_) {
		std::optional<SlotOp> slot_op = slot_op_of (instr.op);

		if (!slot_op)
			continue;

		uint32_t slot = *slot_of (code_, instr);

		switch (*slot_op) {
		case SlotOp::load_local_address:
			if (slot < pinned_locals_.size ())
				pinned_locals_[slot] = true;
			break;
		case SlotOp::load_arg_address:
			if (slot < pinned_args_.size ())
				pinned_args_[slot] = true;
			writes_args_ = true;
			break;
		case SlotOp::store_arg:
			writes_args_ = true;
			break;
		default:
			break;
		}
	}
}

/// Initializes the method entry state with unknown arguments and locals.
bool
ILAnalyzer::entry_state (State &state)
{
	MonoMethodSignature *sig = mono_method_signature_internal (method_);

	if (sig == nullptr)
		return false;

	state.args.assign (sig->param_count + (sig->hasthis ? 1 : 0), Value ());
	state.locals.assign (header_->num_locals, Value ());
	pinned_locals_.assign (state.locals.size (), false);
	pinned_args_.assign (state.args.size (), false);
	return true;
}

/// Merges state into the block at offset and queues it when the result changes.
bool
ILAnalyzer::reach (uint32_t offset, const State &state)
{
	if (offset >= size_ || block_at_[offset] < 0)
		return false;

	return reach_block (static_cast<uint32_t> (block_at_[offset]), state);
}

bool
ILAnalyzer::reach_block (uint32_t index, const State &state)
{
	Block &block = blocks_[index];
	bool changed;

	if (!block.in) {
		block.in = state;
		changed = true;
	} else {
		Meet met = meet_into (*block.in, state);

		if (met == Meet::mismatch)
			return false;

		changed = met == Meet::changed;
	}

	if (changed && !block.queued) {
		block.queued = true;
		worklist_.push_back (index);
	}

	return true;
}

/// Makes handlers for the block's protected region reachable. Because a
/// handler may start after any instruction in that region, locals are unknown;
/// arguments are also unknown if the method can modify them.
bool
ILAnalyzer::reach_handlers_of (const Block &block)
{
	for (unsigned i = 0; i < header_->num_clauses; ++i) {
		const MonoExceptionClause &clause = header_->clauses[i];

		if (handler_reached_[i] || block.offset < clause.try_offset
		    || block.offset >= clause.try_offset + clause.try_len)
			continue;

		handler_reached_[i] = true;

		State state;

		state.locals.assign (header_->num_locals, Value ());
		state.args = writes_args_ ? SmallVector<Value, 8> (blocks_[0].in->args.size (), Value ())
		                          : blocks_[0].in->args;

		if (clause.flags == MONO_EXCEPTION_CLAUSE_NONE
		    || clause.flags == MONO_EXCEPTION_CLAUSE_FILTER)
			state.stack.push_back (Value ());

		if (clause.flags == MONO_EXCEPTION_CLAUSE_FILTER
		    && !reach (clause.data.filter_offset, state))
			return false;
		if (!reach (clause.handler_offset, state))
			return false;
	}

	return true;
}

MonoClass *
ILAnalyzer::resolve_class (uint32_t token)
{
	ERROR_DECL (error);
	MonoClass *klass = mono_class_get_and_inflate_typespec_checked (
		m_class_get_image (method_->klass), token, mono_method_get_context (method_), error);

	if (klass == nullptr)
		mono_error_cleanup (error);

	return klass;
}

bool
ILAnalyzer::visit_slot (const Instr &instr, SlotOp slot_op, State &state)
{
	uint32_t slot = *slot_of (code_, instr);
	bool is_arg = slot_op == SlotOp::load_arg || slot_op == SlotOp::load_arg_address
	              || slot_op == SlotOp::store_arg;
	SmallVector<Value, 8> &slots = is_arg ? state.args : state.locals;
	const std::vector<bool> &pinned = is_arg ? pinned_args_ : pinned_locals_;

	if (slot >= slots.size ())
		return false;

	switch (slot_op) {
	case SlotOp::load_arg:
	case SlotOp::load_local:
		state.stack.push_back (slots[slot]);
		return true;
	case SlotOp::load_arg_address:
	case SlotOp::load_local_address:
		state.stack.push_back (Value ());
		return true;
	case SlotOp::store_arg:
	case SlotOp::store_local:
		if (state.stack.empty ())
			return false;

		Value stored = state.stack.pop_back_val ();

		slots[slot] = pinned[slot] ? Value () : stored;
		return true;
	}

	return false;
}

/// Applies a call's stack effect and folds its result when a rule is available.
/// Unresolved targets and vararg calls are not analyzed.
bool
ILAnalyzer::visit_call (const Instr &instr, State &state)
{
	MonoMethod *target = il_call_target (method_, il_read_u32 (code_ + instr.operand));

	if (target == nullptr)
		return false;

	MonoMethodSignature *sig = mono_method_signature_internal (target);

	if (sig == nullptr || sig->call_convention == MONO_CALL_VARARG)
		return false;

	bool constructing = instr.op == MONO_CEE_NEWOBJ;
	size_t count = sig->param_count + (sig->hasthis && !constructing ? 1 : 0);

	if (state.stack.size () < count)
		return false;

	ArrayRef<Value> args (state.stack.end () - count, count);
	Value result = constructing ? Value::of_kind (Value::Kind::non_null)
	                            : fold_call (target, args, sharing_);

	state.stack.pop_back_n (count);

	pushed_call_result_ = constructing || sig->ret->type != MONO_TYPE_VOID;
	if (pushed_call_result_)
		state.stack.push_back (result);

	return true;
}

/// Evaluates a non-terminator. Unhandled opcodes use their declared stack
/// effect and produce unknown values.
bool
ILAnalyzer::visit (const Instr &instr, State &state)
{
	using Kind = Value::Kind;

	auto pop = [&] (Value &into) {
		if (state.stack.empty ())
			return false;

		into = state.stack.pop_back_val ();
		return true;
	};

	if (std::optional<SlotOp> slot_op = slot_op_of (instr.op))
		return visit_slot (instr, *slot_op, state);

	switch (instr.op) {
	case MONO_CEE_LDC_I4_M1:
	case MONO_CEE_LDC_I4_0:
	case MONO_CEE_LDC_I4_1:
	case MONO_CEE_LDC_I4_2:
	case MONO_CEE_LDC_I4_3:
	case MONO_CEE_LDC_I4_4:
	case MONO_CEE_LDC_I4_5:
	case MONO_CEE_LDC_I4_6:
	case MONO_CEE_LDC_I4_7:
	case MONO_CEE_LDC_I4_8:
		state.stack.push_back (Value::int32 (instr.op - MONO_CEE_LDC_I4_0));
		return true;
	case MONO_CEE_LDC_I4_S:
		state.stack.push_back (Value::int32 (static_cast<int8_t> (code_[instr.operand])));
		return true;
	case MONO_CEE_LDC_I4:
		state.stack.push_back (
			Value::int32 (static_cast<int32_t> (il_read_u32 (code_ + instr.operand))));
		return true;
	case MONO_CEE_LDC_I8:
		state.stack.push_back (Value::int64 (
			static_cast<int64_t> (il_read_u32 (code_ + instr.operand))
			| (static_cast<int64_t> (il_read_u32 (code_ + instr.operand + 4)) << 32)));
		return true;

	case MONO_CEE_LDNULL:
		state.stack.push_back (Value::of_kind (Kind::null));
		return true;
	case MONO_CEE_LDSTR:
		state.stack.push_back (Value::of_kind (Kind::non_null));
		return true;
	case MONO_CEE_NEWARR: {
		Value length;

		if (!pop (length))
			return false;

		state.stack.push_back (Value::of_kind (Kind::non_null));
		return true;
	}

	case MONO_CEE_DUP:
		if (state.stack.empty ())
			return false;
		state.stack.push_back (state.stack.back ());
		return true;
	case MONO_CEE_POP:
		if (state.stack.empty ())
			return false;
		state.stack.pop_back ();
		return true;

	case MONO_CEE_LDTOKEN: {
		ERROR_DECL (error);
		MonoClass *handle_class = nullptr;
		gpointer handle = mono_ldtoken_checked (m_class_get_image (method_->klass),
		                                        il_read_u32 (code_ + instr.operand),
		                                        &handle_class, mono_method_get_context (method_),
		                                        error);

		if (handle == nullptr)
			mono_error_cleanup (error);

		state.stack.push_back (handle != nullptr && handle_class == mono_defaults.typehandle_class
		                               ? Value::typed (Kind::type_handle,
		                                               static_cast<MonoType *> (handle))
		                               : Value ());
		return true;
	}

	case MONO_CEE_SIZEOF: {
		MonoClass *klass = resolve_class (il_read_u32 (code_ + instr.operand));

		if (klass == nullptr || class_depends_on_context (klass, sharing_))
			state.stack.push_back (Value ());
		else
			state.stack.push_back (Value::int32 (m_class_is_valuetype (klass)
			                                             ? mono_class_value_size (klass, nullptr)
			                                             : TARGET_SIZEOF_VOID_P));
		return true;
	}

	case MONO_CEE_BOX: {
		// Boxing preserves references. Non-nullable value types produce a
		// non-null object, while Nullable<T> may produce null.
		Value value;
		MonoClass *klass = resolve_class (il_read_u32 (code_ + instr.operand));

		if (!pop (value))
			return false;
		if (klass == nullptr || class_depends_on_context (klass, sharing_))
			state.stack.push_back (Value ());
		else if (!m_class_is_valuetype (klass))
			state.stack.push_back (value);
		else if (mono_class_is_nullable (klass))
			state.stack.push_back (Value ());
		else
			state.stack.push_back (Value::of_kind (Kind::non_null));
		return true;
	}

	case MONO_CEE_CALL:
	case MONO_CEE_CALLVIRT:
	case MONO_CEE_NEWOBJ:
		return visit_call (instr, state);

	case MONO_CEE_ADD:
	case MONO_CEE_SUB:
	case MONO_CEE_MUL:
	case MONO_CEE_AND:
	case MONO_CEE_OR:
	case MONO_CEE_XOR:
	case MONO_CEE_SHL:
	case MONO_CEE_SHR:
	case MONO_CEE_SHR_UN: {
		Value b, a;

		if (!pop (b) || !pop (a))
			return false;

		state.stack.push_back (fold_arithmetic (instr.op, a, b).value_or (Value ()));
		return true;
	}

	case MONO_CEE_NOT:
	case MONO_CEE_NEG: {
		Value a;

		if (!pop (a))
			return false;
		if (!a.is (Kind::integer)) {
			state.stack.push_back (Value ());
			return true;
		}

		uint64_t x = static_cast<uint64_t> (a.integer);
		uint64_t result = instr.op == MONO_CEE_NOT ? ~x : 0 - x;

		state.stack.push_back (a.wide ? Value::int64 (static_cast<int64_t> (result))
		                              : Value::int32 (static_cast<int64_t> (result)));
		return true;
	}

	case MONO_CEE_CEQ:
	case MONO_CEE_CGT:
	case MONO_CEE_CGT_UN:
	case MONO_CEE_CLT:
	case MONO_CEE_CLT_UN: {
		Value b, a;

		if (!pop (b) || !pop (a))
			return false;

		state.stack.push_back (
			from_optional (evaluate_compare (*compare_of (instr.op), a, b, sharing_)));
		return true;
	}

	case MONO_CEE_CONV_I1:
	case MONO_CEE_CONV_I2:
	case MONO_CEE_CONV_I4:
	case MONO_CEE_CONV_I8:
	case MONO_CEE_CONV_U1:
	case MONO_CEE_CONV_U2:
	case MONO_CEE_CONV_U4:
	case MONO_CEE_CONV_U8:
	case MONO_CEE_CONV_I:
	case MONO_CEE_CONV_U: {
		Value a;

		if (!pop (a))
			return false;

		state.stack.push_back (fold_conversion (instr.op, a).value_or (Value ()));
		return true;
	}

	default: {
		StackEffect effect = stack_effects[instr.op];

		if (effect.pops < 0 || effect.pushes < 0 || state.stack.size () < (size_t) effect.pops)
			return false;

		state.stack.pop_back_n (effect.pops);
		state.stack.append (effect.pushes, Value ());
		return true;
	}
	}
}

/// Propagates state to every possible successor of a block terminator.
bool
ILAnalyzer::terminate (const Instr &instr, State &state, uint32_t block)
{
	SmallVector<uint32_t, 2> targets = branch_targets (code_, instr);
	std::optional<bool> taken;

	auto pop = [&] (Value &into) {
		if (state.stack.empty ())
			return false;

		into = state.stack.pop_back_val ();
		return true;
	};

	switch (instr.op) {
	case MONO_CEE_BR:
	case MONO_CEE_BR_S:
		return reach (targets[0], state);

	case MONO_CEE_LEAVE:
	case MONO_CEE_LEAVE_S:
		// Intervening finally handlers may update mutable slots.
		state.stack.clear ();
		std::fill (state.locals.begin (), state.locals.end (), Value ());
		if (writes_args_)
			std::fill (state.args.begin (), state.args.end (), Value ());
		return reach (targets[0], state);

	case MONO_CEE_BRTRUE:
	case MONO_CEE_BRTRUE_S:
	case MONO_CEE_BRFALSE:
	case MONO_CEE_BRFALSE_S: {
		Value value;

		if (!pop (value))
			return false;

		bool on_true = instr.op == MONO_CEE_BRTRUE || instr.op == MONO_CEE_BRTRUE_S;
		std::optional<bool> is_true = truth (value);

		if (is_true)
			taken = *is_true == on_true;
		break;
	}

	case MONO_CEE_SWITCH: {
		Value value;

		if (!pop (value))
			return false;

		if (value.is (Value::Kind::integer)) {
			uint64_t index = static_cast<uint32_t> (value.integer);

			decided_[block] = true;
			return index < targets.size () ? reach (targets[index], state)
			                               : reach (instr.next, state);
		}

		decided_[block] = false;
		for (uint32_t target : targets)
			if (!reach (target, state))
				return false;
		return reach (instr.next, state);
	}

	default: {
		Value b, a;
		std::optional<Compare> compare = compare_of (instr.op);

		if (!compare || !pop (b) || !pop (a))
			return false;

		taken = evaluate_compare (*compare, a, b, sharing_);
		break;
	}
	}

	decided_[block] = taken.has_value ();

	if (taken)
		return reach (*taken ? targets[0] : instr.next, state);

	return reach (targets[0], state) && reach (instr.next, state);
}

bool
ILAnalyzer::evaluate (uint32_t index)
{
	Block &block = blocks_[index];
	State state = *block.in;

	if (!reach_handlers_of (block))
		return false;

	for (uint32_t i = block.first; i < block.end; ++i) {
		const Instr &instr = instrs_[i];

		if (is_branch (instr)) {
			bool ok = terminate (instr, state, index);

			// Do not charge conditional branches that become unconditional.
			skip_bytes_[i] = ok && decided_[index];
			return ok;
		}
		if (!falls_through (instr))
			return true;

		pushed_call_result_ = false;

		if (!visit (instr, state))
			return false;

		bool is_call = instr.op == MONO_CEE_CALL || instr.op == MONO_CEE_CALLVIRT
		               || instr.op == MONO_CEE_NEWOBJ;
		bool pushed = is_call ? pushed_call_result_ : stack_effects[instr.op].pushes == 1;

		skip_bytes_[i] = pushed && !state.stack.empty () && state.stack.back ().is_constant ();
	}

	// Fall through to the next block, if one exists.
	return reach (block.next_offset, state);
}

std::optional<ILReachability>
ILAnalyzer::run ()
{
	if (method_->wrapper_type != MONO_WRAPPER_NONE)
		return std::nullopt;

	if (!decode_all () || instrs_.empty () || !build_blocks ())
		return std::nullopt;

	State entry;

	if (!entry_state (entry))
		return std::nullopt;

	scan_addresses ();

	if (!reach_block (0, entry))
		return std::nullopt;

	while (!worklist_.empty ()) {
		uint32_t index = worklist_.back ();

		worklist_.pop_back ();
		blocks_[index].queued = false;

		if (!evaluate (index))
			return std::nullopt;
	}

	ILReachability result { 0, 0 };

	for (size_t i = 0; i < blocks_.size (); ++i) {
		if (!blocks_[i].in)
			continue;

		if (decided_[i])
			++result.decided_branches;

		for (uint32_t k = blocks_[i].first; k < blocks_[i].end; ++k)
			if (!skip_bytes_[k])
				result.live_bytes += instrs_[k].next - instrs_[k].offset;
	}

	return result;
}

} // namespace

std::optional<ILReachability>
analyze_il_reachability (MonoMethod *method, MonoMethodHeader *header)
{
	return ILAnalyzer (method, header).run ();
}

} // namespace mono

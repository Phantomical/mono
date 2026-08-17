#ifndef __MONO_INTERP_TRANSFORM_H__
#define __MONO_INTERP_TRANSFORM_H__
#include <mono/mini/mini-runtime.h>
#include <mono/metadata/seq-points-data.h>
#include "interp-internals.hpp"

#include <cstdint>

#define INTERP_INST_FLAG_SEQ_POINT_NONEMPTY_STACK 1
#define INTERP_INST_FLAG_SEQ_POINT_METHOD_ENTRY 2
#define INTERP_INST_FLAG_SEQ_POINT_METHOD_EXIT 4
#define INTERP_INST_FLAG_SEQ_POINT_NESTED_CALL 8

#define INTERP_LOCAL_FLAG_DEAD 1
#define INTERP_LOCAL_FLAG_EXECUTION_STACK 2
#define INTERP_LOCAL_FLAG_CALL_ARGS 4

namespace mono::interp {

struct InterpInst;
struct InterpBasicBlock;

/// What the evaluation stack holds a value as. Wider than MintType, which says
/// how a value is stored: everything shorter than four bytes is on the stack as
/// I4.
enum class StackType : std::uint8_t {
	I4 = 0,
	I8 = 1,
	R4 = 2,
	R8 = 3,
	O = 4,
	VT = 5,
	MP = 6,
	F = 7,

	/// A native int, and so an alias for whichever of I4 and I8 that is.
#if SIZEOF_VOID_P == 8
	I = I8,
#else
	I = I4,
#endif
};

/// The member of an opcode family whose entries run I4, I8, R4, R8 in the order
/// StackType names them. base is the I4 member.
constexpr int
op_for_stack_type (int base, StackType type)
{
	return base + (int) type - (int) StackType::I4;
}

/// The member of an opcode family whose entries run I1, U1, I2, U2, I4, I8, R4,
/// R8, O, VT in the order MintType names them. base is the I1 member.
constexpr int
op_for_mint_type (int base, MintType type)
{
	return base + (int) type;
}

/// Which member of such a family op is. base is the I1 member.
constexpr MintType
mint_type_of_op (int base, int op)
{
	return (MintType) (op - base);
}

struct StackInfo {
	MonoClass *klass;
	StackType type;
	unsigned char flags;
	/*
	 * The local associated with the value of this stack entry. Every time we push on
	 * the stack a new local is created.
	 */
	int local;
	/* The offset from the execution stack start where this is stored */
	int offset;
	/* Saves how much stack this is using. It is a multiple of MINT_VT_ALIGNMENT */
	int size;
};

#define LOCAL_VALUE_NONE 0
#define LOCAL_VALUE_LOCAL 1
#define LOCAL_VALUE_I4 2
#define LOCAL_VALUE_I8 3

// LocalValue contains data to construct an InterpInst that is equivalent with the contents
// of the stack slot / local / argument.
struct LocalValue {
	// Indicates the type of the stored information. It can be another local or a constant
	int type;
	// Holds the local index or the actual constant value
	union {
		int local;
		gint32 i;
		gint64 l;
	};
	// The instruction that writes this local.
	InterpInst *ins;
	int def_index;
};

struct InterpInst {
	guint16 opcode;
	InterpInst *next, *prev;
	// If this is -1, this instruction is not logically associated with an IL offset, it is
	// part of the IL instruction associated with the previous interp instruction.
	int il_offset;
	guint32 flags;
	gint32 dreg;
	gint32 sregs [3]; // Currently all instructions have at most 3 sregs
	// This union serves the same purpose as the data array. The difference is that
	// the data array maps exactly to the final representation of the instruction.
	// FIXME We should consider using a separate higher level IR, that is also easier
	// to use for various optimizations.
	union {
		InterpBasicBlock *target_bb;
		InterpBasicBlock **target_bb_table;
		// We handle newobj poorly due to not having our own local offset allocator.
		// We temporarily use this array to let cprop know the values of the newobj args.
		int *newobj_reg_map;
	} info;
	// Variable data immediately following the dreg/sreg information. This is represented exactly
	// in the final code stream as in this array.
	guint16 data [MONO_ZERO_LEN_ARRAY];
};

struct InterpBasicBlock {
	guint8 *ip;
	GSList *seq_points;
	SeqPoint *last_seq_point;

	InterpInst *first_ins, *last_ins;
	/* Next bb in IL order */
	InterpBasicBlock *next_bb;

	gint16 in_count;
	InterpBasicBlock **in_bb;
	gint16 out_count;
	InterpBasicBlock **out_bb;

	int native_offset;

	/*
	 * The state of the stack when entering this basic block. By default, the stack height is
	 * -1, which means it inherits the stack state from the previous instruction, in IL order
	 */
	int stack_height;
	StackInfo *stack_state;

	int index;

	// This will hold a list of last sequence points of incoming basic blocks
	SeqPoint **pred_seq_points;
	guint num_pred_seq_points;

	// This block has special semantics and it shouldn't be optimized away
	bool eh_block : 1;
	bool dead : 1;
};

enum RelocType {
	RELOC_SHORT_BRANCH,
	RELOC_LONG_BRANCH,
	RELOC_SWITCH
};

struct Reloc {
	RelocType type;
	/* For branch relocation, how many sreg slots to skip */
	int skip;
	/* In the interpreter IR */
	int offset;
	InterpBasicBlock *target_bb;
};

struct InterpLocal {
	MonoType *type;
	MintType mt;
	int flags;
	int indirects;
	int offset;
	int size;
	union {
		// the offset from the start of the execution stack locals space
		int stack_offset;
	};
};

struct TransformData {
	MonoMethod *method;
	MonoMethod *inlined_method;
	MonoMethodHeader *header;
	InterpMethod *rtm;
	const unsigned char *il_code;
	const unsigned char *ip;
	const unsigned char *in_start;
	InterpInst *last_ins;
	int code_size;
	int *in_offsets;
	int current_il_offset;
	unsigned short *new_code;
	unsigned short *new_code_end;
	unsigned int max_code_size;
	StackInfo *stack;
	StackInfo *sp;
	unsigned int max_stack_height;
	unsigned int stack_capacity;
	unsigned int max_stack_size;
	unsigned int total_locals_size;
	InterpLocal *locals;
	unsigned int il_locals_offset;
	unsigned int il_locals_size;
	unsigned int locals_size;
	unsigned int locals_capacity;
	int n_data_items;
	int max_data_items;
	void **data_items;
	GHashTable *data_hash;
	int *clause_indexes;
	gboolean gen_sdb_seq_points;
	GPtrArray *seq_points;
	InterpBasicBlock **offset_to_bb;
	InterpBasicBlock *entry_bb, *cbb;
	int bb_count;
	MonoMemPool     *mempool;
	MonoMemoryManager *mem_manager;
	GList *basic_blocks;
	GPtrArray *relocs;
	gboolean verbose_level;
	GArray *line_numbers;
	gboolean prof_coverage;
	MonoProfilerCoverageInfo *coverage_info;
	GList *dont_inline;
	int inline_depth;
	bool has_localloc : 1;
};

} // namespace mono::interp

/* test exports for white box testing */
void
mono_test_interp_cprop (mono::interp::TransformData *td);
gboolean
mono_test_interp_generate_code (mono::interp::TransformData *td, MonoMethod *method, MonoMethodHeader *header, MonoGenericContext *generic_context, MonoError *error);
void
mono_test_interp_method_compute_offsets (mono::interp::TransformData *td, InterpMethod *imethod, MonoMethodSignature *signature, MonoMethodHeader *header);

/* debugging aid */
void
mono_interp_print_td_code (mono::interp::TransformData *td);

#endif /* __MONO_INTERP_TRANSFORM_H__ */

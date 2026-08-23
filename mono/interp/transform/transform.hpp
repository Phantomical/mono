#ifndef __MONO_INTERP_TRANSFORM_H__
#define __MONO_INTERP_TRANSFORM_H__
#include <mono/mini/mini-runtime.h>
#include <mono/metadata/seq-points-data.h>
#include "arena.hpp"
#include "runtime/internals.hpp"

#include <cstddef>
#include <cstdint>
#include <iterator>
#include <unordered_map>
#include <vector>
#include <optional>

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

/// The type of a value on the evaluation stack. Wider than MintType, which
/// says how a value is stored: everything shorter than four bytes is on the
/// stack as I4.
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

/// Returns the member of an opcode family whose entries run I4, I8, R4, R8 in
/// the order StackType names them. base is the I4 member.
constexpr int
op_for_stack_type (int base, StackType type)
{
	return base + (int) type - (int) StackType::I4;
}

/// Returns the member of an opcode family whose entries run I1, U1, I2, U2,
/// I4, I8, R4, R8, O, VT in the order MintType names them. base is the I1
/// member.
constexpr int
op_for_mint_type (int base, MintType type)
{
	return base + (int) type;
}

/// Returns which member of the op_for_mint_type () family op is, given base,
/// its I1 member.
constexpr MintType
mint_type_of_op (int base, int op)
{
	return (MintType) (op - base);
}

struct StackInfo {
	MonoClass *klass;
	StackType type;
	unsigned char flags;
	// The interp local backing this stack entry's value. Each push creates a
	// new one.
	int local;
	// The offset from the execution stack start where this is stored.
	int offset;
	// How much stack this is using, a multiple of MINT_VT_ALIGNMENT.
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

/// A range over an intrusive list, linked through the `Next` member.
///
/// The iterator reads that member when it advances, so a walk can retire the
/// node it is on - an instruction turned into a MINT_NOP stays linked - but
/// must not unlink or relink one.
template<class T, T *T::*Next>
class IntrusiveList {
public:
	class iterator {
	public:
		using iterator_category = std::forward_iterator_tag;
		using value_type = T *;
		using difference_type = std::ptrdiff_t;
		using pointer = T **;
		using reference = T *&;

		iterator () = default;
		explicit iterator (T *node) : node_ (node) {}

		T *operator* () const { return node_; }

		iterator &operator++ ()
		{
			node_ = node_->*Next;
			return *this;
		}

		bool operator== (const iterator &other) const { return node_ == other.node_; }
		bool operator!= (const iterator &other) const { return node_ != other.node_; }

	private:
		T *node_ = nullptr;
	};

	explicit IntrusiveList (T *head) : head_ (head) {}

	iterator begin () const { return iterator (head_); }
	iterator end () const { return iterator (nullptr); }

private:
	T *head_;
};

struct InterpInst {
	guint16 opcode;
	InterpInst *next, *prev;
	// If this is -1, this instruction is not logically associated with an IL offset, it is
	// part of the IL instruction associated with the previous interp instruction.
	int il_offset;
	guint32 flags;
	gint32 dreg;
	gint32 sregs[3]; // Currently all instructions have at most 3 sregs
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
	guint16 data[MONO_ZERO_LEN_ARRAY];
};

struct InterpBasicBlock {
	guint8 *ip;
	GSList *seq_points;
	SeqPoint *last_seq_point;

	InterpInst *first_ins, *last_ins;
	// Next bb in IL order.
	InterpBasicBlock *next_bb;

	gint16 in_count;
	InterpBasicBlock **in_bb;
	gint16 out_count;
	InterpBasicBlock **out_bb;

	int native_offset;

	// The state of the stack when entering this basic block. By default, the
	// stack height is -1, which means it inherits the stack state from the
	// previous instruction, in IL order.
	int stack_height;
	StackInfo *stack_state;

	int index;

	// This will hold a list of last sequence points of incoming basic blocks
	SeqPoint **pred_seq_points;
	guint num_pred_seq_points;

	// This block has special semantics and it shouldn't be optimized away
	bool eh_block : 1;
	bool dead : 1;

	/// The instructions in this block, in order.
	IntrusiveList<InterpInst, &InterpInst::next>::iterator begin () const
	{
		return IntrusiveList<InterpInst, &InterpInst::next>::iterator (first_ins);
	}

	IntrusiveList<InterpInst, &InterpInst::next>::iterator end () const
	{
		return IntrusiveList<InterpInst, &InterpInst::next>::iterator (nullptr);
	}
};

/// The basic blocks from bb onwards, in IL order.
inline IntrusiveList<InterpBasicBlock, &InterpBasicBlock::next_bb>
blocks_from (InterpBasicBlock *bb)
{
	return IntrusiveList<InterpBasicBlock, &InterpBasicBlock::next_bb> (bb);
}

/// The instructions from ins onwards, to the end of its block.
inline IntrusiveList<InterpInst, &InterpInst::next>
instructions_from (InterpInst *ins)
{
	return IntrusiveList<InterpInst, &InterpInst::next> (ins);
}

enum RelocType {
	RELOC_SHORT_BRANCH,
	RELOC_LONG_BRANCH,
	RELOC_SWITCH
};

struct Reloc {
	RelocType type;
	// For branch relocation, how many sreg slots to skip.
	int skip;
	// Offset into the compacted code (new_code) that the displacement is
	// measured from. A branch reloc points at the instruction's first word,
	// and the displacement is written skip + 1 words later. A switch reloc
	// points at the label slot itself.
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

/// One method's trip through the transform, and everything that trip allocates.
///
/// No member of this struct outlives the transform except what it copies into
/// the InterpMethod, so construction and destruction are the whole memory
/// discipline: the arena and the members below go together.
struct TransformData {
	TransformData (MonoMethod *method, MonoMethodHeader *header, InterpMethod *rtm);
	~TransformData ();

	TransformData (const TransformData &) = delete;
	TransformData &operator= (const TransformData &) = delete;

	// The transform, defined in transform.cpp.
	void alloc_ins_locals (InterpInst *ins);
	void binary_arith_op (int mint_op);
	void cannot_share (const char *what);
	void coerce_fp (StackInfo *sp, std::optional<StackType> dtype);
	void collect_pred_seq_points (InterpBasicBlock *bb, SeqPoint *seqp, GSList **next);
	int create_interp_local (MonoType *type);
	int create_interp_local_explicit (MonoType *type, int size);
	int create_interp_stack_local (StackType type, MonoClass *k, int type_size, int offset);
	guint16 *emit_compacted_instruction (guint16 *start_ip, InterpBasicBlock *bb, InterpInst *ins);
	void emit_convert (MonoType *ftype);
	/// Emits the address of a static field of a class the generic context
	/// names, into dreg, and returns whether it did.
	///
	/// Records a refusal and returns false for a special static, which is
	/// allocated an offset no rgctx entry holds.
	bool emit_static_field_address (MonoClassField *field, int dreg);
	/// Emits the fetch of one runtime generic context entry, and returns the
	/// local it lands in.
	///
	/// Returns -1 with a refusal recorded when this body has no context to read
	/// the entry out of, so a caller checks sharing_refusal before it goes on.
	int emit_rgctx_fetch (MonoRgctxInfoType info_type, gpointer data);
	void fixup_newbb_stack_locals (InterpBasicBlock *newbb);
	gboolean generate_code (MonoMethod *method, MonoMethodHeader *header,
	                        MonoGenericContext *generic_context, MonoError *error);
	void generate_compacted_code ();
	MonoType *get_arg_type_exact (int n, MintType *mt);
	void get_basic_blocks (MonoMethodHeader *header, gboolean make_list);
	InterpBasicBlock *get_bb (unsigned char *ip, gboolean make_list);
	guint16 get_data_item_index (void *ptr);
	guint16 get_data_item_index_nonshared (void *ptr);
	int get_interp_local_offset (int local, gboolean resolve_stack_locals);
	int get_native_offset (int il_offset);
	int get_tos_offset ();
	void handle_branch (int short_op, int long_op, int offset);
	void handle_ldelem (int op, StackType type);
	void handle_ldind (int op, StackType type, gboolean *volatile_);
	void handle_relocations ();
	void handle_stelem (int op);
	void handle_stind (int op, gboolean *volatile_);
	void init_bb_stack_state (InterpBasicBlock *bb);
	void initialize_clause_bblocks ();
	void interp_add_conv (StackInfo *sp, InterpInst *prev_ins, StackType type, int conv_op);
	InterpInst *interp_add_ins (guint16 opcode);
	InterpInst *interp_add_ins_explicit (guint16 opcode, int len);
	void interp_constrained_box (MonoDomain *domain, MonoClass *constrained_class,
	                             MonoMethodSignature *csignature, MonoError *error);
	void interp_cprop ();
	void interp_emit_ldelema (MonoClass *array_class, MonoClass *check_class);
	void interp_emit_ldobj (MonoClass *klass);
	void interp_emit_ldsflda (MonoClassField *field, MonoError *error);
	void interp_emit_ldsflda_dyn (MonoClassField *field);
	gboolean interp_emit_load_const (gpointer field_addr, MintType mt);
	void interp_emit_memory_barrier (int kind);
	void interp_emit_sfld_access (MonoClassField *field, MonoClass *field_class, MintType mt,
	                              gboolean is_load, MonoError *error);
	/// Writes a static field access of a class the generic context names,
	/// through the vtable the context answers with.
	///
	/// Records a refusal for the shapes that name more than that vtable: a
	/// special static, and a store of a value type the context names.
	void interp_emit_sfld_access_dyn (MonoClassField *field, MonoClass *field_class, MintType mt,
	                                  gboolean is_load);
	void interp_emit_stobj (MonoClass *klass);
	void interp_fix_localloc_ret ();
	InterpInst *interp_fold_binop (LocalValue *local_defs, int *local_ref_count, InterpInst *ins);
	InterpInst *interp_fold_binop_cond_br (InterpBasicBlock *cbb, LocalValue *local_defs,
	                                       int *local_ref_count, InterpInst *ins);
	InterpInst *interp_fold_unop (LocalValue *local_defs, int *local_ref_count, InterpInst *ins);
	InterpInst *interp_fold_unop_cond_br (InterpBasicBlock *cbb, LocalValue *local_defs,
	                                      int *local_ref_count, InterpInst *ins);
	void interp_generate_bie_throw ();
	void interp_generate_ipe_throw_with_msg (MonoError *error_msg);
	void interp_generate_mae_throw (MonoMethod *method, MonoMethod *target_method);
	void interp_generate_not_supported_throw ();
	InterpInst *interp_get_ldc_i4_from_const (InterpInst *ins, gint32 ct, int dreg);
	gboolean interp_handle_intrinsics (MonoMethod *target_method, MonoClass *constrained_class,
	                                   MonoMethodSignature *csignature, gboolean readonly, int *op);
	void interp_handle_isinst (MonoClass *klass, gboolean isinst_instr);
	void interp_handle_isinst_dyn (int klass_local, MonoClass *klass, gboolean isinst_instr);
	gboolean interp_handle_magic_type_intrinsics (MonoMethod *target_method,
	                                              MonoMethodSignature *csignature, int type_index);
	gboolean interp_inline_method (MonoMethod *target_method, MonoMethodHeader *header,
	                               MonoError *error);
	InterpInst *interp_insert_ins (InterpInst *prev_ins, guint16 opcode);
	InterpInst *interp_insert_ins_bb (InterpBasicBlock *bb, InterpInst *prev_ins, guint16 opcode);
	InterpInst *interp_inst_replace_with_i8_const (InterpInst *ins, gint64 ct);
	gboolean interp_ip_in_cbb (int il_offset);
	void interp_link_bblocks (InterpBasicBlock *from, InterpBasicBlock *to);
	gboolean interp_local_deadce (int *local_ref_count);
	void interp_merge_bblocks (InterpBasicBlock *bb, InterpBasicBlock *bbadd);
	gboolean interp_method_check_inlining (MonoMethod *method, MonoMethodSignature *csignature);
	void interp_method_compute_offsets (InterpMethod *imethod, MonoMethodSignature *sig,
	                                    MonoMethodHeader *header, MonoError *error);
	InterpInst *interp_new_ins (guint16 opcode, int len);
	gboolean interp_optimize_bblocks ();
	void interp_optimize_code ();
	void interp_remove_bblock (InterpBasicBlock *bb, InterpBasicBlock *prev_bb);
	void interp_save_debug_info (InterpMethod *rtm, MonoMethodHeader *header,
	                             const std::vector<MonoDebugLineNumberEntry> &line_numbers);
	void interp_save_line_numbers (InterpMethod *rtm,
	                               const std::vector<MonoDebugLineNumberEntry> &line_numbers);
	gboolean interp_transform_call (MonoMethod *method, MonoMethod *target_method,
	                                MonoDomain *domain, MonoGenericContext *generic_context,
	                                MonoClass *constrained_class, gboolean readonly,
	                                MonoError *error, gboolean check_visibility,
	                                gboolean save_last_error, gboolean tailcall);
	void load_arg (int n);
	void load_local (int local);
	void mark_bb_as_dead (InterpBasicBlock *bb);
	/// Whether a field access of a class the generic context names can be
	/// written once for every reference instantiation, recording the refusal
	/// when it cannot.
	///
	/// A plain instance arm can. It names the field's offset and its size, and
	/// reference sharing keeps both common. So can a static access outside an
	/// inlined callee, which the caller then writes through
	/// interp_emit_sfld_access_dyn ().
	bool may_share_field_access (MonoClass *klass, gboolean is_static, bool inlining);
	/// Whether a call to a method the generic context names can be written as a
	/// fetch of the callee's InterpMethod and a call through it, recording the
	/// refusal when it cannot.
	///
	/// body is the method whose IL is being read, which is this->method except
	/// while a callee is inlined.
	bool may_call_through_context (MonoMethod *body, MonoMethod *target,
	                               MonoMethodSignature *csignature, gboolean is_virtual,
	                               gboolean tailcall);
	void narrow_index (StackInfo *sp);
	void one_arg_branch (int mint_op, int offset, int inst_size);
	void push_simple_type (StackType type);
	void push_type (StackType type, MonoClass *k);
	void push_type_explicit (StackType type, MonoClass *k, int type_size);
	void push_type_vt (MonoClass *k, int size);
	void push_types (StackInfo *types, int count);
	void realloc_stack ();
	void recursively_make_pred_seq_points (InterpBasicBlock *bb);
	/// Resolves a class token, and records a refusal to share when the class
	/// depends on the generic context.
	///
	/// A caller that can instead read the class out of the context at run time
	/// passes from_context, which is set rather than the refusal being taken.
	/// The class returned is then the shared form's, so it describes the site
	/// and never the instantiation.
	///
	/// A site that answers with a fetch passes NULL while inlining. A fetch
	/// reads the receiver of the body being written, which is the caller's
	/// rather than the callee's, so it would answer for the wrong generic
	/// context. A site that only needs a size or a kind is free of this,
	/// because reference sharing keeps those common.
	MonoClass *resolve_class (MonoMethod *method, guint32 token,
	                          MonoGenericContext *generic_context,
	                          bool *from_context = nullptr);
	/// Resolves a field token, and records a refusal to share when the field's
	/// class depends on the generic context.
	///
	/// A caller that can write the access once for every instantiation passes
	/// from_context, which is set rather than the refusal being taken. It then
	/// asks may_share_field_access () once it knows which arm it takes.
	MonoClassField *resolve_field (MonoMethod *method, guint32 token, MonoClass **klass,
	                               MonoGenericContext *generic_context, MonoError *error,
	                               bool *from_context = nullptr);
	void save_seq_points (MonoJitInfo *jinfo);
	void set_simple_type_and_local (StackInfo *sp, StackType type);
	void set_type_and_local (StackInfo *sp, MonoClass *klass, StackType type);
	void shift_op (int mint_op);
	void store_arg (int n);
	void store_local (int local);
	void two_arg_branch (int mint_op, int offset, int inst_size);
	void unary_arith_op (int mint_op);
	void widen_i4_to_i8 (StackInfo *sp, MonoType *type);

	/// Why this body cannot serve every reference instantiation, or NULL while
	/// it still can. Only a transform of a shared form fills it in, and the
	/// first reason is the one kept.
	const char *sharing_refusal = nullptr;

	/// Whether this transform is of a shared form. It is what makes a site that
	/// names the generic context a refusal rather than an ordinary resolution,
	/// so a transform of a concrete instantiation asks no such question.
	bool sharing = false;

	/// The local holding the receiver a fetch reads the generic context out of,
	/// or -1 in a body with no such receiver.
	int rgctx_receiver_local = -1;

	/// The rgctx entries already fetched in rgctx_fetched_bb, as slot index and
	/// the local the fetch landed in.
	///
	/// The block is part of the key because a local written in one block is not
	/// written in another. A block reached from two directions would read a
	/// local only one of them defined.
	InterpBasicBlock *rgctx_fetched_bb = nullptr;
	std::vector<std::pair<int, int>> rgctx_fetched;

	MonoMethod *method = nullptr;
	MonoMethod *inlined_method = nullptr;
	MonoMethodHeader *header = nullptr;
	InterpMethod *rtm = nullptr;
	const unsigned char *il_code = nullptr;
	const unsigned char *ip = nullptr;
	const unsigned char *in_start = nullptr;
	InterpInst *last_ins = nullptr;
	int code_size = 0;
	int *in_offsets = nullptr;
	int current_il_offset = -1;
	unsigned short *new_code = nullptr;
	unsigned short *new_code_end = nullptr;
	unsigned int max_code_size = 0;
	StackInfo *stack = nullptr;
	StackInfo *sp = nullptr;
	unsigned int max_stack_height = 0;
	unsigned int stack_capacity = 0;
	unsigned int max_stack_size = 0;
	unsigned int total_locals_size = 0;
	std::vector<InterpLocal> locals;
	unsigned int il_locals_offset = 0;
	unsigned int il_locals_size = 0;
	std::vector<gpointer> data_items;
	/// Where a pointer already in data_items sits, so that a second reference
	/// to it reuses the slot.
	std::unordered_map<gpointer, guint16> data_hash;
	int *clause_indexes = nullptr;
	gboolean gen_sdb_seq_points = FALSE;
	std::vector<SeqPoint *> seq_points;
	InterpBasicBlock **offset_to_bb = nullptr;
	InterpBasicBlock *entry_bb = nullptr, *cbb = nullptr;
	int bb_count = 0;
	Arena arena;
	MonoMemoryManager *mem_manager = nullptr;
	std::vector<InterpBasicBlock *> basic_blocks;
	std::vector<Reloc *> relocs;
	gboolean verbose_level = 0;
	std::vector<MonoDebugLineNumberEntry> line_numbers;
	gboolean prof_coverage = FALSE;
	MonoProfilerCoverageInfo *coverage_info = nullptr;
	/// The methods on the inlining path right now, innermost last. A method
	/// already here is not an inline candidate.
	std::vector<MonoMethod *> dont_inline;
	int inline_depth = 0;
	bool has_localloc : 1;
};

} // namespace mono::interp

/* test exports for white box testing */
void mono_test_interp_cprop (mono::interp::TransformData *td);
gboolean mono_test_interp_generate_code (mono::interp::TransformData *td, MonoMethod *method,
                                         MonoMethodHeader *header,
                                         MonoGenericContext *generic_context, MonoError *error);
void mono_test_interp_method_compute_offsets (mono::interp::TransformData *td,
                                              InterpMethod *imethod, MonoMethodSignature *signature,
                                              MonoMethodHeader *header);

/* debugging aid */
void mono_interp_print_td_code (mono::interp::TransformData *td);

#endif /* __MONO_INTERP_TRANSFORM_H__ */

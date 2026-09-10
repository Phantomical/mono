/**
 * \file
 * \brief The classic compiler's IR and pipeline state.
 *
 * What both engines share stays in mini.h.
 */

#ifndef __MONO_MINI_TIER0_COMPILE_H__
#define __MONO_MINI_TIER0_COMPILE_H__

#include "mini.h"
#include "tier0-arch.h"
#include "regalloc.h"

#ifndef __GNUC__
/*#define __alignof__(a) sizeof(a)*/
#define __alignof__(type) G_STRUCT_OFFSET(struct { char c; type x; }, x)
#endif
#if DISABLE_LOGGING
#define MINI_DEBUG(level,limit,code)
#else
#define MINI_DEBUG(level,limit,code) do {if (G_UNLIKELY ((level) >= (limit))) code} while (0)
#endif
#if ENABLE_LLVM
#define COMPILE_LLVM(cfg) ((cfg)->compile_llvm)
#define LLVM_ENABLED TRUE
#else
#define COMPILE_LLVM(cfg) (0)
#define LLVM_ENABLED FALSE
#endif
#ifdef MONO_ARCH_SOFT_FLOAT_FALLBACK
#define COMPILE_SOFT_FLOAT(cfg) (!COMPILE_LLVM ((cfg)) && mono_arch_is_soft_float ())
#else
#define COMPILE_SOFT_FLOAT(cfg) (0)
#endif
/* for 32 bit systems */
#if G_BYTE_ORDER == G_LITTLE_ENDIAN
#define MINI_LS_WORD_IDX 0
#define MINI_MS_WORD_IDX 1
#else
#define MINI_LS_WORD_IDX 1
#define MINI_MS_WORD_IDX 0
#endif
#define MINI_LS_WORD_OFFSET (MINI_LS_WORD_IDX * 4)
#define MINI_MS_WORD_OFFSET (MINI_MS_WORD_IDX * 4)
#define MONO_LVREG_LS(lvreg)	((lvreg) + 1)
#define MONO_LVREG_MS(lvreg)	((lvreg) + 2)
//TODO: This is x86/amd64 specific.
#define mono_simd_shuffle_mask(a,b,c,d) ((a) | ((b) << 2) | ((c) << 4) | ((d) << 6))
//XXX this ignores if t is byref
#define MONO_TYPE_IS_PRIMITIVE_SCALAR(t) ((((((t)->type >= MONO_TYPE_BOOLEAN && (t)->type <= MONO_TYPE_U8) || ((t)->type >= MONO_TYPE_I && (t)->type <= MONO_TYPE_U)))))
typedef struct {
	MonoJitInfo *ji;
	MonoCodeManager *code_mp;
} MonoJitDynamicMethodInfo;
typedef struct {
	guint32 index;
	MonoExceptionClause *clause;
} MonoLeaveClause;
#if 0
#define mono_bitset_foreach_bit(set,b,n) \
	for (b = 0; b < n; b++)\
		if (mono_bitset_test_fast(set,b))
#else
#define mono_bitset_foreach_bit(set,b,n) \
	for (b = mono_bitset_find_start (set); b < n && b >= 0; b = mono_bitset_find_first (set, b))
#endif
#define MONO_VARINFO(cfg,varnum) (&(cfg)->vars [varnum])
#define MONO_INST_NULLIFY_SREGS(dest) do {				\
		(dest)->sreg1 = (dest)->sreg2 = (dest)->sreg3 = -1;	\
	} while (0)
#define MONO_INST_NEW(cfg,dest,op) do {	\
		(dest) = (MonoInst *)mono_mempool_alloc0 ((cfg)->mempool, sizeof (MonoInst));	\
		(dest)->opcode = (op);	\
		(dest)->dreg = -1;			    \
		MONO_INST_NULLIFY_SREGS ((dest));	    \
        (dest)->cil_code = (cfg)->ip;  \
	} while (0)
#define MONO_INST_NEW_CALL(cfg,dest,op) do {	\
		(dest) = (MonoCallInst *)mono_mempool_alloc0 ((cfg)->mempool, sizeof (MonoCallInst));	\
		(dest)->inst.opcode = (op);	\
		(dest)->inst.dreg = -1;					\
		MONO_INST_NULLIFY_SREGS (&(dest)->inst);		\
        (dest)->inst.cil_code = (cfg)->ip;  \
	} while (0)
#define MONO_ADD_INS(b,inst) do {	\
		if ((b)->last_ins) {	\
			(b)->last_ins->next = (inst);	\
            (inst)->prev = (b)->last_ins;   \
			(b)->last_ins = (inst);	\
		} else {	\
			(b)->code = (b)->last_ins = (inst);	\
		}	\
	} while (0)
#define NULLIFY_INS(ins) do { \
        (ins)->opcode = OP_NOP; \
        (ins)->dreg = -1;				\
	MONO_INST_NULLIFY_SREGS ((ins));		\
    } while (0)
/* Remove INS from BB */
#define MONO_REMOVE_INS(bb,ins) do { \
        if ((ins)->prev) \
            (ins)->prev->next = (ins)->next; \
        if ((ins)->next) \
            (ins)->next->prev = (ins)->prev; \
        if ((bb)->code == (ins)) \
            (bb)->code = (ins)->next; \
        if ((bb)->last_ins == (ins)) \
            (bb)->last_ins = (ins)->prev; \
    } while (0)
/* Remove INS from BB and nullify it */
#define MONO_DELETE_INS(bb,ins) do { \
        MONO_REMOVE_INS ((bb), (ins)); \
        NULLIFY_INS ((ins)); \
    } while (0)
/* 
 * this is used to determine when some branch optimizations are possible: we exclude FP compares
 * because they have weird semantics with NaNs.
 */
#define MONO_IS_COND_BRANCH_OP(ins) (((ins)->opcode >= OP_LBEQ && (ins)->opcode <= OP_LBLT_UN) || ((ins)->opcode >= OP_FBEQ && (ins)->opcode <= OP_FBLT_UN) || ((ins)->opcode >= OP_IBEQ && (ins)->opcode <= OP_IBLT_UN))
#define MONO_IS_COND_BRANCH_NOFP(ins) (MONO_IS_COND_BRANCH_OP(ins) && !(((ins)->opcode >= OP_FBEQ) && ((ins)->opcode <= OP_FBLT_UN)))
#define MONO_IS_BRANCH_OP(ins) (MONO_IS_COND_BRANCH_OP(ins) || ((ins)->opcode == OP_BR) || ((ins)->opcode == OP_BR_REG) || ((ins)->opcode == OP_SWITCH))
#define MONO_IS_COND_EXC(ins) ((((ins)->opcode >= OP_COND_EXC_EQ) && ((ins)->opcode <= OP_COND_EXC_LT_UN)) || (((ins)->opcode >= OP_COND_EXC_IEQ) && ((ins)->opcode <= OP_COND_EXC_ILT_UN)))
#define MONO_IS_SETCC(ins) ((((ins)->opcode >= OP_CEQ) && ((ins)->opcode <= OP_CLT_UN)) || (((ins)->opcode >= OP_ICEQ) && ((ins)->opcode <= OP_ICLE_UN)) || (((ins)->opcode >= OP_LCEQ) && ((ins)->opcode <= OP_LCLT_UN)) || (((ins)->opcode >= OP_FCEQ) && ((ins)->opcode <= OP_FCLT_UN)))
#define MONO_HAS_CUSTOM_EMULATION(ins) (((ins)->opcode >= OP_FBEQ && (ins)->opcode <= OP_FBLT_UN) || ((ins)->opcode >= OP_FCEQ && (ins)->opcode <= OP_FCLT_UN))
#define MONO_IS_LOAD_MEMBASE(ins) (((ins)->opcode >= OP_LOAD_MEMBASE && (ins)->opcode <= OP_LOADV_MEMBASE) || ((ins)->opcode >= OP_ATOMIC_LOAD_I1 && (ins)->opcode <= OP_ATOMIC_LOAD_R8))
#define MONO_IS_STORE_MEMBASE(ins) (((ins)->opcode >= OP_STORE_MEMBASE_REG && (ins)->opcode <= OP_STOREV_MEMBASE) || ((ins)->opcode >= OP_ATOMIC_STORE_I1 && (ins)->opcode <= OP_ATOMIC_STORE_R8))
#define MONO_IS_STORE_MEMINDEX(ins) (((ins)->opcode >= OP_STORE_MEMINDEX) && ((ins)->opcode <= OP_STORER8_MEMINDEX))
// This is internal because it is easily confused with any enum or integer.
#define MONO_IS_TAILCALL_OPCODE_INTERNAL(opcode) ((opcode) == OP_TAILCALL || (opcode) == OP_TAILCALL_MEMBASE || (opcode) == OP_TAILCALL_REG)
#define MONO_IS_TAILCALL_OPCODE(call) (MONO_IS_TAILCALL_OPCODE_INTERNAL (call->inst.opcode))
// OP_DYN_CALL is not a MonoCallInst
#define MONO_IS_CALL(ins) (((ins)->opcode >= OP_VOIDCALL && (ins)->opcode <= OP_VCALL2_MEMBASE) || \
	MONO_IS_TAILCALL_OPCODE_INTERNAL ((ins)->opcode))
#define MONO_IS_JUMP_TABLE(ins) (((ins)->opcode == OP_JUMP_TABLE) ? TRUE : ((((ins)->opcode == OP_AOTCONST) && (ins->inst_i1 == (gpointer)MONO_PATCH_INFO_SWITCH)) ? TRUE : ((ins)->opcode == OP_SWITCH) ? TRUE : ((((ins)->opcode == OP_GOT_ENTRY) && ((ins)->inst_right->inst_i1 == (gpointer)MONO_PATCH_INFO_SWITCH)) ? TRUE : FALSE)))
#define MONO_JUMP_TABLE_FROM_INS(ins) (((ins)->opcode == OP_JUMP_TABLE) ? (ins)->inst_p0 : (((ins)->opcode == OP_AOTCONST) && (ins->inst_i1 == (gpointer)MONO_PATCH_INFO_SWITCH) ? (ins)->inst_p0 : (((ins)->opcode == OP_SWITCH) ? (ins)->inst_p0 : ((((ins)->opcode == OP_GOT_ENTRY) && ((ins)->inst_right->inst_i1 == (gpointer)MONO_PATCH_INFO_SWITCH)) ? (ins)->inst_right->inst_p0 : NULL))))
/* FIXME: Add more instructions */
/* INEG sets the condition codes, and the OP_LNEG decomposition depends on this on x86 */
#define MONO_INS_HAS_NO_SIDE_EFFECT(ins) (MONO_IS_MOVE (ins) || (ins->opcode == OP_ICONST) || (ins->opcode == OP_I8CONST) || MONO_IS_ZERO (ins) || (ins->opcode == OP_ADD_IMM) || (ins->opcode == OP_R8CONST) || (ins->opcode == OP_LADD_IMM) || (ins->opcode == OP_ISUB_IMM) || (ins->opcode == OP_IADD_IMM) || (ins->opcode == OP_LNEG) || (ins->opcode == OP_ISUB) || (ins->opcode == OP_CMOV_IGE) || (ins->opcode == OP_ISHL_IMM) || (ins->opcode == OP_ISHR_IMM) || (ins->opcode == OP_ISHR_UN_IMM) || (ins->opcode == OP_IAND_IMM) || (ins->opcode == OP_ICONV_TO_U1) || (ins->opcode == OP_ICONV_TO_I1) || (ins->opcode == OP_SEXT_I4) || (ins->opcode == OP_LCONV_TO_U1) || (ins->opcode == OP_ICONV_TO_U2) || (ins->opcode == OP_ICONV_TO_I2) || (ins->opcode == OP_LCONV_TO_I2) || (ins->opcode == OP_LDADDR) || (ins->opcode == OP_PHI) || (ins->opcode == OP_NOP) || (ins->opcode == OP_ZEXT_I4) || (ins->opcode == OP_NOT_NULL) || (ins->opcode == OP_IL_SEQ_POINT) || (ins->opcode == OP_XZERO))
#define MONO_INS_IS_PCONST_NULL(ins) ((ins)->opcode == OP_PCONST && (ins)->inst_p0 == 0)
/* Determine whenever 'ins' represents a load of the 'this' argument */
#define MONO_CHECK_THIS(ins) (mono_method_signature_internal (cfg->method)->hasthis && ((ins)->opcode == OP_MOVE) && ((ins)->sreg1 == cfg->args [0]->dreg))
#ifdef MONO_ARCH_SIMD_INTRINSICS
#define MONO_IS_PHI(ins) (((ins)->opcode == OP_PHI) || ((ins)->opcode == OP_FPHI) || ((ins)->opcode == OP_VPHI)  || ((ins)->opcode == OP_XPHI))
#define MONO_IS_MOVE(ins) (((ins)->opcode == OP_MOVE) || ((ins)->opcode == OP_FMOVE) || ((ins)->opcode == OP_VMOVE) || ((ins)->opcode == OP_XMOVE) || ((ins)->opcode == OP_RMOVE))
#define MONO_IS_NON_FP_MOVE(ins) (((ins)->opcode == OP_MOVE) || ((ins)->opcode == OP_VMOVE) || ((ins)->opcode == OP_XMOVE))
#define MONO_IS_REAL_MOVE(ins) (((ins)->opcode == OP_MOVE) || ((ins)->opcode == OP_FMOVE) || ((ins)->opcode == OP_XMOVE) || ((ins)->opcode == OP_RMOVE))
#define MONO_IS_ZERO(ins) (((ins)->opcode == OP_VZERO) || ((ins)->opcode == OP_XZERO))
#else
#define MONO_IS_PHI(ins) (((ins)->opcode == OP_PHI) || ((ins)->opcode == OP_FPHI) || ((ins)->opcode == OP_VPHI))
#define MONO_IS_MOVE(ins) (((ins)->opcode == OP_MOVE) || ((ins)->opcode == OP_FMOVE) || ((ins)->opcode == OP_VMOVE) || ((ins)->opcode == OP_RMOVE))
#define MONO_IS_NON_FP_MOVE(ins) (((ins)->opcode == OP_MOVE) || ((ins)->opcode == OP_VMOVE))
/*A real MOVE is one that isn't decomposed such as a VMOVE or LMOVE*/
#define MONO_IS_REAL_MOVE(ins) (((ins)->opcode == OP_MOVE) || ((ins)->opcode == OP_FMOVE) || ((ins)->opcode == OP_RMOVE))
#define MONO_IS_ZERO(ins) ((ins)->opcode == OP_VZERO)
#endif
#if defined(TARGET_X86) || defined(TARGET_AMD64)
#define EMIT_NEW_X86_LEA(cfg,dest,sr1,sr2,shift,imm) do { \
		MONO_INST_NEW (cfg, dest, OP_X86_LEA); \
		(dest)->dreg = alloc_ireg_mp ((cfg)); \
		(dest)->sreg1 = (sr1); \
		(dest)->sreg2 = (sr2); \
		(dest)->inst_imm = (imm); \
		(dest)->backend.shift_amount = (shift); \
		MONO_ADD_INS ((cfg)->cbb, (dest)); \
	} while (0)
#endif
typedef struct MonoInstList MonoInstList;
typedef struct MonoCallInst MonoCallInst;
typedef struct MonoCallArgParm MonoCallArgParm;
typedef struct MonoMethodVar MonoMethodVar;
typedef struct MonoSpillInfo MonoSpillInfo;

typedef struct _MonoGraphDumper MonoGraphDumper;
#define INS_INFO(opcode) (&mini_ins_info [((opcode) - OP_START - 1) * 4])
/* instruction description for use in regalloc/scheduling */
enum {
	MONO_INST_DEST = 0,
	MONO_INST_SRC1 = 1,             /* we depend on the SRCs to be consecutive */
	MONO_INST_SRC2 = 2,
	MONO_INST_SRC3 = 3,
	MONO_INST_LEN = 4,
	MONO_INST_CLOB = 5,
	/* Unused, commented out to reduce the size of the mdesc tables
	MONO_INST_FLAGS,
	MONO_INST_COST,
	MONO_INST_DELAY,
	MONO_INST_RES,
	*/
	MONO_INST_MAX = 6
};
typedef union MonoInstSpec { // instruction specification
	struct {
		char dest;
		char src1;
		char src2;
		char src3;
		unsigned char len;
		char clob;
		// char flags;
		// char cost;
		// char delay;
		// char res;
	};
	struct {
		char xdest;
		char src [3];
		unsigned char xlen;
		char xclob;
	};
	char bytes[MONO_INST_MAX];
} MonoInstSpec;
extern const char mini_ins_info[];
extern const gint8 mini_ins_sreg_counts [];
#ifndef DISABLE_JIT
#define mono_inst_get_num_src_registers(ins) (mini_ins_sreg_counts [(ins)->opcode - OP_START - 1])
#else
#define mono_inst_get_num_src_registers(ins) 0
#endif
#define mono_inst_get_src_registers(ins, regs) (((regs) [0] = (ins)->sreg1), ((regs) [1] = (ins)->sreg2), ((regs) [2] = (ins)->sreg3), mono_inst_get_num_src_registers ((ins)))
#define MONO_BB_FOR_EACH_INS(bb, ins) for ((ins) = (bb)->code; (ins); (ins) = (ins)->next)
#define MONO_BB_FOR_EACH_INS_SAFE(bb, n, ins) for ((ins) = (bb)->code, n = (ins) ? (ins)->next : NULL; (ins); (ins) = (n), (n) = (ins) ? (ins)->next : NULL)
#define MONO_BB_FOR_EACH_INS_REVERSE(bb, ins) for ((ins) = (bb)->last_ins; (ins); (ins) = (ins)->prev)
#define MONO_BB_FOR_EACH_INS_REVERSE_SAFE(bb, p, ins) for ((ins) = (bb)->last_ins, p = (ins) ? (ins)->prev : NULL; (ins); (ins) = (p), (p) = (ins) ? (ins)->prev : NULL)
#define mono_bb_first_ins(bb) (bb)->code
/*
 * Iterate through all used registers in the instruction.
 * Relies on the existing order of the MONO_INST enum: MONO_INST_{DREG,SREG1,SREG2,SREG3,LEN}
 * INS is the instruction, IDX is the register index, REG is the pointer to a register.
 */
#define MONO_INS_FOR_EACH_REG(ins, idx, reg) for ((idx) = INS_INFO ((ins)->opcode)[MONO_INST_DEST] != ' ' ? MONO_INST_DEST : \
							  (mono_inst_get_num_src_registers (ins) ? MONO_INST_SRC1 : MONO_INST_LEN); \
						  (reg) = (idx) == MONO_INST_DEST ? &(ins)->dreg : \
							  ((idx) == MONO_INST_SRC1 ? &(ins)->sreg1 : \
							   ((idx) == MONO_INST_SRC2 ? &(ins)->sreg2 : \
							    ((idx) == MONO_INST_SRC3 ? &(ins)->sreg3 : NULL))), \
							  idx < MONO_INST_LEN; \
						  (idx) = (idx) > mono_inst_get_num_src_registers (ins) + (INS_INFO ((ins)->opcode)[MONO_INST_DEST] != ' ') ? MONO_INST_LEN : (idx) + 1)
struct MonoSpillInfo {
	int offset;
};
/*
 * Information about a call site for the GC map creation code
 */
typedef struct {
	/* The next offset after the call instruction */
	int pc_offset;
	/* The basic block containing the call site */
	MonoBasicBlock *bb;
	/* 
	 * The set of variables live at the call site.
	 * Has length cfg->num_varinfo in bits.
	 */
	guint8 *liveness;
	/*
	 * List of OP_GC_PARAM_SLOT_LIVENESS_DEF instructions defining the param slots
	 * used by this call.
	 */
	GSList *param_slots;
} GCCallSite;
/*
 * The IR-level extended basic block.  
 *
 * A basic block can have multiple exits just fine, as long as the point of
 * 'departure' is the last instruction in the basic block. Extended basic
 * blocks, on the other hand, may have instructions that leave the block
 * midstream. The important thing is that they cannot be _entered_
 * midstream, ie, execution of a basic block (or extened bb) always start
 * at the beginning of the block, never in the middle.
 */
struct MonoBasicBlock {
	MonoInst *last_ins;
	/* the next basic block in the order it appears in IL */
	MonoBasicBlock *next_bb;
	/*
	 * Before instruction selection it is the first tree in the
	 * forest and the first item in the list of trees. After
	 * instruction selection it is the first instruction and the
	 * first item in the list of instructions.
	 */
	MonoInst *code;
	/* unique block number identification */
	gint32 block_num;
	
	gint32 dfn;
	/* Basic blocks: incoming and outgoing counts and pointers */
	/* Each bb should only appear once in each array */
	gint16 out_count, in_count;
	MonoBasicBlock **in_bb;
	MonoBasicBlock **out_bb;
	/* Points to the start of the CIL code that initiated this BB */
	unsigned char* cil_code;
	/* Length of the CIL block */
	gint32 cil_length;
	/* The offset of the generated code, used for fixups */
	int native_offset;
	/* The length of the generated code, doesn't include alignment padding */
	int native_length;
	/* The real native offset, which includes alignment padding too */
	int real_native_offset;
	int max_offset;
	int max_length;
	/* Visited and reachable flags */
	guint32 flags;
	/*
	 * SSA and loop based flags
	 */
	MonoBitSet *dominators;
	MonoBitSet *dfrontier;
	MonoBasicBlock *idom;
	GSList *dominated;
	/* fast dominator algorithm */
	MonoBasicBlock *df_parent, *ancestor, *child, *label;
	int size, sdom, idomn;
	
	/* loop nesting and recognition */
	GList *loop_blocks;
	gint8  nesting;
	gint8  loop_body_start;
	/* 
	 * Whenever the bblock is rarely executed so it should be emitted after
	 * the function epilog.
	 */
	guint out_of_line : 1;
	/* Caches the result of uselessness calculation during optimize_branches */
	guint not_useless : 1;
	/* Whenever the decompose_array_access_opts () pass needs to process this bblock */
	guint needs_decompose : 1;
	/* Whenever this bblock is extended, ie. it has branches inside it */
	guint extended : 1;
	/* Whenever this bblock contains a OP_JUMP_TABLE instruction */
	guint has_jump_table : 1;
	/* Whenever this bblock contains an OP_CALL_HANDLER instruction */
	guint has_call_handler : 1;
	/* Whenever this bblock starts a try block */
	guint try_start : 1;
#ifdef ENABLE_LLVM
	/* The offset of the CIL instruction in this bblock which ends a try block */
	intptr_t try_end;
#endif
	/*
	 * If this is set, extend the try range started by this bblock by an arch specific
	 * number of bytes to encompass the end of the previous bblock (e.g. a Monitor.Enter
	 * call).
	 */
	guint extend_try_block : 1;
	
	/* use for liveness analysis */
	MonoBitSet *gen_set;
	MonoBitSet *kill_set;
	MonoBitSet *live_in_set;
	MonoBitSet *live_out_set;
	/* fields to deal with non-empty stack slots at bb boundary */
	guint16 out_scount, in_scount;
	MonoInst **out_stack;
	MonoInst **in_stack;
	/* we use that to prevent merging of bblocks covered by different clauses*/
	guint real_offset;
	GSList *seq_points;
	// The MonoInst of the last sequence point for the current basic block.
	MonoInst *last_seq_point;
	
	// This will hold a list of last sequence points of incoming basic blocks
	MonoInst **pred_seq_points;
	guint num_pred_seq_points;
	GSList *spill_slot_defs;
	/* List of call sites in this bblock sorted by pc_offset */
	GSList *gc_callsites;
	/*
	 * If this is not null, the basic block is a try hole for all the clauses
	 * in the list previous to this element (including the element).
	 */
	GList *clause_holes;
	/*
	 * The region encodes whether the basic block is inside
	 * a finally, catch, filter or none of these.
	 *
	 * If the value is -1, then it is neither finally, catch nor filter
	 *
	 * Otherwise the format is:
	 *
	 *  Bits: |     0-3      |       4-7      |     8-31
	 * 	  |		 |                |
	 *        | clause-flags |   MONO_REGION  | clause-index 
	 *
	 */
	guint region;
	/* The current symbolic register number, used in local register allocation. */
	guint32 max_vreg;
};
/* BBlock flags */
enum {
	BB_VISITED              = 1 << 0,
	BB_REACHABLE            = 1 << 1,
	BB_EXCEPTION_DEAD_OBJ   = 1 << 2,
	BB_EXCEPTION_UNSAFE     = 1 << 3,
	BB_EXCEPTION_HANDLER    = 1 << 4,
	/* for Native Client, mark the blocks that can be jumped to indirectly */
	BB_INDIRECT_JUMP_TARGET = 1 << 5 
};
typedef struct MonoMemcpyArgs {
	int size, align;
} MonoMemcpyArgs;
typedef enum {
	LLVMArgNone,
	/* Scalar argument passed by value */
	LLVMArgNormal,
	/* Only in ainfo->pair_storage */
	LLVMArgInIReg,
	/* Only in ainfo->pair_storage */
	LLVMArgInFPReg,
	/* Valuetype passed in 1-2 consecutive register */
	LLVMArgVtypeInReg,
	LLVMArgVtypeByVal,
	LLVMArgVtypeRetAddr, /* On on cinfo->ret */
	LLVMArgGSharedVt,
	/* Fixed size argument passed to/returned from gsharedvt method by ref */
	LLVMArgGsharedvtFixed,
	/* Fixed size vtype argument passed to/returned from gsharedvt method by ref */
	LLVMArgGsharedvtFixedVtype,
	/* Variable sized argument passed to/returned from gsharedvt method by ref */
	LLVMArgGsharedvtVariable,
	/* Vtype passed/returned as one int array argument */
	LLVMArgAsIArgs,
	/* Vtype passed as a set of fp arguments */
	LLVMArgAsFpArgs,
	/*
	 * Only for returns, a structure which
	 * consists of floats/doubles.
	 */
	LLVMArgFpStruct,
	LLVMArgVtypeByRef,
	/* Vtype returned as an int */
	LLVMArgVtypeAsScalar,
	/* Address to local vtype passed as argument (using register or stack). */
	LLVMArgVtypeAddr
} LLVMArgStorage;
typedef struct {
	LLVMArgStorage storage;
	/*
	 * Only if storage == ArgVtypeInReg/LLVMArgAsFpArgs.
	 * This contains how the parts of the vtype are passed.
	 */
	LLVMArgStorage pair_storage [8];
	/*
	 * Only if storage == LLVMArgAsIArgs/LLVMArgAsFpArgs/LLVMArgFpStruct.
	 * If storage == LLVMArgAsFpArgs, this is the number of arguments
	 * used to pass the value.
	 * If storage == LLVMArgFpStruct, this is the number of fields
	 * in the structure.
	 */
	int nslots;
	/* Only if storage == LLVMArgAsIArgs/LLVMArgAsFpArgs/LLVMArgFpStruct (4/8) */
	int esize;
	/* Parameter index in the LLVM signature */
	int pindex;
	MonoType *type;
	/* Only if storage == LLVMArgAsFpArgs. Dummy fp args to insert before this arg */
	int ndummy_fpargs;
} LLVMArgInfo;
typedef struct {
	LLVMArgInfo ret;
	/* Whenever there is an rgctx argument */
	gboolean rgctx_arg;
	/* Whenever there is an IMT argument */
	gboolean imt_arg;
	/* Whenever there is a dummy extra argument */
	gboolean dummy_arg;
	/* 
	 * The position of the vret arg in the argument list.
	 * Only if ret->storage == ArgVtypeRetAddr.
	 * Should be 0 or 1.
	 */
	int vret_arg_index;
	/* The indexes of various special arguments in the LLVM signature */
	int vret_arg_pindex, this_arg_pindex, rgctx_arg_pindex, imt_arg_pindex, dummy_arg_pindex;
	/* Inline array of argument info */
	/* args [0] is for the this argument if it exists */
	LLVMArgInfo args [1];
} LLVMCallInfo;
#define MONO_MAX_SRC_REGS	3
struct MonoInst {
 	guint16 opcode;
	guint8  type; /* stack type */
	guint8  flags;
	
	/* used by the register allocator */
	gint32 dreg, sreg1, sreg2, sreg3;
	MonoInst *next, *prev;
	union {
		union {
			MonoInst *src;
			MonoMethodVar *var;
			target_mgreg_t const_val;
#if (SIZEOF_REGISTER > TARGET_SIZEOF_VOID_P) && (G_BYTE_ORDER == G_BIG_ENDIAN)
			struct {
				gpointer p[SIZEOF_REGISTER/TARGET_SIZEOF_VOID_P];
			} pdata;
#else
			gpointer p;
#endif
			MonoMethod *method;
			MonoMethodSignature *signature;
			MonoBasicBlock **many_blocks;
			MonoBasicBlock *target_block;
			MonoInst **args;
			MonoType *vtype;
			MonoClass *klass;
			int *phi_args;
			MonoCallInst *call_inst;
			GList *exception_clauses;
		} op [2];
		gint64 i8const;
		double r8const;
	} data;
	const unsigned char* cil_code; /* for debugging and bblock splitting */
	/* used mostly by the backend to store additional info it may need */
	union {
		gint32 reg3;
		gint32 arg_info;
		gint32 size;
		MonoMemcpyArgs *memcpy_args; /* in OP_MEMSET and OP_MEMCPY */
		gpointer data;
		gint shift_amount;
		gboolean is_pinvoke; /* for variables in the unmanaged marshal format */
		gboolean record_cast_details; /* For CEE_CASTCLASS */
		MonoInst *spill_var; /* for OP_MOVE_I4_TO_F/F_TO_I4 and OP_FCONV_TO_R8_X */
		guint16 source_opcode; /*OP_XCONV_R8_TO_I4 needs to know which op was used to do proper widening*/
		int pc_offset; /* OP_GC_LIVERANGE_START/END */
		/*
		 * memory_barrier: MONO_MEMORY_BARRIER_{ACQ,REL,SEQ}
		 * atomic_load_*: MONO_MEMORY_BARRIER_{ACQ,SEQ}
		 * atomic_store_*: MONO_MEMORY_BARRIER_{REL,SEQ}
		 */
		int memory_barrier_kind;
	} backend;
	
	MonoClass *klass;
};
	
struct MonoCallInst {
	MonoInst inst;
	MonoMethodSignature *signature;
	MonoMethod *method;
	MonoInst **args;
	MonoInst *out_args;
	MonoInst *vret_var;
	gconstpointer fptr;
	MonoJitICallId jit_icall_id;
	guint stack_usage;
	guint stack_align_amount;
	regmask_t used_iregs;
	regmask_t used_fregs;
	GSList *out_ireg_args;
	GSList *out_freg_args;
	GSList *outarg_vts;
	CallInfo *call_info;
#ifdef ENABLE_LLVM
	LLVMCallInfo *cinfo;
	int rgctx_arg_reg, imt_arg_reg;
#endif
	// Bitfields are at the end to minimize padding for alignment,
	// unless there is a placement to increase locality.
	guint is_virtual : 1;
	// FIXME tailcall field is written after read; prefer MONO_IS_TAILCALL_OPCODE.
	guint tailcall : 1;
	/* If this is TRUE, 'fptr' points to a MonoJumpInfo instead of an address. */
	guint fptr_is_patch : 1;
	/*
	 * If this is true, then the call returns a vtype in a register using the same
	 * calling convention as OP_CALL.
	 */
	guint vret_in_reg : 1;
	/* Whenever vret_in_reg returns fp values */
	guint vret_in_reg_fp : 1;
	/* Whenever there is an IMT argument and it is dynamic */
	guint dynamic_imt_arg : 1;
	/* Whenever there is an RGCTX argument */
	guint32 rgctx_reg : 1;
	/* Whenever the call will need an unbox trampoline */
	guint need_unbox_trampoline : 1;
};
struct MonoCallArgParm {
	MonoInst ins;
	gint32 size;
	gint32 offset;
	gint32 offPrm;
};
/* 
 * flags for MonoInst
 * Note: some of the values overlap, because they can't appear
 * in the same MonoInst.
 */
enum {
	MONO_INST_HAS_METHOD = 1,
	MONO_INST_INIT       = 1, /* in localloc */
	MONO_INST_SINGLE_STEP_LOC = 1, /* in SEQ_POINT */
	MONO_INST_IS_DEAD    = 2,
	MONO_INST_TAILCALL   = 4,
	MONO_INST_VOLATILE   = 4,
	MONO_INST_NOTYPECHECK    = 4,
	MONO_INST_NONEMPTY_STACK = 4, /* in SEQ_POINT */
	MONO_INST_UNALIGNED  = 8,
	MONO_INST_NESTED_CALL = 8, /* in SEQ_POINT */
    MONO_INST_CFOLD_TAKEN = 8, /* On branches */
    MONO_INST_CFOLD_NOT_TAKEN = 16, /* On branches */
	MONO_INST_DEFINITION_HAS_SIDE_EFFECTS = 8,
	/* the address of the variable has been taken */
	MONO_INST_INDIRECT   = 16,
	MONO_INST_NORANGECHECK   = 16,
	/* On loads, the source address can be null */
	MONO_INST_FAULT = 32,
	/* 
	 * On variables, identifies LMF variables. These variables have a dummy type (int), but
	 * require stack space for a MonoLMF struct.
	 */
	MONO_INST_LMF = 32,
	/* On loads, the source address points to a constant value */
	MONO_INST_INVARIANT_LOAD = 64,
	/* On stores, the destination is the stack */
	MONO_INST_STACK_STORE = 64,
	/* On variables, the variable needs GC tracking */
	MONO_INST_GC_TRACK = 128,
	/*
	 * Set on instructions during code emission which make calls, i.e. OP_CALL, OP_THROW.
	 * backend.pc_offset will be set to the pc offset at the end of the native call instructions.
	 */
	MONO_INST_GC_CALLSITE = 128,
	/* On comparisons, mark the branch following the condition as likely to be taken */
	MONO_INST_LIKELY = 128,
};
#define inst_c0 data.op[0].const_val
#define inst_c1 data.op[1].const_val
#define inst_i0 data.op[0].src
#define inst_i1 data.op[1].src
#if (SIZEOF_REGISTER > TARGET_SIZEOF_VOID_P) && (G_BYTE_ORDER == G_BIG_ENDIAN)
#define inst_p0 data.op[0].pdata.p[SIZEOF_REGISTER/TARGET_SIZEOF_VOID_P - 1]
#define inst_p1 data.op[1].pdata.p[SIZEOF_REGISTER/TARGET_SIZEOF_VOID_P - 1]
#else
#define inst_p0 data.op[0].p
#define inst_p1 data.op[1].p
#endif
#define inst_l  data.i8const
#define inst_r  data.r8const
#define inst_left  data.op[0].src
#define inst_right data.op[1].src
#define inst_newa_len   data.op[0].src
#define inst_newa_class data.op[1].klass
#define inst_var    data.op[0].var
#define inst_vtype  data.op[1].vtype
/* in branch instructions */
#define inst_many_bb   data.op[1].many_blocks
#define inst_target_bb data.op[0].target_block
#define inst_true_bb   data.op[1].many_blocks[0]
#define inst_false_bb  data.op[1].many_blocks[1]
#define inst_basereg sreg1
#define inst_indexreg sreg2
#define inst_destbasereg dreg
#define inst_offset data.op[0].const_val
#define inst_imm    data.op[1].const_val
#define inst_call   data.op[1].call_inst
#define inst_phi_args   data.op[1].phi_args
#define inst_eh_blocks	 data.op[1].exception_clauses
/* Return the lower 32 bits of the 64 bit immediate in INS */
static inline guint32
ins_get_l_low (MonoInst *ins)
{
	return (guint32)(ins->data.i8const & 0xffffffff);
}
/* Return the higher 32 bits of the 64 bit immediate in INS */
static inline guint32
ins_get_l_high (MonoInst *ins)
{
	return (guint32)((ins->data.i8const >> 32) & 0xffffffff);
}
static inline void
mono_inst_set_src_registers (MonoInst *ins, int *regs)
{
	ins->sreg1 = regs [0];
	ins->sreg2 = regs [1];
	ins->sreg3 = regs [2];
}
typedef union {
	struct {
		guint16 tid; /* tree number */
		guint16 bid; /* block number */
	} pos ;
	guint32 abs_pos; 
} MonoPosition;
typedef struct {
	MonoPosition first_use, last_use;
} MonoLiveRange;
typedef struct MonoLiveRange2 MonoLiveRange2;
struct MonoLiveRange2 {
	int from, to;
	MonoLiveRange2 *next;
};
typedef struct {
	/* List of live ranges sorted by 'from' */
	MonoLiveRange2 *range;
	MonoLiveRange2 *last_range;
} MonoLiveInterval;
/*
 * Additional information about a variable
 */
struct MonoMethodVar {
	guint           idx; /* inside cfg->varinfo, cfg->vars */
	MonoLiveRange   range; /* generated by liveness analysis */
	MonoLiveInterval *interval; /* generated by liveness analysis */
	int             reg; /* != -1 if allocated into a register */
	int             spill_costs;
	MonoBitSet     *def_in; /* used by SSA */
	MonoInst       *def;    /* used by SSA */
	MonoBasicBlock *def_bb; /* used by SSA */
	GList          *uses;   /* used by SSA */
	char            cpstate;  /* used by SSA conditional  constant propagation */
	/* The native offsets corresponding to the live range of the variable */
	gint32         live_range_start, live_range_end;
	/* 
	 * cfg->varinfo [idx]->dreg could be replaced for OP_REGVAR, this contains the 
	 * original vreg.
	 */
	gint32         vreg;
};
// Subset of MonoJumpInfo.
 typedef struct MonoJumpInfoTarget {
	MonoJumpInfoType type;
	gconstpointer   target;
} MonoJumpInfoTarget;
#define mono_jit_icall_id_to_trampoline_type(a) ((MonoTrampolineType)((a) - MONO_JIT_ICALL_generic_trampoline_jit))
/* These trampolines receive an argument directly in a register */
#define MONO_TRAMPOLINE_TYPE_HAS_ARG(t)		\
	(FALSE)
/*
 * This structure represents a JIT backend.
 */
typedef struct {
	guint            have_card_table_wb : 1;
	guint            have_op_generic_class_init : 1;
	guint            emulate_mul_div : 1;
	guint            emulate_div : 1;
	guint            emulate_long_shift_opts : 1;
	guint            have_objc_get_selector : 1;
	guint            have_generalized_imt_trampoline : 1;
	gboolean         have_op_tailcall_membase : 1;
	gboolean         have_op_tailcall_reg : 1;
	gboolean         have_volatile_non_param_register : 1;
	guint            gshared_supported : 1;
	guint            use_fpstack : 1;
	guint            ilp32 : 1;
	guint            need_got_var : 1;
	guint            need_div_check : 1;
	guint            no_unaligned_access : 1;
	guint            disable_div_with_mul : 1;
	guint            explicit_null_checks : 1;
	guint            optimized_div : 1;
	guint            force_float32 : 1;
	int              monitor_enter_adjustment;
	int              dyn_call_param_area;
} MonoBackend;
/* Flags for mini_method_compile () */
typedef enum {
	/* Whenever to run cctors during JITting */
	JIT_FLAG_RUN_CCTORS = (1 << 0),
	/* Whenever this is an AOT compilation */
	JIT_FLAG_AOT = (1 << 1),
	/* Whenever this is a full AOT compilation */
	JIT_FLAG_FULL_AOT = (1 << 2),
	/* Whenever to compile with LLVM */
	JIT_FLAG_LLVM = (1 << 3),
	/* Whenever to disable direct calls to icall functions */
	JIT_FLAG_NO_DIRECT_ICALLS = (1 << 4),
	/* Emit explicit null checks */
	JIT_FLAG_EXPLICIT_NULL_CHECKS = (1 << 5),
	/* Whenever to compile in llvm-only mode */
	JIT_FLAG_LLVM_ONLY = (1 << 6),
	/* Whenever calls to pinvoke functions are made directly */
	JIT_FLAG_DIRECT_PINVOKE = (1 << 7),
	/* Whenever this is a compile-all run and the result should be discarded */
	JIT_FLAG_DISCARD_RESULTS = (1 << 8),
	/* Whenever to generate code which can work with the interpreter */
	JIT_FLAG_INTERP = (1 << 9),
	/* Allow AOT to use all current CPU instructions */
	JIT_FLAG_USE_CURRENT_CPU = (1 << 10),
	/* Generate code to self-init the method for AOT */
	JIT_FLAG_SELF_INIT = (1 << 11),
	/* Assume code memory is exec only */
	JIT_FLAG_CODE_EXEC_ONLY = (1 << 12),
	/*
	 * If the LLVM backend declines the method, fail the compile instead of
	 * restarting it as a classic one. Used by tier-1 promotion, where the
	 * method already has a classic tier-0 body, so a fallback compile would
	 * only produce a second copy of it that is immediately discarded.
	 */
	JIT_FLAG_NO_LLVM_FALLBACK = (1 << 13),
	/*
	 * Run the front-end (and the LLVM-path IR transforms) but stop before
	 * mono_llvm_emit_method (): return the MonoCompile with LLVM-ready MonoIR
	 * instead of emitting/codegen'ing it. The tier-1 inliner's lazy callee
	 * materialization uses this to obtain a callee body it then translates into
	 * the caller's module itself. Implies the LLVM path.
	 */
	JIT_FLAG_LLVM_IR_ONLY = (1 << 14),
	/*
	 * The method being compiled is ALREADY the shared form - its instantiation is
	 * over type parameters rather than over real types - so compile it as it
	 * stands with cfg->gshared on, instead of asking whether it can be shared and
	 * redirecting through mini_get_shared_method_full (). An already-shared method
	 * answers "no" to that question, which would otherwise get its
	 * type-parameter-bearing body compiled as if it were concrete.
	 *
	 * Only the tier-1 inliner passes this, and only for a callee it resolved out
	 * of a gshared root's own IR, where the caller supplies the runtime generic
	 * context the shared body expects.
	 */
	JIT_FLAG_METHOD_IS_GSHARED = (1 << 15),
} JitFlags;
/* Bit-fields in the MonoBasicBlock.region */
#define MONO_REGION_TRY       0
#define MONO_REGION_FINALLY  16
#define MONO_REGION_CATCH    32
#define MONO_REGION_FAULT    64
#define MONO_REGION_FILTER  128
#define MONO_BBLOCK_IS_IN_REGION(bblock, regtype) (((bblock)->region & (0xf << 4)) == (regtype))
#define MONO_REGION_FLAGS(region) ((region) & 0x7)
#define MONO_REGION_CLAUSE_INDEX(region) (((region) >> 8) - 1)
#define get_vreg_to_inst(cfg, vreg) ((vreg) < (cfg)->vreg_to_inst_len ? (cfg)->vreg_to_inst [(vreg)] : NULL)
#define vreg_is_volatile(cfg, vreg) (G_UNLIKELY (get_vreg_to_inst ((cfg), (vreg)) && (get_vreg_to_inst ((cfg), (vreg))->flags & (MONO_INST_VOLATILE|MONO_INST_INDIRECT))))
#define vreg_is_ref(cfg, vreg) ((vreg) < (cfg)->vreg_is_ref_len ? (cfg)->vreg_is_ref [(vreg)] : 0)
#define vreg_is_mp(cfg, vreg) ((vreg) < (cfg)->vreg_is_mp_len ? (cfg)->vreg_is_mp [(vreg)] : 0)
/*
 * Control Flow Graph and compilation unit information
 */
typedef struct MonoCompile {
	MonoMethod      *method;
	MonoMethodHeader *header;
	MonoMemPool     *mempool;
	MonoInst       **varinfo;
	MonoMethodVar   *vars;
	MonoInst        *ret;
	MonoBasicBlock  *bb_entry;
	MonoBasicBlock  *bb_exit;
	MonoBasicBlock  *bb_init;
	MonoBasicBlock **bblocks;
	MonoBasicBlock **cil_offset_to_bb;
	MonoMemPool     *state_pool; /* used by instruction selection */
	MonoBasicBlock  *cbb;        /* used by instruction selection */
	MonoInst        *prev_ins;   /* in decompose */
	MonoJumpInfo    *patch_info;
	MonoJitInfo     *jit_info;
	MonoJitDynamicMethodInfo *dynamic_info;
	guint            num_bblocks, max_block_num;
	guint            locals_start;
	guint            num_varinfo; /* used items in varinfo */
	guint            varinfo_count; /* total storage in varinfo */
	gint             stack_offset;
	gint             max_ireg;
	gint             cil_offset_to_bb_len;
	MonoRegState    *rs;
	MonoSpillInfo   *spill_info [16]; /* machine register spills */
	gint             spill_count;
	gint             spill_info_len [16];
	/* unsigned char   *cil_code; */
	MonoInst        *domainvar; /* a cache for the current domain */
	MonoInst        *got_var; /* Global Offset Table variable */
	MonoInst        **locals;
	/* Variable holding the mrgctx/vtable address for gshared methods */
	MonoInst        *rgctx_var;
	MonoInst        **args;
	MonoType        **arg_types;
	MonoMethod      *current_method; /* The method currently processed by method_to_ir () */
	MonoMethod      *method_to_register; /* The method to register in JIT info tables */
	MonoGenericContext *generic_context;
	MonoInst        *this_arg;
	MonoBackend *backend;
	/* 
	 * This variable represents the hidden argument holding the vtype
	 * return address. If the method returns something other than a vtype, or
	 * the vtype is returned in registers this is NULL.
	 */
	MonoInst        *vret_addr;
	/*
	 * This is used to initialize the cil_code field of MonoInst's.
	 */
	const unsigned char *ip;
	
	struct MonoAliasingInformation *aliasing_info;
	/* A hashtable of region ID-> SP var mappings */
	/* An SP var is a place to store the stack pointer (used by handlers)*/
	/*
	 * FIXME We can potentially get rid of this, since it was mainly used
	 * for hijacking return address for handler.
	 */
	GHashTable      *spvars;
	/*
	 * A hashtable of region ID -> EX var mappings
	 * An EX var stores the exception object passed to catch/filter blocks
	 * For finally blocks, it is set to TRUE if we should throw an abort
	 * once the execution of the finally block is over.
	 */
	GHashTable      *exvars;
	GList           *ldstr_list; /* used by AOT */
	MonoDomain      *domain;
	
	guint            real_offset;
	GHashTable      *cbb_hash;
	/* The current virtual register number */
	guint32 next_vreg;
	MonoGenericSharingContext gsctx;
	MonoGenericContext *gsctx_context;
	MonoGSharedVtMethodInfo *gsharedvt_info;
	MonoMemoryManager *mem_manager;
	/* Points to the gsharedvt locals area at runtime */
	MonoInst *gsharedvt_locals_var;
	/* The localloc instruction used to initialize gsharedvt_locals_var */
	MonoInst *gsharedvt_locals_var_ins;
	/* Points to a MonoGSharedVtMethodRuntimeInfo at runtime */
	MonoInst *gsharedvt_info_var;
	/* For native-to-managed wrappers, CEE_MONO_JIT_(AT|DE)TACH opcodes */
	MonoInst *orig_domain_var;
	MonoInst *lmf_var;
	MonoInst *lmf_addr_var;
	MonoInst *stack_inbalance_var;
	unsigned char   *cil_start;
	unsigned char   *native_code;
	guint            code_size;
	guint            code_len;
	guint            prolog_end;
	guint            epilog_begin;
	guint            epilog_end;
	regmask_t        used_int_regs;
	guint32          opt;
	guint32          flags;
	guint32          comp_done;
	guint32          verbose_level;
	guint32          stack_usage;
	guint32          param_area;
	guint32          frame_reg;
	gint32           sig_cookie;
	guint            disable_aot : 1;
	guint            disable_ssa : 1;
	guint            disable_llvm : 1;
	guint            enable_extended_bblocks : 1;
	guint            run_cctors : 1;
	guint            need_lmf_area : 1;
	guint            compile_aot : 1;
	guint            full_aot : 1;
	guint            compile_llvm : 1;
	guint            got_var_allocated : 1;
	guint            ret_var_is_local : 1;
	guint            ret_var_set : 1;
	guint            unverifiable : 1;
	guint            skip_visibility : 1;
	guint            disable_llvm_implicit_null_checks : 1;
	guint            disable_reuse_registers : 1;
	guint            disable_reuse_stack_slots : 1;
	guint            disable_reuse_ref_stack_slots : 1;
	guint            disable_ref_noref_stack_slot_share : 1;
	guint            disable_initlocals_opt : 1;
	guint            disable_initlocals_opt_refs : 1;
	guint            disable_omit_fp : 1;
	guint            disable_vreg_to_lvreg : 1;
	guint            disable_deadce_vars : 1;
	guint            disable_out_of_line_bblocks : 1;
	guint            disable_direct_icalls : 1;
	guint            disable_gc_safe_points : 1;
	guint            direct_pinvoke : 1;
	guint            create_lmf_var : 1;
	/*
	 * When this is set, the code to push/pop the LMF from the LMF stack is generated as IR
	 * instead of being generated in emit_prolog ()/emit_epilog ().
	 */
	guint            lmf_ir : 1;
	/*
	 * Whenever to use the mono_lmf TLS variable instead of indirection through the
	 * mono_lmf_addr TLS variable.
	 */
	guint            gen_write_barriers : 1;
	guint            init_ref_vars : 1;
	guint            extend_live_ranges : 1;
	guint            compute_precise_live_ranges : 1;
	guint            has_got_slots : 1;
	guint            uses_rgctx_reg : 1;
	guint            uses_vtable_reg : 1;
	guint            keep_cil_nops : 1;
	guint            gen_seq_points : 1;
	/* Generate seq points for use by the debugger */
	guint            gen_sdb_seq_points : 1;
	guint            explicit_null_checks : 1;
	guint            compute_gc_maps : 1;
	guint            soft_breakpoints : 1;
	guint            arch_eh_jit_info : 1;
	guint            has_calls : 1;
	guint            has_emulated_ops : 1;
	guint            has_indirection : 1;
	guint            has_atomic_add_i4 : 1;
	guint            has_atomic_exchange_i4 : 1;
	guint            has_atomic_cas_i4 : 1;
	guint            check_pinvoke_callconv : 1;
	guint            has_unwind_info_for_epilog : 1;
	guint            disable_inline : 1;
	/* Disable inlining into caller */
	guint            no_inline : 1;
	guint            gshared : 1;
	guint            gsharedvt : 1;
	guint            r4fp : 1;
	guint            llvm_only : 1;
	/*
	 * JIT_FLAG_LLVM_IR_ONLY: this body is being compiled for the tier-1 inliner
	 * to fold into a caller, not to be published as the method's own code. It
	 * will therefore never be reached by a call to the method itself, which is
	 * an assumption parts of the front-end otherwise make.
	 */
	guint            llvm_ir_only : 1;
	guint            interp : 1;
	guint            use_current_cpu : 1;
	guint            self_init : 1;
	guint            domainvar_inited : 1;
	guint            code_exec_only : 1;
	guint            interp_entry_only : 1;
	guint8           uses_simd_intrinsics;
	int              r4_stack_type;
	gpointer         debug_info;
	guint32          lmf_offset;
	guint16          *intvars;
	MonoProfilerCoverageInfo *coverage_info;
	GHashTable       *token_info_hash;
	MonoCompileArch  arch;
	guint32          inline_depth;
	/* Size of memory reserved for thunks */
	int              thunk_area;
	/* Thunks */
	guint8          *thunks;
	/* Offset between the start of code and the thunks area */
	int              thunks_offset;
	MonoExceptionType exception_type;	/* MONO_EXCEPTION_* */
	guint32          exception_data;
	char*            exception_message;
	gpointer         exception_ptr;
	guint8 *         encoded_unwind_ops;
	guint32          encoded_unwind_ops_len;
	GSList*          unwind_ops;
	GList*           dont_inline;
	/* Fields used by the local reg allocator */
	void*            reginfo;
	int              reginfo_len;
	/* Maps vregs to their associated MonoInst's */
	/* vregs with an associated MonoInst are 'global' while others are 'local' */
	MonoInst **vreg_to_inst;
	/* Size of above array */
	guint32 vreg_to_inst_len;
	/* Marks vregs which hold a GC ref */
	/* FIXME: Use a bitmap */
	gboolean *vreg_is_ref;
	/* Size of above array */
	guint32 vreg_is_ref_len;
	/* Marks vregs which hold a managed pointer */
	/* FIXME: Use a bitmap */
	gboolean *vreg_is_mp;
	/* Size of above array */
	guint32 vreg_is_mp_len;
	/* 
	 * The original method to compile, differs from 'method' when doing generic
	 * sharing.
	 */
	MonoMethod *orig_method;
	/* Patches which describe absolute addresses embedded into the native code */
	GHashTable *abs_patches;
	/* Used to implement move_i4_to_f on archs that can't do raw
	copy between an ireg and a freg. This is an int32 var.*/
	MonoInst *iconv_raw_var;
	/* Used to implement fconv_to_r8_x. This is a double (8 bytes) var.*/
	MonoInst *fconv_to_r8_x_var;
	/*Use to implement simd constructors. This is a vector (16 bytes) var.*/
	MonoInst *simd_ctor_var;
	/* Used to implement dyn_call */
	MonoInst *dyn_call_var;
	MonoInst *last_seq_point;
	/*
	 * List of sequence points represented as IL offset+native offset pairs.
	 * Allocated using glib.
	 * IL offset can be -1 or 0xffffff to refer to the sequence points
	 * inside the prolog and epilog used to implement method entry/exit events.
	 */
	GPtrArray *seq_points;
	/* The encoded sequence point info */
	struct MonoSeqPointInfo *seq_point_info;
	/* Method headers which need to be freed after compilation */
	GSList *headers_to_free;
	/* Used by AOT */
	guint32 got_offset, ex_info_offset, method_info_offset, method_index;
	guint32 aot_method_flags;
	/* For llvm */
	guint32 got_access_count;
	gpointer llvmonly_init_cond;
	gpointer llvm_dummy_info_var, llvm_info_var;
	/* Symbol used to refer to this method in generated assembly */
	char *asm_symbol;
	char *asm_debug_symbol;
	char *llvm_method_name;
	int castclass_cache_index;
	MonoJitExceptionInfo *llvm_ex_info;
	guint32 llvm_ex_info_len;
	int llvm_this_reg, llvm_this_offset;
	/*
	 * This body's native_offset -> il_offset map, or NULL when it has none.
	 * mono_save_il_offset_map () builds it and create_jit_info () copies it onto
	 * jit_info->il_offsets.
	 */
	MonoILOffsetEntry *il_offsets;
	guint32 n_il_offsets;
	/* Copied onto jit_info->llvm_inline_frames by create_jit_info (). */
	MonoLLVMInlineFrame *llvm_inline_frames;
	guint32 n_llvm_inline_frames;
	GSList *try_block_holes;
	/* DWARF location list for 'this' */
	GSList *this_loclist;
	/* DWARF location list for 'rgctx_var' */
	GSList *rgctx_loclist;
	int *gsharedvt_vreg_to_idx;
	GSList *signatures;
	GSList *interp_in_signatures;
	/* GC Maps */
   
	/* The offsets of the locals area relative to the frame pointer */
	gint locals_min_stack_offset, locals_max_stack_offset;
	/* The current CFA rule */
	int cur_cfa_reg, cur_cfa_offset;
	/* The final CFA rule at the end of the prolog */
	int cfa_reg, cfa_offset;
	/* Points to a MonoCompileGC */
	gpointer gc_info;
	/*
	 * The encoded GC map along with its size. This contains binary data so it can be saved in an AOT
	 * image etc, but it requires a 4 byte alignment.
	 */
	guint8 *gc_map;
	guint32 gc_map_size;
	/* Error handling */
	MonoError* error;
	MonoErrorInternal error_value;
	/* pointer to context datastructure used for graph dumping */
	MonoGraphDumper *gdump_ctx;
	/* Stats */
	int stat_allocate_var;
	int stat_locals_stack_size;
	int stat_basic_blocks;
	int stat_cil_code_size;
	int stat_n_regvars;
	int stat_inlineable_methods;
	int stat_inlined_methods;
	int stat_code_reallocs;
	MonoProfilerCallInstrumentationFlags prof_flags;
	gboolean prof_coverage;
	/* For deduplication */
	gboolean skip;
} MonoCompile;
#define MONO_CFG_PROFILE(cfg, flag) \
	G_UNLIKELY ((cfg)->prof_flags & MONO_PROFILER_CALL_INSTRUMENTATION_ ## flag)
#define MONO_CFG_PROFILE_CALL_CONTEXT(cfg) \
	(MONO_CFG_PROFILE (cfg, ENTER_CONTEXT) || MONO_CFG_PROFILE (cfg, LEAVE_CONTEXT))
typedef enum {
	MONO_CFG_HAS_ALLOCA = 1 << 0,
	MONO_CFG_HAS_CALLS  = 1 << 1,
	MONO_CFG_HAS_LDELEMA  = 1 << 2,
	MONO_CFG_HAS_VARARGS  = 1 << 3,
	MONO_CFG_HAS_TAILCALL = 1 << 4,
	MONO_CFG_HAS_FPOUT    = 1 << 5, /* there are fp values passed in int registers */
	MONO_CFG_HAS_SPILLUP  = 1 << 6, /* spill var slots are allocated from bottom to top */
	MONO_CFG_HAS_CHECK_THIS  = 1 << 7,
	MONO_CFG_NEEDS_DECOMPOSE = 1 << 8,
	MONO_CFG_HAS_TYPE_CHECK = 1 << 9
} MonoCompileFlags;
typedef enum {
	MONO_CFG_USES_SIMD_INTRINSICS = 1 << 0,
	MONO_CFG_USES_SIMD_INTRINSICS_SIMPLIFY_INDIRECTION = 1 << 1,
	MONO_CFG_USES_SIMD_INTRINSICS_DECOMPOSE_VTYPE = 1 << 2
} MonoSimdIntrinsicsFlags;
/* opcodes: value assigned after all the CIL opcodes */
#ifdef MINI_OP
#undef MINI_OP
#endif
#ifdef MINI_OP3
#undef MINI_OP3
#endif
#define MINI_OP(a,b,dest,src1,src2) a,
#define MINI_OP3(a,b,dest,src1,src2,src3) a,
enum {
	OP_START = MONO_CEE_LAST - 1,
#include "mini-ops.h"
	OP_LAST
};
#undef MINI_OP
#undef MINI_OP3
#if TARGET_SIZEOF_VOID_P == 8
#define OP_PCONST OP_I8CONST
#define OP_DUMMY_PCONST OP_DUMMY_I8CONST
#define OP_PADD OP_LADD
#define OP_PADD_IMM OP_LADD_IMM
#define OP_PSUB_IMM OP_LSUB_IMM
#define OP_PAND_IMM OP_LAND_IMM
#define OP_PXOR_IMM OP_LXOR_IMM
#define OP_PSUB OP_LSUB
#define OP_PMUL OP_LMUL
#define OP_PMUL_IMM OP_LMUL_IMM
#define OP_PNEG OP_LNEG
#define OP_PCONV_TO_I1 OP_LCONV_TO_I1
#define OP_PCONV_TO_U1 OP_LCONV_TO_U1
#define OP_PCONV_TO_I2 OP_LCONV_TO_I2
#define OP_PCONV_TO_U2 OP_LCONV_TO_U2
#define OP_PCONV_TO_OVF_I1_UN OP_LCONV_TO_OVF_I1_UN
#define OP_PCONV_TO_OVF_I1 OP_LCONV_TO_OVF_I1
#define OP_PBEQ OP_LBEQ
#define OP_PCEQ OP_LCEQ
#define OP_PCLT OP_LCLT
#define OP_PCGT OP_LCGT
#define OP_PCLT_UN OP_LCLT_UN
#define OP_PCGT_UN OP_LCGT_UN
#define OP_PBNE_UN OP_LBNE_UN
#define OP_PBGE_UN OP_LBGE_UN
#define OP_PBLT_UN OP_LBLT_UN
#define OP_PBGE OP_LBGE
#define OP_STOREP_MEMBASE_REG OP_STOREI8_MEMBASE_REG
#define OP_STOREP_MEMBASE_IMM OP_STOREI8_MEMBASE_IMM
#else
#define OP_PCONST OP_ICONST
#define OP_DUMMY_PCONST OP_DUMMY_ICONST
#define OP_PADD OP_IADD
#define OP_PADD_IMM OP_IADD_IMM
#define OP_PSUB_IMM OP_ISUB_IMM
#define OP_PAND_IMM OP_IAND_IMM
#define OP_PXOR_IMM OP_IXOR_IMM
#define OP_PSUB OP_ISUB
#define OP_PMUL OP_IMUL
#define OP_PMUL_IMM OP_IMUL_IMM
#define OP_PNEG OP_INEG
#define OP_PCONV_TO_I1 OP_ICONV_TO_I1
#define OP_PCONV_TO_U1 OP_ICONV_TO_U1
#define OP_PCONV_TO_I2 OP_ICONV_TO_I2
#define OP_PCONV_TO_U2 OP_ICONV_TO_U2
#define OP_PCONV_TO_OVF_I1_UN OP_ICONV_TO_OVF_I1_UN
#define OP_PCONV_TO_OVF_I1 OP_ICONV_TO_OVF_I1
#define OP_PBEQ OP_IBEQ
#define OP_PCEQ OP_ICEQ
#define OP_PCLT OP_ICLT
#define OP_PCGT OP_ICGT
#define OP_PCLT_UN OP_ICLT_UN
#define OP_PCGT_UN OP_ICGT_UN
#define OP_PBNE_UN OP_IBNE_UN
#define OP_PBGE_UN OP_IBGE_UN
#define OP_PBLT_UN OP_IBLT_UN
#define OP_PBGE OP_IBGE
#define OP_STOREP_MEMBASE_REG OP_STOREI4_MEMBASE_REG
#define OP_STOREP_MEMBASE_IMM OP_STOREI4_MEMBASE_IMM
#endif
/* Opcodes to load/store regsize quantities */
#if defined (MONO_ARCH_ILP32)
#define OP_LOADR_MEMBASE OP_LOADI8_MEMBASE
#define OP_STORER_MEMBASE_REG OP_STOREI8_MEMBASE_REG
#else
#define OP_LOADR_MEMBASE OP_LOAD_MEMBASE
#define OP_STORER_MEMBASE_REG OP_STORE_MEMBASE_REG
#endif
typedef enum {
	STACK_INV,
	STACK_I4,
	STACK_I8,
	STACK_PTR,
	STACK_R8,
	STACK_MP,
	STACK_OBJ,
	STACK_VTYPE,
	STACK_R4,
	STACK_MAX
} MonoStackType;
typedef struct {
	union {
		double   r8;
		gint32   i4;
		gint64   i8;
		gpointer p;
		MonoClass *klass;
	} data;
	int type;
} StackSlot;
extern const MonoInstSpec MONO_ARCH_CPU_SPEC [];
#define MONO_ARCH_CPU_SPEC_IDX_COMBINE(a) a ## _idx
#define MONO_ARCH_CPU_SPEC_IDX(a) MONO_ARCH_CPU_SPEC_IDX_COMBINE(a)
extern const guint16 MONO_ARCH_CPU_SPEC_IDX(MONO_ARCH_CPU_SPEC) [];
#define ins_get_spec(op) ((const char*)&MONO_ARCH_CPU_SPEC [MONO_ARCH_CPU_SPEC_IDX(MONO_ARCH_CPU_SPEC)[(op) - OP_LOAD]])
#ifndef DISABLE_JIT
static inline int
ins_get_size (int opcode)
{
	return ((guint8 *)ins_get_spec (opcode))[MONO_INST_LEN];
}
guint8*
mini_realloc_code_slow (MonoCompile *cfg, int size);
static inline guint8*
realloc_code (MonoCompile *cfg, int size)
{
	const guint EXTRA_CODE_SPACE = 16;
	const guint code_len = cfg->code_len;
	if (G_UNLIKELY (code_len + (guint)size > (cfg->code_size - EXTRA_CODE_SPACE)))
		return mini_realloc_code_slow (cfg, size);
	return cfg->native_code + code_len;
}
static inline void
set_code_len (MonoCompile *cfg, int len)
{
	g_assert ((guint)len <= cfg->code_size);
	cfg->code_len = len;
}
static inline void
set_code_cursor (MonoCompile *cfg, void* void_code)
{
	guint8* code = (guint8*)void_code;
	g_assert (code <= (cfg->native_code + cfg->code_size));
	set_code_len (cfg, code - cfg->native_code);
}
#endif
enum {
	MONO_COMP_DOM = 1,
	MONO_COMP_IDOM = 2,
	MONO_COMP_DFRONTIER = 4,
	MONO_COMP_DOM_REV = 8,
	MONO_COMP_LIVENESS = 16,
	MONO_COMP_SSA = 32,
	MONO_COMP_SSA_DEF_USE = 64,
	MONO_COMP_REACHABILITY = 128,
	MONO_COMP_LOOPS = 256
};
typedef enum {
	MONO_GRAPH_CFG = 1,
	MONO_GRAPH_DTREE = 2,
	MONO_GRAPH_CFG_CODE = 4,
	MONO_GRAPH_CFG_SSA = 8,
	MONO_GRAPH_CFG_OPTCODE = 16
} MonoGraphOptions;
typedef struct {
	guint16 size;
	guint16 offset;
	guint8  pad;
} MonoJitArgumentInfo;
enum {
	BRANCH_NOT_TAKEN,
	BRANCH_TAKEN,
	BRANCH_UNDEF
};
typedef enum {
	CMP_EQ,
	CMP_NE,
	CMP_LE,
	CMP_GE,
	CMP_LT,
	CMP_GT,
	CMP_LE_UN,
	CMP_GE_UN,
	CMP_LT_UN,
	CMP_GT_UN,
	CMP_ORD,
	CMP_UNORD
} CompRelation;
typedef enum {
	CMP_TYPE_L,
	CMP_TYPE_I,
	CMP_TYPE_F
} CompType;
/* Implicit exceptions */
enum {
	MONO_EXC_INDEX_OUT_OF_RANGE,
	MONO_EXC_OVERFLOW,
	MONO_EXC_ARITHMETIC,
	MONO_EXC_DIVIDE_BY_ZERO,
	MONO_EXC_INVALID_CAST,
	MONO_EXC_NULL_REF,
	MONO_EXC_ARRAY_TYPE_MISMATCH,
	MONO_EXC_ARGUMENT,
	MONO_EXC_ARGUMENT_OUT_OF_RANGE,
	MONO_EXC_INTRINS_NUM
};
typedef void (*MonoInstFunc) (MonoInst *tree, gpointer data);
enum {
	FILTER_IL_SEQ_POINT = 1 << 0,
	FILTER_NOP          = 1 << 1,
};
static inline gboolean
mono_inst_filter (MonoInst *ins, int filter)
{
	if (!ins || !filter)
		return FALSE;
	if ((filter & FILTER_IL_SEQ_POINT) && ins->opcode == OP_IL_SEQ_POINT)
		return TRUE;
	if ((filter & FILTER_NOP) && ins->opcode == OP_NOP)
		return TRUE;
	return FALSE;
}
static inline MonoInst*
mono_inst_next (MonoInst *ins, int filter)
{
	do {
		ins = ins->next;
	} while (mono_inst_filter (ins, filter));
	return ins;
}
static inline MonoInst*
mono_inst_prev (MonoInst *ins, int filter)
{
	do {
		ins = ins->prev;
	} while (mono_inst_filter (ins, filter));
	return ins;
}
static inline MonoInst*
mono_bb_first_inst (MonoBasicBlock *bb, int filter)
{
	MonoInst *ins = bb->code;
	if (mono_inst_filter (ins, filter))
		ins = mono_inst_next (ins, filter);
	return ins;
}
static inline MonoInst*
mono_bb_last_inst (MonoBasicBlock *bb, int filter)
{
	MonoInst *ins = bb->last_ins;
	if (mono_inst_filter (ins, filter))
		ins = mono_inst_prev (ins, filter);
	return ins;
}
void        mini_profiler_emit_enter (MonoCompile *cfg);
void        mini_profiler_emit_leave (MonoCompile *cfg, MonoInst *ret);
void        mini_profiler_emit_tail_call (MonoCompile *cfg, MonoMethod *target);
void        mini_profiler_emit_call_finally (MonoCompile *cfg, MonoMethodHeader *header, unsigned char *ip, guint32 index, MonoExceptionClause *clause);
/* Charge one call, and one turn of a loop whose body spans il_bytes, against
 * the method's way out of tier 0. Both do nothing while cfg is translating an
 * inlined callee's own body.
 *
 * The charge sits behind a branch, so the block being built has to be one a
 * branch can end. The initlocals block is not. */
void        mini_tier0_emit_entry_counter (MonoCompile *cfg);
void        mini_tier0_emit_loop_counter (MonoCompile *cfg, int32_t il_bytes);
/* graph dumping */
void mono_cfg_dump_create_context (MonoCompile *cfg);
void mono_cfg_dump_begin_group (MonoCompile *cfg);
void mono_cfg_dump_close_group (MonoCompile *cfg);
void mono_cfg_dump_ir (MonoCompile *cfg, const char *phase_name);
MonoInst* mono_find_spvar_for_region        (MonoCompile *cfg, int region);
MonoInst* mono_find_exvar_for_offset        (MonoCompile *cfg, int offset);
int mono_get_block_region_notry (MonoCompile *cfg, int region);
void mono_bblock_add_inst (MonoBasicBlock *bb, MonoInst *inst);
void      mono_bblock_insert_after_ins      (MonoBasicBlock *bb, MonoInst *ins, MonoInst *ins_to_insert);
void      mono_bblock_insert_before_ins     (MonoBasicBlock *bb, MonoInst *ins, MonoInst *ins_to_insert);
void      mono_verify_bblock                (MonoBasicBlock *bb);
void      mono_verify_cfg                   (MonoCompile *cfg);
void      mono_constant_fold                (MonoCompile *cfg);
MonoInst* mono_constant_fold_ins            (MonoCompile *cfg, MonoInst *ins, MonoInst *arg1, MonoInst *arg2, gboolean overwrite);
int       mono_eval_cond_branch             (MonoInst *branch);
int mono_is_power_of_two (guint32 val);
void      mono_cprop_local                  (MonoCompile *cfg, MonoBasicBlock *bb, MonoInst **acp, int acp_size);
MonoInst* mono_compile_create_var (MonoCompile *cfg, MonoType *type, int opcode);
MonoInst* mono_compile_create_var_for_vreg  (MonoCompile *cfg, MonoType *type, int opcode, int vreg);
void      mono_compile_make_var_load        (MonoCompile *cfg, MonoInst *dest, gssize var_index);
MonoInst* mini_get_int_to_float_spill_area  (MonoCompile *cfg);
MonoType* mono_type_from_stack_type         (MonoInst *ins);
guint32 mono_alloc_ireg  (MonoCompile *cfg);
guint32 mono_alloc_lreg  (MonoCompile *cfg);
guint32 mono_alloc_freg  (MonoCompile *cfg);
guint32 mono_alloc_preg  (MonoCompile *cfg);
guint32   mono_alloc_dreg                   (MonoCompile *cfg, MonoStackType stack_type);
guint32 mono_alloc_ireg_ref (MonoCompile *cfg);
guint32 mono_alloc_ireg_mp (MonoCompile *cfg);
guint32 mono_alloc_ireg_copy (MonoCompile *cfg, guint32 vreg);
void      mono_mark_vreg_as_ref             (MonoCompile *cfg, int vreg);
void      mono_mark_vreg_as_mp              (MonoCompile *cfg, int vreg);
void      mono_link_bblock                  (MonoCompile *cfg, MonoBasicBlock *from, MonoBasicBlock* to);
void      mono_unlink_bblock                (MonoCompile *cfg, MonoBasicBlock *from, MonoBasicBlock* to);
gboolean  mono_bblocks_linked               (MonoBasicBlock *bb1, MonoBasicBlock *bb2);
void      mono_remove_bblock                (MonoCompile *cfg, MonoBasicBlock *bb);
void      mono_nullify_basic_block          (MonoBasicBlock *bb);
void      mono_merge_basic_blocks           (MonoCompile *cfg, MonoBasicBlock *bb, MonoBasicBlock *bbn);
void      mono_optimize_branches            (MonoCompile *cfg);
void      mono_blockset_print               (MonoCompile *cfg, MonoBitSet *set, const char *name, guint idom);
void      mono_print_ins_index              (int i, MonoInst *ins);
GString  *mono_print_ins_index_strbuf       (int i, MonoInst *ins);
void      mono_print_ins                    (MonoInst *ins);
void      mono_print_bb                     (MonoBasicBlock *bb, const char *msg);
void      mono_print_code                   (MonoCompile *cfg, const char *msg);
const char* mono_inst_name (int op);
int       mono_op_to_op_imm                 (int opcode);
int       mono_op_imm_to_op                 (int opcode);
int       mono_load_membase_to_load_mem     (int opcode);
guint     mono_type_to_load_membase         (MonoCompile *cfg, MonoType *type);
guint     mono_type_to_store_membase        (MonoCompile *cfg, MonoType *type);
guint32   mono_type_to_stloc_coerce         (MonoType *type);
guint     mini_type_to_stind                (MonoCompile* cfg, MonoType *type);
MonoStackType mini_type_to_stack_type       (MonoCompile *cfg, MonoType *t);
MonoJitInfo* mini_lookup_method             (MonoDomain *domain, MonoMethod *method, MonoMethod *shared);
guint32   mono_reverse_branch_op            (guint32 opcode);
void      mono_disassemble_code             (MonoCompile *cfg, guint8 *code, int size, char *id, FILE *out);
MonoJumpInfoTarget mono_call_to_patch       (MonoCallInst *call);
void      mono_call_add_patch_info          (MonoCompile *cfg, MonoCallInst *call, int ip);
void mono_add_patch_info (MonoCompile *cfg, int ip, MonoJumpInfoType type, gconstpointer target);
void mono_add_patch_info_rel (MonoCompile *cfg, int ip, MonoJumpInfoType type, gconstpointer target, int relocation);
void      mono_remove_patch_info            (MonoCompile *cfg, int ip);
GList    *mono_varlist_insert_sorted        (MonoCompile *cfg, GList *list, MonoMethodVar *mv, int sort_type);
GList    *mono_varlist_sort                 (MonoCompile *cfg, GList *list, int sort_type);
void      mono_analyze_liveness             (MonoCompile *cfg);
void      mono_analyze_liveness_gc          (MonoCompile *cfg);
void      mono_linear_scan                  (MonoCompile *cfg, GList *vars, GList *regs, regmask_t *used_mask);
void      mono_global_regalloc              (MonoCompile *cfg);
void      mono_create_jump_table            (MonoCompile *cfg, MonoInst *label, MonoBasicBlock **bbs, int num_blocks);
MonoCompile *mini_method_compile            (MonoMethod *method, guint32 opts, MonoDomain *domain, JitFlags flags, int parts, int aot_method_index);
void      mono_destroy_compile              (MonoCompile *cfg);
void      mono_empty_compile              (MonoCompile *cfg);
MonoJitICallInfo *mono_find_jit_opcode_emulation (int opcode);
void	  mono_print_ins_index (int i, MonoInst *ins);
void	  mono_print_ins (MonoInst *ins);
gboolean mono_compile_is_broken (MonoCompile *cfg, MonoMethod *method, gboolean fail_compile);
MonoInst *mono_get_got_var (MonoCompile *cfg);
void      mono_add_seq_point (MonoCompile *cfg, MonoBasicBlock *bb, MonoInst *ins, int native_offset);
void      mono_add_var_location (MonoCompile *cfg, MonoInst *var, gboolean is_reg, int reg, int offset, int from, int to);
MonoInst* mono_emit_jit_icall_id (MonoCompile *cfg, MonoJitICallId jit_icall_id, MonoInst **args);
#define mono_emit_jit_icall(cfg, name, args) (mono_emit_jit_icall_id ((cfg), MONO_JIT_ICALL_ ## name, (args)))
MonoInst* mono_emit_jit_icall_by_info (MonoCompile *cfg, int il_offset, MonoJitICallInfo *info, MonoInst **args);
MonoInst* mono_emit_method_call (MonoCompile *cfg, MonoMethod *method, MonoInst **args, MonoInst *this_ins);
gboolean  mini_class_is_system_array (MonoClass *klass);
void      mono_linterval_add_range          (MonoCompile *cfg, MonoLiveInterval *interval, int from, int to);
void      mono_linterval_print              (MonoLiveInterval *interval);
void      mono_linterval_print_nl (MonoLiveInterval *interval);
gboolean  mono_linterval_covers             (MonoLiveInterval *interval, int pos);
gint32    mono_linterval_get_intersect_pos  (MonoLiveInterval *i1, MonoLiveInterval *i2);
void      mono_linterval_split              (MonoCompile *cfg, MonoLiveInterval *interval, MonoLiveInterval **i1, MonoLiveInterval **i2, int pos);
void      mono_liveness_handle_exception_clauses (MonoCompile *cfg);
gpointer mono_realloc_native_code (MonoCompile *cfg);
void      mono_register_opcode_emulation    (int opcode, const char* name, MonoMethodSignature *sig, gpointer func, gboolean no_throw);
void      mono_draw_graph                   (MonoCompile *cfg, MonoGraphOptions draw_options);
void      mono_add_ins_to_end               (MonoBasicBlock *bb, MonoInst *inst);
void      mono_replace_ins                  (MonoCompile *cfg, MonoBasicBlock *bb, MonoInst *ins, MonoInst **prev, MonoBasicBlock *first_bb, MonoBasicBlock *last_bb);
void      mini_register_opcode_emulation (int opcode, MonoJitICallInfo *jit_icall_info, const char *name, MonoMethodSignature *sig, gpointer func, const char *symbol, gboolean no_throw);
/* A template cannot have C linkage, so step outside the guard for it. */
G_END_DECLS
#ifdef __cplusplus
template <typename T>
inline void
mini_register_opcode_emulation (int opcode, MonoJitICallInfo *jit_icall_info, const char *name, MonoMethodSignature *sig, T func, const char *symbol, gboolean no_throw)
{
	mini_register_opcode_emulation (opcode, jit_icall_info, name, sig, (gpointer)func, symbol, no_throw);
}
#endif // __cplusplus
G_BEGIN_DECLS
gpointer          mono_create_delegate_virtual_trampoline (MonoDomain *domain, MonoClass *klass, MonoMethod *method);
const char       *mono_regname_full (int reg, int bank);
gint32*           mono_allocate_stack_slots (MonoCompile *cfg, gboolean backward, guint32 *stack_size, guint32 *stack_align);
void              mono_local_regalloc (MonoCompile *cfg, MonoBasicBlock *bb);
MonoInst         *mono_branch_optimize_exception_target (MonoCompile *cfg, MonoBasicBlock *bb, const char * exname);
void              mono_remove_critical_edges (MonoCompile *cfg);
MonoJumpInfo *    mono_patch_info_new (MonoMemPool *mp, int ip, MonoJumpInfoType type, gconstpointer target);
gpointer          mono_resolve_patch_target (MonoMethod *method, MonoDomain *domain, guint8 *code, MonoJumpInfo *patch_info, gboolean run_cctors, MonoError *error);
int               mini_class_check_context_used (MonoCompile *cfg, MonoClass *klass);
int               mini_method_check_context_used (MonoCompile *cfg, MonoMethod *method);
gboolean          mini_method_call_passes_rgctx (MonoCompile *cfg, MonoMethod *cmethod);
void              mini_type_from_op (MonoCompile *cfg, MonoInst *ins, MonoInst *src1, MonoInst *src2);
void              mini_set_inline_failure (MonoCompile *cfg, const char *msg);
void              mini_test_tailcall (MonoCompile *cfg, gboolean tailcall);
gboolean          mini_should_check_stack_pointer (MonoCompile *cfg);
MonoInst*         mini_emit_box (MonoCompile *cfg, MonoInst *val, MonoClass *klass, int context_used);
void              mini_emit_memcpy (MonoCompile *cfg, int destreg, int doffset, int srcreg, int soffset, int size, int align);
void              mini_emit_memset (MonoCompile *cfg, int destreg, int offset, int size, int val, int align);
void              mini_emit_stobj (MonoCompile *cfg, MonoInst *dest, MonoInst *src, MonoClass *klass, gboolean native);
void              mini_emit_initobj (MonoCompile *cfg, MonoInst *dest, const guchar *ip, MonoClass *klass);
void              mini_emit_init_rvar (MonoCompile *cfg, int dreg, MonoType *rtype);
int               mini_emit_sext_index_reg (MonoCompile *cfg, MonoInst *index);
MonoInst*         mini_emit_ldelema_1_ins (MonoCompile *cfg, MonoClass *klass, MonoInst *arr, MonoInst *index, gboolean bcheck, gboolean bounded);
MonoInst*         mini_emit_get_gsharedvt_info_klass (MonoCompile *cfg, MonoClass *klass, MonoRgctxInfoType rgctx_type);
MonoInst*         mini_emit_get_rgctx_method (MonoCompile *cfg, int context_used,
											  MonoMethod *cmethod, MonoRgctxInfoType rgctx_type);
void              mini_emit_tailcall_parameters (MonoCompile *cfg, MonoMethodSignature *sig);
MonoCallInst *    mini_emit_call_args (MonoCompile *cfg, MonoMethodSignature *sig,
									   MonoInst **args, gboolean calli, gboolean virtual_, gboolean tailcall,
									   gboolean rgctx, gboolean unbox_trampoline, MonoMethod *target);
MonoInst*         mini_emit_calli (MonoCompile *cfg, MonoMethodSignature *sig, MonoInst **args, MonoInst *addr, MonoInst *imt_arg, MonoInst *rgctx_arg);
MonoInst*         mini_emit_calli_full (MonoCompile *cfg, MonoMethodSignature *sig, MonoInst **args, MonoInst *addr,
										MonoInst *imt_arg, MonoInst *rgctx_arg, gboolean tailcall);
MonoInst*         mini_emit_method_call_full (MonoCompile *cfg, MonoMethod *method, MonoMethodSignature *sig, gboolean tailcall,
											  MonoInst **args, MonoInst *this_ins, MonoInst *imt_arg, MonoInst *rgctx_arg);
MonoMethod*       mini_synchronized_target (MonoCompile *cfg, MonoMethod *target);
MonoInst*         mini_emit_abs_call (MonoCompile *cfg, MonoJumpInfoType patch_type, gconstpointer data,
									  MonoMethodSignature *sig, MonoInst **args);
MonoInst*         mono_emit_native_call (MonoCompile *cfg, gconstpointer func, MonoMethodSignature *sig,
										 MonoInst **args);
MonoInst*         mini_emit_extra_arg_calli (MonoCompile *cfg, MonoMethodSignature *fsig, MonoInst **orig_args, int arg_reg, MonoInst *call_target);
MonoInst*         mini_emit_llvmonly_calli (MonoCompile *cfg, MonoMethodSignature *fsig, MonoInst **args, MonoInst *addr);
MonoInst*         mini_emit_llvmonly_virtual_call (MonoCompile *cfg, MonoMethod *cmethod, MonoMethodSignature *fsig, int context_used, MonoInst **sp);
MonoInst*         mini_emit_memory_barrier (MonoCompile *cfg, int kind);
MonoInst*         mini_emit_storing_write_barrier (MonoCompile *cfg, MonoInst *ptr, MonoInst *value);
void              mini_emit_write_barrier (MonoCompile *cfg, MonoInst *ptr, MonoInst *value);
MonoInst*         mini_emit_memory_load (MonoCompile *cfg, MonoType *type, MonoInst *src, int offset, int ins_flag);
void              mini_emit_memory_store (MonoCompile *cfg, MonoType *type, MonoInst *dest, MonoInst *value, int ins_flag);
void              mini_emit_memory_copy_bytes (MonoCompile *cfg, MonoInst *dest, MonoInst *src, MonoInst *size, int ins_flag);
void              mini_emit_memory_init_bytes (MonoCompile *cfg, MonoInst *dest, MonoInst *value, MonoInst *size, int ins_flag);
void              mini_emit_memory_copy (MonoCompile *cfg, MonoInst *dest, MonoInst *src, MonoClass *klass, gboolean native, int ins_flag);
MonoInst*         mini_emit_array_store (MonoCompile *cfg, MonoClass *klass, MonoInst **sp, gboolean safety_checks);
MonoInst*         mini_emit_inst_for_method (MonoCompile *cfg, MonoMethod *cmethod, MonoMethodSignature *fsig, MonoInst **args);
MonoInst*         mini_emit_inst_for_ctor (MonoCompile *cfg, MonoMethod *cmethod, MonoMethodSignature *fsig, MonoInst **args);
/* True if calling CMETHOD reports something about its CALLER's stack frame. */
gboolean          mini_method_reports_caller_frame (MonoMethod *cmethod);
/* True if METHOD's body calls such a method, so METHOD must keep its frame. */
gboolean          mini_method_body_reports_caller_frame (MonoMethod *method);
#ifndef ENABLE_NETCORE
MonoInst*         mini_emit_inst_for_sharable_method (MonoCompile *cfg, MonoMethod *cmethod, MonoMethodSignature *fsig, MonoInst **args);
#endif
MonoInst*         mini_emit_inst_for_field_load (MonoCompile *cfg, MonoClassField *field);
MonoInst*         mini_handle_enum_has_flag (MonoCompile *cfg, MonoClass *klass, MonoInst *enum_this, int enum_val_reg, MonoInst *enum_flag);
MonoMethod*       mini_get_memcpy_method (void);
MonoMethod*       mini_get_memset_method (void);
int               mini_class_check_context_used (MonoCompile *cfg, MonoClass *klass);
CompRelation mono_opcode_to_cond (int opcode);
CompType          mono_opcode_to_type (int opcode, int cmp_opcode);
CompRelation      mono_negate_cond (CompRelation cond);
int               mono_op_imm_to_op (int opcode);
void              mono_decompose_op_imm (MonoCompile *cfg, MonoBasicBlock *bb, MonoInst *ins);
void              mono_peephole_ins (MonoBasicBlock *bb, MonoInst *ins);
int mini_exception_id_by_name (const char *name);
gboolean mini_type_is_hfa (MonoType *t, int *out_nfields, int *out_esize);
int               mono_method_to_ir (MonoCompile *cfg, MonoMethod *method, MonoBasicBlock *start_bblock, MonoBasicBlock *end_bblock, 
									 MonoInst *return_var, MonoInst **inline_args,
									 guint inline_offset, gboolean is_virtual_call);
//the following methods could just be renamed/moved from method-to-ir.c
int               mini_inline_method (MonoCompile *cfg, MonoMethod *cmethod, MonoMethodSignature *fsig, MonoInst **sp, guchar *ip,
									  guint real_offset, gboolean inline_always);
MonoInst*         mini_emit_get_rgctx_klass (MonoCompile *cfg, int context_used, MonoClass *klass, MonoRgctxInfoType rgctx_type);
MonoInst*         mini_emit_runtime_constant (MonoCompile *cfg, MonoJumpInfoType patch_type, gpointer data);
void              mini_save_cast_details (MonoCompile *cfg, MonoClass *klass, int obj_reg, gboolean null_check);
void              mini_reset_cast_details (MonoCompile *cfg);
void              mini_emit_class_check (MonoCompile *cfg, int klass_reg, MonoClass *klass);
gboolean          mini_class_has_reference_variant_generic_argument (MonoCompile *cfg, MonoClass *klass, int context_used);
MonoInst         *mono_decompose_opcode (MonoCompile *cfg, MonoInst *ins);
void              mono_decompose_long_opts (MonoCompile *cfg);
void              mono_decompose_vtype_opts (MonoCompile *cfg);
void              mono_decompose_array_access_opts (MonoCompile *cfg);
void              mono_decompose_soft_float (MonoCompile *cfg);
void              mono_local_emulate_ops (MonoCompile *cfg);
void              mono_handle_global_vregs (MonoCompile *cfg);
void              mono_spill_global_vars (MonoCompile *cfg, gboolean *need_local_opts);
void              mono_allocate_gsharedvt_vars (MonoCompile *cfg);
void              mono_if_conversion (MonoCompile *cfg);
/* Delegates */
char*             mono_get_delegate_virtual_invoke_impl_name (gboolean load_imt_reg, int offset);
gpointer          mono_get_delegate_virtual_invoke_impl  (MonoMethodSignature *sig, MonoMethod *method);
void      mono_codegen                          (MonoCompile *cfg);
void mono_call_inst_add_outarg_reg (MonoCompile *cfg, MonoCallInst *call, int vreg, int hreg, int bank);
void      mono_call_inst_add_outarg_vt          (MonoCompile *cfg, MonoCallInst *call, MonoInst *outarg_vt);
gpointer  mono_arch_get_get_tls_tramp (void);
GList    *mono_arch_get_allocatable_int_vars    (MonoCompile *cfg);
GList    *mono_arch_get_global_int_regs         (MonoCompile *cfg);
guint32   mono_arch_regalloc_cost               (MonoCompile *cfg, MonoMethodVar *vmv);
guint8   *mono_arch_emit_prolog                 (MonoCompile *cfg);
void      mono_arch_emit_epilog                 (MonoCompile *cfg);
void      mono_arch_emit_exceptions             (MonoCompile *cfg);
void      mono_arch_lowering_pass               (MonoCompile *cfg, MonoBasicBlock *bb);
void      mono_arch_peephole_pass_1             (MonoCompile *cfg, MonoBasicBlock *bb);
void      mono_arch_peephole_pass_2             (MonoCompile *cfg, MonoBasicBlock *bb);
void      mono_arch_output_basic_block          (MonoCompile *cfg, MonoBasicBlock *bb);
void      mono_arch_fill_argument_info          (MonoCompile *cfg);
void      mono_arch_allocate_vars               (MonoCompile *m);
int       mono_arch_get_argument_info           (MonoMethodSignature *csig, int param_count, MonoJitArgumentInfo *arg_info);
void      mono_arch_emit_call                   (MonoCompile *cfg, MonoCallInst *call);
void      mono_arch_emit_outarg_vt              (MonoCompile *cfg, MonoInst *ins, MonoInst *src);
void      mono_arch_emit_setret                 (MonoCompile *cfg, MonoMethod *method, MonoInst *val);
MonoInst *mono_arch_emit_inst_for_method        (MonoCompile *cfg, MonoMethod *cmethod, MonoMethodSignature *fsig, MonoInst **args);
void      mono_arch_decompose_opts              (MonoCompile *cfg, MonoInst *ins);
void      mono_arch_decompose_long_opts         (MonoCompile *cfg, MonoInst *ins);
GSList*   mono_arch_get_delegate_invoke_impls   (void);
LLVMCallInfo* mono_arch_get_llvm_call_info (MonoCompile *cfg, MonoMethodSignature *sig);
guint8*   mono_arch_emit_load_got_addr          (guint8 *start, guint8 *code, MonoCompile *cfg, MonoJumpInfo **ji);
void      mono_arch_set_target                  (char *mtriple);
gboolean  mono_arch_opcode_needs_emulation      (MonoCompile *cfg, int opcode);
gboolean  mono_arch_tailcall_supported          (MonoCompile *cfg, MonoMethodSignature *caller_sig, MonoMethodSignature *callee_sig, gboolean virtual_);
int       mono_arch_translate_tls_offset        (int offset);
gboolean  mono_arch_opcode_supported            (int opcode);
gpointer  mono_arch_get_throw_exception_by_name (void);
gpointer  mono_arch_get_throw_pending_exception (MonoTrampInfo **info, gboolean aot);
gboolean mono_arch_is_inst_imm                  (int opcode, int imm_opcode, gint64 imm);
void     mono_arch_invalidate_method            (MonoJitInfo *ji, void *func, gpointer func_arg);
guint32  mono_arch_get_patch_offset             (guint8 *code);
gpointer*mono_arch_get_delegate_method_ptr_addr (guint8* code, host_mgreg_t *regs);
void mono_arch_create_vars   (MonoCompile *cfg);
void     mono_arch_save_unwind_info             (MonoCompile *cfg);
gpointer  mono_arch_get_llvm_imt_trampoline     (MonoDomain *domain, MonoMethod *method, int vt_offset);
gpointer mono_arch_get_delegate_virtual_invoke_impl (MonoMethodSignature *sig, MonoMethod *method, int offset, gboolean load_imt_reg);
void    mono_arch_notify_pending_exc            (MonoThreadInfo *info);
gpointer mono_altstack_restore_prot             (host_mgreg_t *regs, guint8 *code, gpointer *tramp_data, guint8* tramp);
void     mono_llvm_reset_exception              (void);
void mono_decompose_typechecks (MonoCompile *cfg);
/* Dominator/SSA methods */
void        mono_compile_dominator_info         (MonoCompile *cfg, int dom_flags);
void        mono_compute_natural_loops          (MonoCompile *cfg);
MonoBitSet* mono_compile_iterated_dfrontier     (MonoCompile *cfg, MonoBitSet *set);
void        mono_ssa_compute                    (MonoCompile *cfg);
void        mono_ssa_remove                     (MonoCompile *cfg);
void        mono_ssa_remove_gsharedvt           (MonoCompile *cfg);
void        mono_ssa_cprop                      (MonoCompile *cfg);
void        mono_ssa_deadce                     (MonoCompile *cfg);
void        mono_ssa_strength_reduction         (MonoCompile *cfg);
void        mono_free_loop_info                 (MonoCompile *cfg);
void        mono_ssa_loop_invariant_code_motion (MonoCompile *cfg);
void        mono_ssa_compute2                   (MonoCompile *cfg);
void        mono_ssa_remove2                    (MonoCompile *cfg);
void        mono_ssa_cprop2                     (MonoCompile *cfg);
void        mono_ssa_deadce2                    (MonoCompile *cfg);
void      mono_debug_init_method                (MonoCompile *cfg, MonoBasicBlock *start_block,
						 guint32 breakpoint_id);
void      mono_debug_open_method                (MonoCompile *cfg);
void      mono_debug_close_method               (MonoCompile *cfg);
void      mono_debug_free_method                (MonoCompile *cfg);
void      mono_debug_open_block                 (MonoCompile *cfg, MonoBasicBlock *bb, guint32 address);
void      mono_debug_record_line_number         (MonoCompile *cfg, MonoInst *ins, guint32 address);
void      mono_debug_serialize_debug_info       (MonoCompile *cfg, guint8 **out_buf, guint32 *buf_len);
void      mono_debug_add_aot_method             (MonoDomain *domain,
						 MonoMethod *method, guint8 *code_start, 
						 guint8 *debug_info, guint32 debug_info_len);
gboolean
mono_tailcall_print_enabled (void);
void
mono_tailcall_print (const char *format, ...);
gboolean
mono_is_supported_tailcall_helper (gboolean value, const char *svalue);
#define IS_SUPPORTED_TAILCALL(x) (mono_is_supported_tailcall_helper((x), #x))
extern void
mono_perform_abc_removal (MonoCompile *cfg);
extern void
mono_local_cprop (MonoCompile *cfg);
extern void
mono_local_deadce (MonoCompile *cfg);
void
mono_local_alias_analysis (MonoCompile *cfg);
gpointer mono_helper_get_rgctx_other_ptr (MonoClass *caller_class, MonoVTable *vtable,
					  guint32 token, guint32 token_source, guint32 rgctx_type,
					  gint32 rgctx_index);
MonoMethod* mini_get_shared_method_to_register (MonoMethod *method);
void mini_type_to_eval_stack_type (MonoCompile *cfg, MonoType *type, MonoInst *inst);
guint mono_type_to_regmove (MonoCompile *cfg, MonoType *type);
void mono_cfg_add_try_hole (MonoCompile *cfg, MonoExceptionClause *clause, guint8 *start, MonoBasicBlock *bb);
void mono_cfg_set_exception (MonoCompile *cfg, MonoExceptionType type);
void mono_cfg_set_exception_invalid_program (MonoCompile *cfg, char *msg);
void mono_update_jit_stats (MonoCompile *cfg);
void mono_save_seq_point_info (MonoCompile *cfg, MonoJitInfo *jinfo);
void mono_save_il_offset_map (MonoCompile *cfg);
void mono_bb_deduplicate_op_il_seq_points (MonoCompile *cfg, MonoBasicBlock *bb);
gboolean mini_gsharedvt_runtime_invoke_supported (MonoMethodSignature *sig);
MonoCPUFeatures mini_get_cpu_features (MonoCompile* cfg);
enum {
	SIMD_COMP_EQ,
	SIMD_COMP_LT,
	SIMD_COMP_LE,
	SIMD_COMP_UNORD,
	SIMD_COMP_NEQ,
	SIMD_COMP_NLT,
	SIMD_COMP_NLE,
	SIMD_COMP_ORD
};
enum {
	SIMD_PREFETCH_MODE_NTA,
	SIMD_PREFETCH_MODE_0,
	SIMD_PREFETCH_MODE_1,
	SIMD_PREFETCH_MODE_2,
};
/* SIMD operations */
typedef enum {
	SIMD_OP_LLVM_FABS,
	SIMD_OP_LLVM_DABS,
	SIMD_OP_LLVM_I8ABS,
	SIMD_OP_LLVM_I16ABS,
	SIMD_OP_LLVM_I32ABS,
	SIMD_OP_LLVM_I64ABS,
	SIMD_OP_SSE_CVTSS2SI,
	SIMD_OP_SSE_CVTTSS2SI,
	SIMD_OP_SSE_CVTSS2SI64,
	SIMD_OP_SSE_CVTTSS2SI64,
	SIMD_OP_SSE_CVTSD2SI,
	SIMD_OP_SSE_CVTTSD2SI,
	SIMD_OP_SSE_CVTSD2SI64,
	SIMD_OP_SSE_CVTTSD2SI64,
	SIMD_OP_SSE_CVTSD2SS,
	SIMD_OP_SSE_MAXPS,
	SIMD_OP_SSE_MAXSS,
	SIMD_OP_SSE_MINPS,
	SIMD_OP_SSE_MINSS,
	SIMD_OP_SSE_MAXPD,
	SIMD_OP_SSE_MAXSD,
	SIMD_OP_SSE_MINPD,
	SIMD_OP_SSE_MINSD,
	SIMD_OP_SSE_SFENCE,
	SIMD_OP_SSE_LFENCE,
	SIMD_OP_SSE_MFENCE,
	SIMD_OP_SSE_SQRTPS,
	SIMD_OP_SSE_RCPPS,
	SIMD_OP_SSE_RSQRTPS,
	SIMD_OP_SSE_SQRTSS,
	SIMD_OP_SSE_RCPSS,
	SIMD_OP_SSE_RSQRTSS,
	SIMD_OP_SSE_SQRTPD,
	SIMD_OP_SSE_SQRTSD,
	SIMD_OP_SSE_PMULUDQ,
	SIMD_OP_SSE_PMULHW,
	SIMD_OP_SSE_PMULHUW,
	SIMD_OP_SSE_PMADDWD,
	SIMD_OP_SSE_PACKSSWB,
	SIMD_OP_SSE_PACKSSDW,
	SIMD_OP_SSE_PSRLW_IMM,
	SIMD_OP_SSE_PSRLD_IMM,
	SIMD_OP_SSE_PSRLQ_IMM,
	SIMD_OP_SSE_PSRLW,
	SIMD_OP_SSE_PSRLD,
	SIMD_OP_SSE_PSRLQ,
	SIMD_OP_SSE_PSLLW_IMM,
	SIMD_OP_SSE_PSLLD_IMM,
	SIMD_OP_SSE_PSLLQ_IMM,
	SIMD_OP_SSE_PSLLW,
	SIMD_OP_SSE_PSLLD,
	SIMD_OP_SSE_PSLLQ,
	SIMD_OP_SSE_PSRAW_IMM,
	SIMD_OP_SSE_PSRAD_IMM,
	SIMD_OP_SSE_PSRAW,
	SIMD_OP_SSE_PSRAD,
	SIMD_OP_SSE_PSADBW,
	SIMD_OP_SSE_ADDSUBPS,
	SIMD_OP_SSE_ADDSUBPD,
	SIMD_OP_SSE_HADDPS,
	SIMD_OP_SSE_HADDPD,
	SIMD_OP_SSE_PHADDW,
	SIMD_OP_SSE_PHADDD,
	SIMD_OP_SSE_PHSUBW,
	SIMD_OP_SSE_PHSUBD,
	SIMD_OP_SSE_HSUBPS,
	SIMD_OP_SSE_HSUBPD,
	SIMD_OP_SSE_PHADDSW,
	SIMD_OP_SSE_PHSUBSW,
	SIMD_OP_SSE_PSIGNB,
	SIMD_OP_SSE_PSIGNW,
	SIMD_OP_SSE_PSIGND,
	SIMD_OP_SSE_PMADDUBSW,
	SIMD_OP_SSE_PMULHRSW,
	SIMD_OP_SSE_LDDQU,
	SIMD_OP_SSE_TESTC,
	SIMD_OP_SSE_TESTNZ,
	SIMD_OP_SSE_TESTZ,
	SIMD_OP_SSE_PACKUSDW,
	SIMD_OP_SSE_PHMINPOSUW,
	SIMD_OP_AES_IMC,
	SIMD_OP_AES_ENC,
	SIMD_OP_AES_ENCLAST,
	SIMD_OP_AES_DEC,
	SIMD_OP_AES_DECLAST,
	SIMD_OP_ARM64_CRC32B,
	SIMD_OP_ARM64_CRC32H,
	SIMD_OP_ARM64_CRC32W,
	SIMD_OP_ARM64_CRC32X,
	SIMD_OP_ARM64_CRC32CB,
	SIMD_OP_ARM64_CRC32CH,
	SIMD_OP_ARM64_CRC32CW,
	SIMD_OP_ARM64_CRC32CX,
	SIMD_OP_ARM64_RBIT32,
	SIMD_OP_ARM64_RBIT64,
	SIMD_OP_ARM64_SHA1C,
	SIMD_OP_ARM64_SHA1H,
	SIMD_OP_ARM64_SHA1M,
	SIMD_OP_ARM64_SHA1P,
	SIMD_OP_ARM64_SHA1SU0,
	SIMD_OP_ARM64_SHA1SU1,
	SIMD_OP_ARM64_SHA256H,
	SIMD_OP_ARM64_SHA256H2,
	SIMD_OP_ARM64_SHA256SU0,
	SIMD_OP_ARM64_SHA256SU1
} SimdOp;
const char *mono_arch_xregname (int reg);
#ifdef MONO_ARCH_SIMD_INTRINSICS
void        mono_simd_simplify_indirection (MonoCompile *cfg);
void        mono_simd_decompose_intrinsic (MonoCompile *cfg, MonoBasicBlock *bb, MonoInst *ins);
MonoInst*   mono_emit_simd_intrinsics (MonoCompile *cfg, MonoMethod *cmethod, MonoMethodSignature *fsig, MonoInst **args);
MonoInst*   mono_emit_simd_field_load (MonoCompile *cfg, MonoClassField *field, MonoInst *addr);
void        mono_simd_intrinsics_init (void);
#endif
MonoInst*   mono_emit_native_types_intrinsics (MonoCompile *cfg, MonoMethod *cmethod, MonoMethodSignature *fsig, MonoInst **args);

#endif /* __MONO_MINI_TIER0_COMPILE_H__ */

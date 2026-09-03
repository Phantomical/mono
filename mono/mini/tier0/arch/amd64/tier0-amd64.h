/**
 * \file
 * \brief What the classic code generator needs of the amd64 port.
 *
 * mini-amd64.h holds what the rest of the runtime reads.
 */

#ifndef __MONO_MINI_TIER0_AMD64_H__
#define __MONO_MINI_TIER0_AMD64_H__

#include "mini-amd64.h"

#ifndef DISABLE_SIMD
#define MONO_ARCH_NEED_SIMD_BANK 1
#define MONO_ARCH_USE_SHARED_FP_SIMD_BANK 1
#endif

#define MONO_ARCH_CPU_SPEC mono_amd64_desc

#define MONO_ARCH_FP_RETURN_REG AMD64_XMM0

#ifdef TARGET_WIN32
/* xmm5 is used as a scratch register */
#define MONO_ARCH_CALLEE_FREGS 0x1f
/* xmm6:xmm15 */
#define MONO_ARCH_CALLEE_SAVED_FREGS (0xffff - 0x3f)
#define MONO_ARCH_FP_SCRATCH_REG AMD64_XMM5
#else
/* xmm15 is used as a scratch register */
#define MONO_ARCH_CALLEE_FREGS 0x7fff
#define MONO_ARCH_CALLEE_SAVED_FREGS 0
#define MONO_ARCH_FP_SCRATCH_REG AMD64_XMM15
#endif

#define MONO_MAX_XREGS MONO_MAX_FREGS

#define MONO_ARCH_CALLEE_XREGS MONO_ARCH_CALLEE_FREGS
#define MONO_ARCH_CALLEE_SAVED_XREGS MONO_ARCH_CALLEE_SAVED_FREGS

#define MONO_ARCH_CALLEE_REGS AMD64_CALLEE_REGS
#define MONO_ARCH_CALLEE_SAVED_REGS AMD64_CALLEE_SAVED_REGS

#define MONO_ARCH_USE_FPSTACK FALSE

#define MONO_ARCH_INST_FIXED_REG(desc) ((desc == '\0') ? -1 : ((desc == 'i' ? -1 : ((desc == 'a') ? AMD64_RAX : ((desc == 's') ? AMD64_RCX : ((desc == 'd') ? AMD64_RDX : ((desc == 'A') ? MONO_AMD64_ARG_REG1 : -1)))))))

/* RDX is clobbered by the opcode implementation before accessing sreg2 */
#define MONO_ARCH_INST_SREG2_MASK(ins) (((ins [MONO_INST_CLOB] == 'a') || (ins [MONO_INST_CLOB] == 'd')) ? (1 << AMD64_RDX) : 0)

#define MONO_ARCH_INST_IS_REGPAIR(desc) FALSE
#define MONO_ARCH_INST_REGPAIR_REG2(desc,hreg1) (-1)

/* fixme: align to 16byte instead of 32byte (we align to 32byte to get
 * reproduceable results for benchmarks */
#define MONO_ARCH_CODE_ALIGNMENT 32

typedef struct MonoCompileArch {
	gint32 localloc_offset;
	gint32 reg_save_area_offset;
	gint32 stack_alloc_size;
	gint32 sp_fp_offset;
	guint32 saved_iregs;
	gboolean omit_fp;
	gboolean omit_fp_computed;
	CallInfo *cinfo;
	gint32 async_point_count;
	MonoInst *vret_addr_loc;
	MonoInst *seq_point_info_var;
	MonoInst *ss_tramp_var;
	MonoInst *bp_tramp_var;
	MonoInst *lmf_var;
#ifdef HOST_WIN32
	struct _UNWIND_INFO* unwindinfo;
#endif
} MonoCompileArch;

#define MONO_ARCH_HAVE_INVALIDATE_METHOD 1
#define MONO_ARCH_HAVE_FULL_AOT_TRAMPOLINES 1

#define MONO_ARCH_HAVE_CMOV_OPS 1
#define MONO_ARCH_HAVE_EXCEPTIONS_INIT 1
#define MONO_ARCH_HAVE_GENERALIZED_IMT_TRAMPOLINE 1
#define MONO_ARCH_HAVE_GET_TRAMPOLINES 1

#define MONO_ARCH_DYN_CALL_PARAM_AREA 0

#define MONO_ARCH_HAVE_CARD_TABLE_WBARRIER 1
#define MONO_ARCH_GC_MAPS_SUPPORTED 1

#define MONO_ARCH_HAVE_CREATE_LLVM_NATIVE_THUNK 1
#define MONO_ARCH_HAVE_OP_TAILCALL_MEMBASE 1
#define MONO_ARCH_HAVE_OP_TAILCALL_REG 1

#define MONO_ARCH_HAVE_OP_GENERIC_CLASS_INIT 1
#define MONO_ARCH_HAVE_GENERAL_RGCTX_LAZY_FETCH_TRAMPOLINE 1

#define MONO_ARCH_FLOAT32_SUPPORTED 1

/* Used for optimization, not complete */
#define MONO_ARCH_IS_OP_MEMBASE(opcode) ((opcode) == OP_X86_PUSH_MEMBASE)

#define MONO_ARCH_EMIT_BOUNDS_CHECK(cfg, array_reg, offset, index_reg, ex_name) do { \
            MonoInst *inst; \
            MONO_INST_NEW ((cfg), inst, OP_AMD64_ICOMPARE_MEMBASE_REG); \
            inst->inst_basereg = array_reg; \
            inst->inst_offset = offset; \
            inst->sreg2 = index_reg; \
            MONO_ADD_INS ((cfg)->cbb, inst); \
            MONO_EMIT_NEW_COND_EXC (cfg, LE_UN, ex_name); \
       } while (0)

// Does the ABI have a volatile non-parameter register, so tailcall
// can pass context to generics or interfaces?
#define MONO_ARCH_HAVE_VOLATILE_NON_PARAM_REGISTER 1

#if defined(TARGET_WIN32) && !defined(DISABLE_JIT)

guint
mono_arch_unwindinfo_init_method_unwind_info (gpointer cfg);

#endif /* defined(TARGET_WIN32) && !defined(DISABLE_JIT) */

#endif /* __MONO_MINI_TIER0_AMD64_H__ */

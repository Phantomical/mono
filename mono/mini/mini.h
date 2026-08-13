/**
 * \file
 * Copyright 2002-2003 Ximian Inc
 * Copyright 2003-2011 Novell Inc
 * Copyright 2011 Xamarin Inc
 * Licensed under the MIT license. See LICENSE file in the project root for full license information.
 */
#ifndef __MONO_MINI_H__
#define __MONO_MINI_H__

#include "config.h"
#include <glib.h>
#include <signal.h>
#ifdef HAVE_SYS_TYPES_H
#include <sys/types.h>
#endif

#include <mono/utils/mono-forward-internal.h>
#include <mono/metadata/loader.h>
#include <mono/metadata/mempool.h>
#include <mono/utils/monobitset.h>
#include <mono/metadata/class.h>
#include <mono/metadata/object.h>
#include <mono/metadata/opcodes.h>
#include <mono/metadata/tabledefs.h>
#include <mono/metadata/domain-internals.h>
#include "mono/metadata/class-internals.h"
#include "mono/metadata/class-init.h"
#include "mono/metadata/object-internals.h"
#include <mono/metadata/profiler-private.h>
#include <mono/metadata/debug-helpers.h>
#include <mono/metadata/abi-details.h>
#include <mono/utils/mono-compiler.h>
#include <mono/utils/mono-machine.h>
#include <mono/utils/mono-stack-unwinding.h>
#include <mono/utils/mono-threads.h>
#include <mono/utils/mono-threads-coop.h>
#include <mono/utils/mono-tls.h>
#include <mono/utils/atomic.h>
#include <mono/utils/mono-jemalloc.h>
#include <mono/utils/mono-conc-hashtable.h>
#include <mono/utils/mono-signal-handler.h>
#include <mono/utils/ftnptr.h>
#include <mono/metadata/icalls.h>

// Forward declare so that mini-*.h can have pointers to them.
// CallInfo is presently architecture specific.
typedef struct MonoInst MonoInst;
typedef struct CallInfo CallInfo;
typedef struct SeqPointInfo SeqPointInfo;

#include "mini-arch.h"
#include "mini-unwind.h"
#include "jit.h"

#include "mono/metadata/tabledefs.h"
#include "mono/metadata/marshal.h"
#include "mono/metadata/security-manager.h"
#include "mono/metadata/exception.h"
#include "mono/metadata/callspec.h"
#include "mono/metadata/icall-signatures.h"

/*
 * Everything below declares C functions. mini.h is included from C++ too
 * (mono/llvm/), where without this guard the declarations would take C++
 * linkage and calls would emit mangled references that do not resolve against
 * mono's C definitions. G_BEGIN_DECLS expands to nothing when compiling C, so
 * this is a no-op for every existing C consumer.
 */
G_BEGIN_DECLS

/*
 * The mini code should not have any compile time dependencies on the GC being used, so the same object file from mini/
 * can be linked into both mono and mono-sgen.
 */
#if !defined(MONO_DLL_EXPORT) || !defined(_MSC_VER)
#if defined(HAVE_BOEHM_GC) || defined(HAVE_SGEN_GC)
#error "The code in mini/ should not depend on these defines."
#endif
#endif

#if !defined(DISABLE_TASKLETS) && defined(MONO_ARCH_SUPPORT_TASKLETS)
#if defined(__GNUC__)
#define MONO_SUPPORT_TASKLETS 1
#elif defined(HOST_WIN32)
#define MONO_SUPPORT_TASKLETS 1
// Replace some gnu intrinsics needed for tasklets with MSVC equivalents.
#define __builtin_extract_return_addr(x) x
#define __builtin_return_address(x) _ReturnAddress()
#define __builtin_frame_address(x) _AddressOfReturnAddress()
#endif
#endif

#define NOT_IMPLEMENTED do { g_assert_not_reached (); } while (0)

#ifndef DISABLE_AOT
#define MONO_USE_AOT_COMPILER
#endif

/* Remap printf to g_print (we use a mix of these in the mini code) */
#ifdef HOST_ANDROID
#define printf g_print
#endif

#define MONO_TYPE_IS_PRIMITIVE(t) ((!(t)->byref && ((((t)->type >= MONO_TYPE_BOOLEAN && (t)->type <= MONO_TYPE_R8) || ((t)->type >= MONO_TYPE_I && (t)->type <= MONO_TYPE_U)))))

typedef struct
{
	MonoClass *klass;
	MonoMethod *method;
} MonoClassMethodPair;

typedef struct
{
	MonoClass *klass;
	MonoMethod *method;
	gboolean is_virtual;
} MonoDelegateClassMethodPair;

typedef struct {
	MonoJitInfo *ji;
	MonoCodeManager *code_mp;
} MonoJitDynamicMethodInfo;

/* An extension of MonoGenericParamFull used in generic sharing */
typedef struct {
	MonoGenericParamFull param;
	MonoGenericParam *parent;
} MonoGSharedGenericParam;

/* Contains a list of ips which needs to be patched when a method is compiled */
typedef struct {
	GSList *list;
} MonoJumpList;

/* Arch-specific */
typedef struct {
	int dummy;
} MonoDynCallInfo;

/*
 * Information about a stack frame.
 * FIXME This typedef exists only to avoid tons of code rewriting
 */
typedef MonoStackFrameInfo StackFrameInfo;

/*
 * Pull the list of opcodes
 */
#define OPDEF(a,b,c,d,e,f,g,h,i,j) \
	a = i,

enum {
#include "mono/cil/opcode.def"
	CEE_LASTOP
};
#undef OPDEF

#define MONO_METHOD_IS_FINAL(m) (((m)->flags & METHOD_ATTRIBUTE_FINAL) || ((m)->klass && (mono_class_get_flags ((m)->klass) & TYPE_ATTRIBUTE_SEALED)))

#ifdef MONO_ARCH_SIMD_INTRINSICS

#define MONO_CLASS_IS_SIMD(cfg, klass) (((cfg)->opt & MONO_OPT_SIMD) && m_class_is_simd_type (klass))

#else

#define MONO_CLASS_IS_SIMD(cfg, klass) (0)

#endif

typedef struct MonoBasicBlock MonoBasicBlock;

extern MonoCallSpec *mono_jit_trace_calls;
extern MonoMethodDesc *mono_inject_async_exc_method;
extern int mono_inject_async_exc_pos;
extern MonoMethodDesc *mono_break_at_bb_method;
extern int mono_break_at_bb_bb_num;
extern gboolean mono_do_x86_stack_align;
extern int mini_verbose;
extern int valgrind_register;

/* Generic sharing */

/*
 * Flags for which contexts were used in inflating a generic.
 */
enum {
	MONO_GENERIC_CONTEXT_USED_CLASS = 1,
	MONO_GENERIC_CONTEXT_USED_METHOD = 2
};

enum {
	/* Cannot be 0 since this is stored in rgctx slots, and 0 means an unitialized rgctx slot */
	MONO_GSHAREDVT_BOX_TYPE_VTYPE = 1,
	MONO_GSHAREDVT_BOX_TYPE_REF = 2,
	MONO_GSHAREDVT_BOX_TYPE_NULLABLE = 3
};

typedef enum {
	MONO_RGCTX_INFO_STATIC_DATA                  = 0,
	MONO_RGCTX_INFO_KLASS                        = 1,
	MONO_RGCTX_INFO_ELEMENT_KLASS                = 2,
	MONO_RGCTX_INFO_VTABLE                       = 3,
	MONO_RGCTX_INFO_TYPE                         = 4,
	MONO_RGCTX_INFO_REFLECTION_TYPE              = 5,
	MONO_RGCTX_INFO_METHOD                       = 6,
	MONO_RGCTX_INFO_GENERIC_METHOD_CODE          = 7,
	MONO_RGCTX_INFO_GSHAREDVT_OUT_WRAPPER        = 8,
	MONO_RGCTX_INFO_CLASS_FIELD                  = 9,
	MONO_RGCTX_INFO_METHOD_RGCTX                 = 10,
	MONO_RGCTX_INFO_METHOD_CONTEXT               = 11,
	MONO_RGCTX_INFO_REMOTING_INVOKE_WITH_CHECK   = 12,
	MONO_RGCTX_INFO_METHOD_DELEGATE_CODE         = 13,
	MONO_RGCTX_INFO_CAST_CACHE                   = 14,
	MONO_RGCTX_INFO_ARRAY_ELEMENT_SIZE           = 15,
	MONO_RGCTX_INFO_VALUE_SIZE                   = 16,
	/* +1 to avoid zero values in rgctx slots */
	MONO_RGCTX_INFO_FIELD_OFFSET                 = 17,
	/* Either the code for a gsharedvt method, or the address for a gsharedvt-out trampoline for the method */
	/* In llvmonly mode, this is a function descriptor */
	MONO_RGCTX_INFO_METHOD_GSHAREDVT_OUT_TRAMPOLINE = 18,
	/* Same for virtual calls */
	/* In llvmonly mode, this is a function descriptor */
	MONO_RGCTX_INFO_METHOD_GSHAREDVT_OUT_TRAMPOLINE_VIRT = 19,
	/* Same for calli, associated with a signature */
	MONO_RGCTX_INFO_SIG_GSHAREDVT_OUT_TRAMPOLINE_CALLI = 20,
	MONO_RGCTX_INFO_SIG_GSHAREDVT_IN_TRAMPOLINE_CALLI = 21,
	/* One of MONO_GSHAREDVT_BOX_TYPE */
	MONO_RGCTX_INFO_CLASS_BOX_TYPE                = 22,
	/* Resolves to a MonoGSharedVtMethodRuntimeInfo */
	MONO_RGCTX_INFO_METHOD_GSHAREDVT_INFO         = 23,
	MONO_RGCTX_INFO_LOCAL_OFFSET                  = 24,
	MONO_RGCTX_INFO_MEMCPY                        = 25,
	MONO_RGCTX_INFO_BZERO                         = 26,
	/* The address of Nullable<T>.Box () */
	/* In llvmonly mode, this is a function descriptor */
	MONO_RGCTX_INFO_NULLABLE_CLASS_BOX            = 27,
	MONO_RGCTX_INFO_NULLABLE_CLASS_UNBOX          = 28,
	/* MONO_PATCH_INFO_VCALL_METHOD */
	/* In llvmonly mode, this is a function descriptor */
	MONO_RGCTX_INFO_VIRT_METHOD_CODE              = 29,
	/*
	 * MONO_PATCH_INFO_VCALL_METHOD
	 * Same as MONO_RGCTX_INFO_CLASS_BOX_TYPE, but for the class
	 * which implements the method.
	 */
	MONO_RGCTX_INFO_VIRT_METHOD_BOX_TYPE          = 30,
	/* Resolve to 2 (TRUE) or 1 (FALSE) */
	MONO_RGCTX_INFO_CLASS_IS_REF_OR_CONTAINS_REFS = 31,
	/* The MonoDelegateTrampInfo instance */
	MONO_RGCTX_INFO_DELEGATE_TRAMP_INFO           = 32,
	/* Same as MONO_PATCH_INFO_METHOD_FTNDESC */
	MONO_RGCTX_INFO_METHOD_FTNDESC                = 33,
	/* mono_type_size () for a class */
	MONO_RGCTX_INFO_CLASS_SIZEOF                  = 34
} MonoRgctxInfoType;

typedef struct _MonoRuntimeGenericContextInfoTemplate {
	MonoRgctxInfoType info_type;
	gpointer data;
	struct _MonoRuntimeGenericContextInfoTemplate *next;
} MonoRuntimeGenericContextInfoTemplate;

typedef struct {
	MonoClass *next_subclass;
	MonoRuntimeGenericContextInfoTemplate *infos;
	GSList *method_templates;
} MonoRuntimeGenericContextTemplate;

typedef struct {
	MonoVTable *class_vtable; /* must be the first element */
	MonoGenericInst *method_inst;
	gpointer infos [MONO_ZERO_LEN_ARRAY];
} MonoMethodRuntimeGenericContext;

#define MONO_SIZEOF_METHOD_RUNTIME_GENERIC_CONTEXT (MONO_ABI_SIZEOF (MonoMethodRuntimeGenericContext) - MONO_ZERO_LEN_ARRAY * TARGET_SIZEOF_VOID_P)

#define MONO_RGCTX_SLOT_MAKE_RGCTX(i)	(i)
#define MONO_RGCTX_SLOT_MAKE_MRGCTX(i)	((i) | 0x80000000)
#define MONO_RGCTX_SLOT_INDEX(s)	((s) & 0x7fffffff)
#define MONO_RGCTX_SLOT_IS_MRGCTX(s)	(((s) & 0x80000000) ? TRUE : FALSE)

#define MONO_GSHAREDVT_DEL_INVOKE_VT_OFFSET -2

typedef struct {
	MonoMethod *method;
	MonoRuntimeGenericContextInfoTemplate *entries;
	int num_entries, count_entries;
} MonoGSharedVtMethodInfo;

/* This is used by gsharedvt methods to allocate locals and compute local offsets */
typedef struct {
	int locals_size;
	/*
	 * The results of resolving the entries in MOonGSharedVtMethodInfo->entries.
	 * We use this instead of rgctx slots since these can be loaded using a load instead
	 * of a call to an rgctx fetch trampoline.
	 */
	gpointer entries [MONO_ZERO_LEN_ARRAY];
} MonoGSharedVtMethodRuntimeInfo;

typedef struct
{
	MonoMethod *invoke;
	MonoMethod *method;
	MonoMethodSignature *invoke_sig;
	MonoMethodSignature *sig;
	gpointer method_ptr;
	gpointer invoke_impl;
	gpointer impl_this;
	gpointer impl_nothis;
} MonoDelegateTrampInfo;

/*
 * A function descriptor, which is a function address + argument pair.
 * In llvm-only mode, these are used instead of trampolines to pass
 * extra arguments to runtime functions/methods.
 */
typedef struct
{
	gpointer addr;
	gpointer arg;
} MonoFtnDesc;

typedef enum {
#define PATCH_INFO(a,b) MONO_PATCH_INFO_ ## a,
#include "patch-info.h"
#undef PATCH_INFO
	MONO_PATCH_INFO_NUM
} MonoJumpInfoType;

typedef struct MonoJumpInfoRgctxEntry MonoJumpInfoRgctxEntry;
typedef struct MonoJumpInfo MonoJumpInfo;
typedef struct MonoJumpInfoGSharedVtCall MonoJumpInfoGSharedVtCall;

// This ordering is mimiced in MONO_JIT_ICALLS.
typedef enum {
	MONO_TRAMPOLINE_JIT      = 0,
	MONO_TRAMPOLINE_JUMP     = 1,
	MONO_TRAMPOLINE_RGCTX_LAZY_FETCH = 2,
	MONO_TRAMPOLINE_AOT      = 3,
	MONO_TRAMPOLINE_AOT_PLT  = 4,
	MONO_TRAMPOLINE_DELEGATE = 5,
	MONO_TRAMPOLINE_GENERIC_VIRTUAL_REMOTING = 6,
	MONO_TRAMPOLINE_VCALL    = 7,
	MONO_TRAMPOLINE_NUM      = 8,
} MonoTrampolineType;

// Assuming MONO_TRAMPOLINE_JIT / MONO_JIT_ICALL_generic_trampoline_jit are first.
#if __cplusplus
g_static_assert (MONO_TRAMPOLINE_JIT == 0);
#endif
#define mono_trampoline_type_to_jit_icall_id(a) ((a) + MONO_JIT_ICALL_generic_trampoline_jit)

/* These trampolines return normally to their caller */
#define MONO_TRAMPOLINE_TYPE_MUST_RETURN(t)		\
	((t) == MONO_TRAMPOLINE_RGCTX_LAZY_FETCH)

/* optimization flags */
#define OPTFLAG(id,shift,name,descr) MONO_OPT_ ## id = 1 << shift,
enum {
#include "optflags-def.h"
	MONO_OPT_LAST
};

/*
 * What the translator is handed for one method.  The classic compiler kept its
 * whole pipeline state here; the LLVM back end reads the method, its header,
 * the domain the code is being compiled for and the optimization set, and
 * nothing else.
 */
typedef struct {
	MonoMethod      *method;
	MonoMethodHeader *header;
	MonoDomain      *domain;
	guint32          opt;
} MonoCompile;
typedef struct {
	gint32 methods_compiled;
	gint32 methods_aot;
	gint32 methods_aot_llvm;
	gint32 methods_lookups;
	gint32 allocate_var;
	gint32 cil_code_size;
	gint32 native_code_size;
	gint32 code_reallocs;
	gint32 max_code_size_ratio;
	gint32 biggest_method_size;
	gint32 allocated_code_size;
	gint32 allocated_seq_points_size;
	gint32 inlineable_methods;
	gint32 inlined_methods;
	gint32 basic_blocks;
	gint32 max_basic_blocks;
	gint32 locals_stack_size;
	gint32 regvars;
	gint32 generic_virtual_invocations;
	gint32 alias_found;
	gint32 alias_removed;
	gint32 loads_eliminated;
	gint32 stores_eliminated;
	gint32 optimized_divisions;
	gint32 methods_with_llvm;
	gint32 methods_without_llvm;
	gint32 methods_with_interp;
	char *max_ratio_method;
	char *biggest_method;
	gint64 jit_method_to_ir;
	gint64 jit_liveness_handle_exception_clauses;
	gint64 jit_handle_out_of_line_bblock;
	gint64 jit_decompose_long_opts;
	gint64 jit_decompose_typechecks;
	gint64 jit_local_cprop;
	gint64 jit_local_emulate_ops;
	gint64 jit_optimize_branches;
	gint64 jit_handle_global_vregs;
	gint64 jit_local_deadce;
	gint64 jit_local_alias_analysis;
	gint64 jit_if_conversion;
	gint64 jit_bb_ordering;
	gint64 jit_compile_dominator_info;
	gint64 jit_compute_natural_loops;
	gint64 jit_insert_safepoints;
	gint64 jit_ssa_compute;
	gint64 jit_ssa_cprop;
	gint64 jit_ssa_deadce;
	gint64 jit_perform_abc_removal;
	gint64 jit_ssa_remove;
	gint64 jit_local_cprop2;
	gint64 jit_handle_global_vregs2;
	gint64 jit_local_deadce2;
	gint64 jit_optimize_branches2;
	gint64 jit_decompose_vtype_opts;
	gint64 jit_decompose_array_access_opts;
	gint64 jit_liveness_handle_exception_clauses2;
	gint64 jit_analyze_liveness;
	gint64 jit_linear_scan;
	gint64 jit_arch_allocate_vars;
	gint64 jit_spill_global_vars;
	gint64 jit_local_cprop3;
	gint64 jit_local_deadce3;
	gint64 jit_codegen;
	gint64 jit_create_jit_info;
	gint64 jit_gc_create_gc_map;
	gint64 jit_save_seq_point_info;
	gint64 jit_time;
	gboolean enabled;
} MonoJitStats;

extern MonoJitStats mono_jit_stats;

 /* 
  * Information about a trampoline function.
  */
struct MonoTrampInfo
{
	/* 
	 * The native code of the trampoline. Not owned by this structure.
	 */
 	guint8 *code;
 	guint32 code_size;
	/*
	 * The name of the trampoline, for diagnostics. Owned by this
	 * structure.
	 */
 	char *name;
	/* 
	 * Patches required by the trampoline when aot-ing. Owned by this structure.
	 */
	MonoJumpInfo *ji;
	/*
	 * Unwind information. Owned by this structure.
	 */
	GSList *unwind_ops;

	MonoJitICallInfo *jit_icall_info;

	/*
	 * The method the trampoline is associated with, if any.
	 */
	MonoMethod *method;

	 /*
	  * Encoded unwind info loaded from AOT images
	  */
	 guint8 *uw_info;
	 guint32 uw_info_len;
	 /* Whenever uw_info is owned by this structure */
	 gboolean owns_uw_info;
};

/* profiler support */
void        mini_add_profiler_argument (const char *desc);
void        mini_profiler_context_enable (void);
gpointer    mini_profiler_context_get_this (MonoProfilerCallContext *ctx);
gpointer    mini_profiler_context_get_argument (MonoProfilerCallContext *ctx, guint32 pos);
gpointer    mini_profiler_context_get_local (MonoProfilerCallContext *ctx, guint32 pos);
gpointer    mini_profiler_context_get_result (MonoProfilerCallContext *ctx);
void        mini_profiler_context_free_buffer (gpointer buffer);

/* helper methods */

MonoJitInfo* mini_lookup_method             (MonoDomain *domain, MonoMethod *method, MonoMethod *shared);

gboolean  mini_should_insert_breakpoint (MonoMethod *method);
int mono_target_pagesize (void);

void              mono_trampolines_init (void);
void              mono_trampolines_cleanup (void);
guint8 *          mono_get_trampoline_code (MonoTrampolineType tramp_type);
gpointer          mono_create_specific_trampoline (gpointer arg1, MonoTrampolineType tramp_type, MonoDomain *domain, guint32 *code_len);
gpointer          mono_create_jump_trampoline (MonoDomain *domain, 
											   MonoMethod *method, 
											   gboolean add_sync_wrapper,
											   MonoError *error);
gpointer mono_create_jit_trampoline (MonoDomain *domain, MonoMethod *method, MonoError *error);
gpointer          mono_create_jit_trampoline_from_token (MonoImage *image, guint32 token);
gpointer          mono_create_delegate_trampoline (MonoDomain *domain, MonoClass *klass);
MonoDelegateTrampInfo* mono_create_delegate_trampoline_info (MonoDomain *domain, MonoClass *klass, MonoMethod *method);
gpointer          mono_create_rgctx_lazy_fetch_trampoline (guint32 offset);
gpointer          mono_create_ftnptr_arg_trampoline (gpointer arg, gpointer addr);
guint32           mono_find_rgctx_lazy_fetch_trampoline_by_addr (gconstpointer addr);
gpointer          mono_magic_trampoline (host_mgreg_t *regs, guint8 *code, gpointer arg, guint8* tramp);
#ifndef DISABLE_REMOTING
gpointer          mono_generic_virtual_remoting_trampoline (host_mgreg_t *regs, guint8 *code, MonoMethod *m, guint8 *tramp);
#endif
gpointer          mono_delegate_trampoline (host_mgreg_t *regs, guint8 *code, gpointer *tramp_data, guint8* tramp);
gpointer          mono_aot_trampoline (host_mgreg_t *regs, guint8 *code, guint8 *token_info, 
									   guint8* tramp);
gpointer          mono_aot_plt_trampoline (host_mgreg_t *regs, guint8 *code, guint8 *token_info, 
										   guint8* tramp);
gconstpointer     mono_get_trampoline_func (MonoTrampolineType tramp_type);
gpointer          mini_get_vtable_trampoline (MonoVTable *vt, int slot_index);
const char*       mono_get_generic_trampoline_simple_name (MonoTrampolineType tramp_type);
const char*       mono_get_generic_trampoline_name (MonoTrampolineType tramp_type);
char*             mono_get_rgctx_fetch_trampoline_name (int slot);
gpointer          mini_get_single_step_trampoline (void);
gpointer          mini_get_breakpoint_trampoline (void);
gpointer*         mono_arch_get_single_step_tramp_addr (void);
gpointer          mini_add_method_trampoline (MonoMethod *m, gpointer compiled_method, gboolean add_unbox_tramp);
gboolean          mini_jit_info_is_gsharedvt (MonoJitInfo *ji);
gpointer*         mini_resolve_imt_method (MonoVTable *vt, gpointer *vtable_slot, MonoMethod *imt_method, MonoMethod **impl_method, gpointer *out_aot_addr,
					   MonoMethod **variant_iface, MonoError *error);

void*             mono_global_codeman_reserve (int size);

#define mono_global_codeman_reserve(size) (g_cast (mono_global_codeman_reserve ((size))))

void              mono_global_codeman_foreach (MonoCodeManagerFunc func, void *user_data);
gboolean          mono_is_regsize_var (MonoType *t);
MonoUnwindOp     *mono_create_unwind_op (int when, 
										 int tag, int reg, 
										 int val);
void              mono_emit_unwind_op (MonoCompile *cfg, int when, 
									   int tag, int reg, 
									   int val);
MonoTrampInfo*    mono_tramp_info_create (const char *name, guint8 *code, guint32 code_size, MonoJumpInfo *ji, GSList *unwind_ops);
void              mono_tramp_info_free (MonoTrampInfo *info);
void              mono_aot_tramp_info_register (MonoTrampInfo *info, MonoDomain *domain);
void              mono_tramp_info_register (MonoTrampInfo *info, MonoDomain *domain);
MonoJitInfo*      mono_tramp_info_register_reclaimable (MonoDomain *domain, MonoMethod *method, gpointer code, guint32 code_size, const char *name, guint8 *uw_info, guint32 uw_info_len);

/* methods that must be provided by the arch-specific port */
void      mono_arch_init                        (void);
void      mono_arch_finish_init                 (void);
void      mono_arch_cleanup                     (void);
void      mono_arch_cpu_init                    (void);
guint32   mono_arch_cpu_optimizations           (guint32 *exclude_mask);
const char *mono_arch_regname                   (int reg);
const char *mono_arch_fregname                  (int reg);
void      mono_arch_exceptions_init             (void);
guchar*   mono_arch_create_generic_trampoline   (MonoTrampolineType tramp_type, MonoTrampInfo **info, gboolean aot);
gpointer  mono_arch_create_rgctx_lazy_fetch_trampoline (guint32 slot, MonoTrampInfo **info, gboolean aot);
gpointer  mono_arch_create_general_rgctx_lazy_fetch_trampoline (MonoTrampInfo **info, gboolean aot);
guint8*   mono_arch_create_sdb_trampoline (gboolean single_step, MonoTrampInfo **info, gboolean aot);
guint8 *mono_arch_create_llvm_native_thunk (MonoDomain *domain, guint8* addr);
void      mono_arch_patch_code                  (MonoCompile *cfg, MonoMethod *method, MonoDomain *domain, guint8 *code, MonoJumpInfo *ji, gboolean run_cctors, MonoError *error);
void      mono_arch_patch_code_new              (MonoCompile *cfg, MonoDomain *domain, guint8 *code, MonoJumpInfo *ji, gpointer target);
void      mono_arch_flush_icache                (guint8 *code, gint size);
MonoDynCallInfo *mono_arch_dyn_call_prepare     (MonoMethodSignature *sig);
void      mono_arch_dyn_call_free               (MonoDynCallInfo *info);
int       mono_arch_dyn_call_get_buf_size       (MonoDynCallInfo *info);
void      mono_arch_start_dyn_call              (MonoDynCallInfo *info, gpointer **args, guint8 *ret, guint8 *buf);
void      mono_arch_finish_dyn_call             (MonoDynCallInfo *info, guint8 *buf);
guint8*   mono_arch_emit_load_aotconst          (guint8 *start, guint8 *code, MonoJumpInfo **ji, MonoJumpInfoType tramp_type, gconstpointer target);
GSList*   mono_arch_get_cie_program             (void);
gboolean  mono_arch_gsharedvt_sig_supported     (MonoMethodSignature *sig);
gpointer  mono_arch_get_gsharedvt_trampoline    (MonoTrampInfo **info, gboolean aot);
gpointer  mono_arch_get_gsharedvt_call_info     (gpointer addr, MonoMethodSignature *normal_sig, MonoMethodSignature *gsharedvt_sig, gboolean gsharedvt_in, gint32 vcall_offset, gboolean calli);
void     mono_arch_setup_resume_sighandler_ctx  (MonoContext *ctx, gpointer func);
gboolean  mono_arch_have_fast_tls               (void);

#ifdef MONO_ARCH_HAS_REGISTER_ICALL
void      mono_arch_register_icall              (void);
#endif

#ifdef MONO_ARCH_SOFT_FLOAT_FALLBACK
gboolean  mono_arch_is_soft_float               (void);
#else
static inline MONO_ALWAYS_INLINE gboolean
mono_arch_is_soft_float (void)
{
	return FALSE;
}
#endif

/* Soft Debug support */
#ifdef MONO_ARCH_SOFT_DEBUG_SUPPORTED
void      mono_arch_set_breakpoint              (MonoJitInfo *ji, guint8 *ip);
void      mono_arch_clear_breakpoint            (MonoJitInfo *ji, guint8 *ip);
void      mono_arch_start_single_stepping       (void);
void      mono_arch_stop_single_stepping        (void);
gboolean  mono_arch_is_single_step_event        (void *info, void *sigctx);
gboolean  mono_arch_is_breakpoint_event         (void *info, void *sigctx);
void     mono_arch_skip_breakpoint              (MonoContext *ctx, MonoJitInfo *ji);
void     mono_arch_skip_single_step             (MonoContext *ctx);
SeqPointInfo *mono_arch_get_seq_point_info      (MonoDomain *domain, guint8 *code);
#endif

gboolean
mono_arch_unwind_frame (MonoDomain *domain, MonoJitTlsData *jit_tls, 
						MonoJitInfo *ji, MonoContext *ctx, 
						MonoContext *new_ctx, MonoLMF **lmf,
						host_mgreg_t **save_locations,
						StackFrameInfo *frame_info);
gpointer mono_arch_get_call_filter              (MonoTrampInfo **info, gboolean aot);
gpointer mono_arch_get_restore_context          (MonoTrampInfo **info, gboolean aot);
gpointer  mono_arch_get_throw_exception         (MonoTrampInfo **info, gboolean aot);
gpointer  mono_arch_get_rethrow_exception       (MonoTrampInfo **info, gboolean aot);
gpointer  mono_arch_get_rethrow_preserve_exception (MonoTrampInfo **info, gboolean aot);
gpointer  mono_arch_get_throw_corlib_exception  (MonoTrampInfo **info, gboolean aot);
gboolean mono_arch_handle_exception             (void *sigctx, gpointer obj);
void     mono_arch_handle_altstack_exception    (void *sigctx, MONO_SIG_HANDLER_INFO_TYPE *siginfo, gpointer fault_addr, gboolean stack_ovf);
gboolean mono_handle_soft_stack_ovf             (MonoJitTlsData *jit_tls, MonoJitInfo *ji, void *ctx, MONO_SIG_HANDLER_INFO_TYPE *siginfo, guint8* fault_addr);
void     mono_handle_hard_stack_ovf             (MonoJitTlsData *jit_tls, MonoJitInfo *ji, MonoContext *mctx, guint8* fault_addr);
void     mono_arch_undo_ip_adjustment           (MonoContext *ctx);
void     mono_arch_do_ip_adjustment             (MonoContext *ctx);
gpointer mono_arch_ip_from_context              (void *sigctx);
host_mgreg_t mono_arch_context_get_int_reg      (MonoContext *ctx, int reg);
void     mono_arch_context_set_int_reg		(MonoContext *ctx, int reg, host_mgreg_t val);
void     mono_arch_flush_register_windows       (void);
gboolean mono_arch_is_int_overflow              (void *sigctx, void *info);
void     mono_arch_invalidate_method            (MonoJitInfo *ji, void *func, gpointer func_arg);
void     mono_arch_register_lowlevel_calls      (void);
gpointer mono_arch_get_unbox_trampoline         (MonoMethod *m, gpointer addr);
gpointer mono_arch_get_static_rgctx_trampoline  (MonoMemoryManager *mem_manager, gpointer arg, gpointer addr);
gpointer mono_arch_get_ftnptr_arg_trampoline    (MonoMemoryManager *mem_manager, gpointer arg, gpointer addr);
gpointer mono_arch_get_gsharedvt_arg_trampoline (MonoDomain *domain, gpointer arg, gpointer addr);
void     mono_arch_patch_callsite               (guint8 *method_start, guint8 *code, guint8 *addr);
void     mono_arch_patch_plt_entry              (guint8 *code, gpointer *got, host_mgreg_t *regs, guint8 *addr);
void     mono_arch_patch_jump_trampoline        (guint8* jump_tramp, guint8* addr);
int      mono_arch_get_this_arg_reg             (guint8 *code);
gpointer mono_arch_get_this_arg_from_call       (host_mgreg_t *regs, guint8 *code);
gpointer mono_arch_get_delegate_invoke_impl     (MonoMethodSignature *sig, gboolean has_target);
gpointer mono_arch_create_specific_trampoline   (gpointer arg1, MonoTrampolineType tramp_type, MonoMemoryManager *mem_manager, guint32 *code_len);
MonoMethod* mono_arch_find_imt_method           (host_mgreg_t *regs, guint8 *code);
MonoVTable* mono_arch_find_static_call_vtable   (host_mgreg_t *regs, guint8 *code);
gpointer    mono_arch_build_imt_trampoline      (MonoVTable *vtable, MonoDomain *domain, MonoIMTCheckItem **imt_entries, int count, gpointer fail_tramp);
guint8* mono_arch_get_call_target               (guint8 *code);
guint32 mono_arch_get_plt_info_offset           (guint8 *plt_entry, host_mgreg_t *regs, guint8 *code);
GSList *mono_arch_get_trampolines               (gboolean aot);
gpointer mono_arch_get_interp_to_native_trampoline (MonoTrampInfo **info);
gpointer mono_arch_get_native_to_interp_trampoline (MonoTrampInfo **info);

#ifdef MONO_ARCH_HAVE_INTERP_PINVOKE_TRAMP
// Moves data (arguments and return vt address) from the InterpFrame to the CallContext so a pinvoke call can be made.
void mono_arch_set_native_call_context_args     (CallContext *ccontext, gpointer frame, MonoMethodSignature *sig);
// Moves the return value from the InterpFrame to the ccontext, or to the retp (if native code passed the retvt address)
void mono_arch_set_native_call_context_ret      (CallContext *ccontext, gpointer frame, MonoMethodSignature *sig, gpointer retp);
// When entering interp from native, this moves the arguments from the ccontext to the InterpFrame. If we have a return
// vt address, we return it. This ret vt address needs to be passed to mono_arch_set_native_call_context_ret.
gpointer mono_arch_get_native_call_context_args     (CallContext *ccontext, gpointer frame, MonoMethodSignature *sig);
// After the pinvoke call is done, this moves return value from the ccontext to the InterpFrame.
void mono_arch_get_native_call_context_ret      (CallContext *ccontext, gpointer frame, MonoMethodSignature *sig);
#endif

/*New interruption machinery */
void
mono_setup_async_callback (MonoContext *ctx, void (*async_cb)(void *fun), gpointer user_data);

void
mono_arch_setup_async_callback (MonoContext *ctx, void (*async_cb)(void *fun), gpointer user_data);

gboolean
mono_thread_state_init_from_handle (MonoThreadUnwindState *tctx, MonoThreadInfo *info, /*optional*/ void *sigctx);

/* Exception handling */
typedef gboolean (*MonoJitStackWalk)            (StackFrameInfo *frame, MonoContext *ctx, gpointer data);

void     mono_exceptions_init                   (void);
gboolean mono_handle_exception                  (MonoContext *ctx, gpointer obj);
void     mono_handle_native_crash               (const char *signal, MonoContext *mctx, MONO_SIG_HANDLER_INFO_TYPE *siginfo);
MONO_API void     mono_print_thread_dump                 (void *sigctx);
MONO_API void     mono_print_thread_dump_from_ctx        (MonoContext *ctx);
void     mono_walk_stack_with_ctx               (MonoJitStackWalk func, MonoContext *start_ctx, MonoUnwindOptions unwind_options, void *user_data);
void     mono_walk_stack_with_state             (MonoJitStackWalk func, MonoThreadUnwindState *state, MonoUnwindOptions unwind_options, void *user_data);
void     mono_walk_stack                        (MonoJitStackWalk func, MonoUnwindOptions options, void *user_data);
gboolean mono_thread_state_init_from_sigctx     (MonoThreadUnwindState *ctx, void *sigctx);
void     mono_thread_state_init                 (MonoThreadUnwindState *ctx);
gboolean mono_thread_state_init_from_current    (MonoThreadUnwindState *ctx);
gboolean mono_thread_state_init_from_monoctx    (MonoThreadUnwindState *ctx, MonoContext *mctx);

void     mono_setup_altstack                    (MonoJitTlsData *tls);
void     mono_free_altstack                     (MonoJitTlsData *tls);
void     mono_setup_resume_states               (MonoJitTlsData *tls);
void     mono_free_resume_states                (MonoJitTlsData *tls);
MonoJitInfo* mini_jit_info_table_find           (MonoDomain *domain, gpointer addr, MonoDomain **out_domain);
MonoJitInfo* mini_jit_info_table_find_ext       (MonoDomain *domain, gpointer addr, gboolean allow_trampolines, MonoDomain **out_domain);
G_EXTERN_C void mono_resume_unwind              (MonoContext *ctx);

MonoJitInfo * mono_find_jit_info                (MonoDomain *domain, MonoJitTlsData *jit_tls, MonoJitInfo *res, MonoJitInfo *prev_ji, MonoContext *ctx, MonoContext *new_ctx, char **trace, MonoLMF **lmf, int *native_offset, gboolean *managed);
int      mono_jit_info_llvm_il_offset           (MonoJitInfo *ji, guint32 native_offset);

typedef gboolean (*MonoExceptionFrameWalk)      (MonoMethod *method, MonoJitInfo *ji, gpointer ip, size_t native_offset, gboolean managed, gpointer user_data);
MONO_API gboolean mono_exception_walk_trace     (MonoException *ex, MonoExceptionFrameWalk func, gpointer user_data);
void mono_restore_context                       (MonoContext *ctx);
guint8* mono_jinfo_get_unwind_info              (MonoJitInfo *ji, guint32 *unwind_info_len);
int  mono_jinfo_get_epilog_size                 (MonoJitInfo *ji);
G_EXTERN_C void mono_llvm_rethrow_exception     (MonoObject *ex);
G_EXTERN_C void mono_llvm_throw_exception       (MonoObject *ex);
G_EXTERN_C void mono_llvm_throw_corlib_exception (guint32 ex_token_index);
G_EXTERN_C void mono_llvm_resume_exception      (void);
G_EXTERN_C void mono_llvm_clear_exception       (void);
G_EXTERN_C MonoObject *mono_llvm_load_exception (void);
void     mono_llvm_raise_exception              (MonoException *e);
void     mono_llvm_reraise_exception            (MonoException *e);
G_EXTERN_C gint32 mono_llvm_match_exception     (MonoJitInfo *jinfo, guint32 region_start, guint32 region_end, gpointer rgctx, MonoObject *this_obj);

gboolean
mono_find_jit_info_ext (MonoDomain *domain, MonoJitTlsData *jit_tls, 
						MonoJitInfo *prev_ji, MonoContext *ctx,
						MonoContext *new_ctx, char **trace, MonoLMF **lmf,
						host_mgreg_t **save_locations,
						StackFrameInfo *frame);

int
mono_jinfo_get_il_offset (MonoDomain *domain, MonoJitInfo *ji, guint32 native_offset);

struct _MonoDebugSourceLocation *
mono_jinfo_lookup_source_location (MonoDomain *domain, MonoJitInfo *ji, guint32 native_offset);

gpointer mono_get_throw_exception               (void);
gpointer mono_get_rethrow_exception             (void);
gpointer mono_get_rethrow_preserve_exception             (void);
gpointer mono_get_call_filter                   (void);
gpointer mono_get_restore_context               (void);
gpointer mono_get_throw_corlib_exception        (void);
gpointer mono_get_throw_exception_addr          (void);
gpointer mono_get_rethrow_preserve_exception_addr          (void);
ICALL_EXPORT
MonoArray *ves_icall_get_trace                  (MonoException *exc, gint32 skip, MonoBoolean need_file_info);

ICALL_EXPORT
MonoBoolean ves_icall_get_frame_info            (gint32 skip, MonoBoolean need_file_info, 
						 MonoReflectionMethod **method, 
						 gint32 *iloffset, gint32 *native_offset,
						 MonoString **file, gint32 *line, gint32 *column);
void mono_set_cast_details                      (MonoClass *from, MonoClass *to);

/* debugging support */
MONO_API void      mono_debug_print_vars                 (gpointer ip, gboolean only_arguments);
MONO_API void      mono_debugger_run_finally             (MonoContext *start_ctx);

MONO_API gboolean mono_breakpoint_clean_code (guint8 *method_start, guint8 *code, int offset, guint8 *buf, int size);

/* Tracing */
MonoCallSpec *mono_trace_set_options           (const char *options);
gboolean       mono_trace_eval                  (MonoMethod *method);

/* Generic sharing */

void
mono_set_generic_sharing_supported (gboolean supported);

void
mono_set_generic_sharing_vt_supported (gboolean supported);

void
mono_set_partial_sharing_supported (gboolean supported);

gboolean
mono_class_generic_sharing_enabled (MonoClass *klass);

gpointer
mono_class_fill_runtime_generic_context (MonoVTable *class_vtable, guint32 slot, MonoError *error);

gpointer
mono_method_fill_runtime_generic_context (MonoMethodRuntimeGenericContext *mrgctx, guint32 slot, MonoError *error);

const char*
mono_rgctx_info_type_to_str (MonoRgctxInfoType type);

MonoJumpInfoType
mini_rgctx_info_type_to_patch_info_type (MonoRgctxInfoType info_type);

gboolean
mono_method_needs_static_rgctx_invoke (MonoMethod *method, gboolean allow_type_vars);

int
mono_class_rgctx_get_array_size (int n, gboolean mrgctx);

MonoGenericContext
mono_method_construct_object_context (MonoMethod *method);

MonoMethod*
mono_method_get_declaring_generic_method (MonoMethod *method);

int
mono_generic_context_check_used (MonoGenericContext *context);

int
mono_class_check_context_used (MonoClass *klass);

gboolean
mono_generic_context_is_sharable (MonoGenericContext *context, gboolean allow_type_vars);

gboolean
mono_generic_context_is_sharable_full (MonoGenericContext *context, gboolean allow_type_vars, gboolean allow_partial);

gboolean
mono_method_is_generic_impl (MonoMethod *method);

gboolean
mono_method_is_generic_sharable (MonoMethod *method, gboolean allow_type_vars);

gboolean
mono_method_is_generic_sharable_full (MonoMethod *method, gboolean allow_type_vars, gboolean allow_partial, gboolean allow_gsharedvt);

gboolean
mini_class_is_generic_sharable (MonoClass *klass);

gboolean
mini_generic_inst_is_sharable (MonoGenericInst *inst, gboolean allow_type_vars, gboolean allow_partial);

MonoMethod*
mono_class_get_method_generic (MonoClass *klass, MonoMethod *method, MonoError *error);

gboolean
mono_is_partially_sharable_inst (MonoGenericInst *inst);

gboolean
mini_is_gsharedvt_gparam (MonoType *t);

gboolean
mini_is_gsharedvt_inst (MonoGenericInst *inst);

MonoGenericContext* mini_method_get_context (MonoMethod *method);

int mono_method_check_context_used (MonoMethod *method);

gboolean mono_generic_context_equal_deep (MonoGenericContext *context1, MonoGenericContext *context2);

void mono_generic_sharing_init (void);
void mono_generic_sharing_cleanup (void);

MonoClass* mini_class_get_container_class (MonoClass *klass);
MonoGenericContext* mini_class_get_context (MonoClass *klass);

typedef enum {
	SHARE_MODE_NONE = 0x0,
	SHARE_MODE_GSHAREDVT = 0x1,
} GetSharedMethodFlags;

MonoType* mini_get_underlying_type (MonoType *type);
MonoType* mini_type_get_underlying_type (MonoType *type);
MonoClass* mini_get_class (MonoMethod *method, guint32 token, MonoGenericContext *context);
MonoMethod* mini_get_shared_method_full (MonoMethod *method, GetSharedMethodFlags flags, MonoError *error);
MonoType* mini_get_shared_gparam (MonoType *t, MonoType *constraint);
int mini_get_rgctx_entry_slot (MonoJumpInfoRgctxEntry *entry);

int mini_type_stack_size (MonoType *t, int *align);
int mini_type_stack_size_full (MonoType *t, guint32 *align, gboolean pinvoke);

#define MONO_TIME_TRACK(a, phase) \
	{ \
		gint64 start = mono_time_track_start (); \
		(phase) ; \
		mono_time_track_end (&(a), start); \
	}

gint64 mono_time_track_start (void);
void mono_time_track_end (gint64 *time, gint64 start);

gboolean mini_type_is_reference (MonoType *type);
gboolean mini_type_is_vtype (MonoType *t);
gboolean mini_type_var_is_vt (MonoType *type);
gboolean mini_is_gsharedvt_type (MonoType *t);
gboolean mini_is_gsharedvt_klass (MonoClass *klass);
gboolean mini_is_gsharedvt_signature (MonoMethodSignature *sig);
gboolean mini_is_gsharedvt_variable_type (MonoType *t);
gboolean mini_is_gsharedvt_variable_klass (MonoClass *klass);
gboolean mini_is_gsharedvt_sharable_method (MonoMethod *method);
gboolean mini_is_gsharedvt_variable_signature (MonoMethodSignature *sig);
gboolean mini_is_gsharedvt_sharable_inst (MonoGenericInst *inst);
gboolean mini_method_is_default_method (MonoMethod *m);
gboolean mini_method_needs_mrgctx (MonoMethod *m);
gpointer mini_method_get_rgctx (MonoMethod *m);
void mini_init_gsctx (MonoDomain *domain, MonoMemPool *mp, MonoGenericContext *context, MonoGenericSharingContext *gsctx);

gpointer mini_get_gsharedvt_wrapper (gboolean gsharedvt_in, gpointer addr, MonoMethodSignature *normal_sig, MonoMethodSignature *gsharedvt_sig,
									 gint32 vcall_offset, gboolean calli);
MonoMethod* mini_get_gsharedvt_in_sig_wrapper (MonoMethodSignature *sig);
MonoMethod* mini_get_gsharedvt_out_sig_wrapper (MonoMethodSignature *sig);
MonoMethodSignature* mini_get_gsharedvt_out_sig_wrapper_signature (gboolean has_this, gboolean has_ret, int param_count);
G_EXTERN_C void mono_interp_entry_from_trampoline (gpointer ccontext, gpointer imethod);
G_EXTERN_C void mono_interp_to_native_trampoline (gpointer addr, gpointer ccontext);
MonoMethod* mini_get_interp_in_wrapper (MonoMethodSignature *sig);
MonoMethod* mini_get_interp_lmf_wrapper (const char *name, gpointer target);
char* mono_get_method_from_ip (void *ip);

/* SIMD support */

typedef enum {
	/* Used for lazy initialization */
	MONO_CPU_INITED		= 1 << 0,
#if defined(TARGET_X86) || defined(TARGET_AMD64)
	MONO_CPU_X86_SSE	= 1 << 1,
	MONO_CPU_X86_SSE2	= 1 << 2,
	MONO_CPU_X86_PCLMUL	= 1 << 3,
	MONO_CPU_X86_AES	= 1 << 4,
	MONO_CPU_X86_SSE3	= 1 << 5,
	MONO_CPU_X86_SSSE3	= 1 << 6,
	MONO_CPU_X86_SSE41	= 1 << 7,
	MONO_CPU_X86_SSE42	= 1 << 8,
	MONO_CPU_X86_POPCNT	= 1 << 9,
	MONO_CPU_X86_AVX	= 1 << 10,
	MONO_CPU_X86_AVX2	= 1 << 11,
	MONO_CPU_X86_FMA	= 1 << 12,
	MONO_CPU_X86_LZCNT	= 1 << 13,
	MONO_CPU_X86_BMI1	= 1 << 14,
	MONO_CPU_X86_BMI2	= 1 << 15,

	//
	// Dependencies (based on System.Runtime.Intrinsics.X86 class hierarchy):
	//
	// sse
	//   sse2
	//     pclmul
	//     aes
	//     sse3
	//       ssse3     (doesn't include 'pclmul' and 'aes')
	//         sse4.1
	//           sse4.2
	//             popcnt
	//             avx     (doesn't include 'popcnt')
	//               avx2
	//               fma
	// lzcnt
	// bmi1
	// bmi2
	MONO_CPU_X86_SSE_COMBINED         = MONO_CPU_X86_SSE,
	MONO_CPU_X86_SSE2_COMBINED        = MONO_CPU_X86_SSE_COMBINED   | MONO_CPU_X86_SSE2,
	MONO_CPU_X86_PCLMUL_COMBINED      = MONO_CPU_X86_SSE2_COMBINED  | MONO_CPU_X86_PCLMUL,
	MONO_CPU_X86_AES_COMBINED         = MONO_CPU_X86_SSE2_COMBINED  | MONO_CPU_X86_AES,
	MONO_CPU_X86_SSE3_COMBINED        = MONO_CPU_X86_SSE2_COMBINED  | MONO_CPU_X86_SSE3,
	MONO_CPU_X86_SSSE3_COMBINED       = MONO_CPU_X86_SSE3_COMBINED  | MONO_CPU_X86_SSSE3,
	MONO_CPU_X86_SSE41_COMBINED       = MONO_CPU_X86_SSSE3_COMBINED | MONO_CPU_X86_SSE41,
	MONO_CPU_X86_SSE42_COMBINED       = MONO_CPU_X86_SSE41_COMBINED | MONO_CPU_X86_SSE42,
	MONO_CPU_X86_POPCNT_COMBINED      = MONO_CPU_X86_SSE42_COMBINED | MONO_CPU_X86_POPCNT,
	MONO_CPU_X86_AVX_COMBINED         = MONO_CPU_X86_SSE42_COMBINED | MONO_CPU_X86_AVX,
	MONO_CPU_X86_AVX2_COMBINED        = MONO_CPU_X86_AVX_COMBINED   | MONO_CPU_X86_AVX2,
	MONO_CPU_X86_FMA_COMBINED         = MONO_CPU_X86_AVX_COMBINED   | MONO_CPU_X86_FMA,
	MONO_CPU_X86_FULL_SSEAVX_COMBINED = MONO_CPU_X86_FMA_COMBINED   | MONO_CPU_X86_AVX2   | MONO_CPU_X86_PCLMUL 
									  | MONO_CPU_X86_AES            | MONO_CPU_X86_POPCNT | MONO_CPU_X86_FMA,
#endif
#ifdef TARGET_WASM
	MONO_CPU_WASM_SIMD = 1 << 1,
#endif
#ifdef TARGET_ARM64
	MONO_CPU_ARM64_BASE   = 1 << 1,
	MONO_CPU_ARM64_CRC    = 1 << 2,
	MONO_CPU_ARM64_CRYPTO = 1 << 3,
	MONO_CPU_ARM64_ADVSIMD = 1 << 4,
#endif
} MonoCPUFeatures;

G_ENUM_FUNCTIONS (MonoCPUFeatures)

gboolean    mono_class_is_magic_int (MonoClass *klass);
gboolean    mono_class_is_magic_float (MonoClass *klass);
gsize       mini_magic_type_size (MonoType *type);
gboolean    mini_magic_is_int_type (MonoType *t);
gboolean    mini_magic_is_float_type (MonoType *t);
MonoType* mini_native_type_replace_type (MonoType *type);

MonoMethod*
mini_method_to_shared (MonoMethod *method); // null if not shared

static inline gboolean
mini_safepoints_enabled (void)
{
#if defined (TARGET_WASM)
	return FALSE;
#else
	return TRUE;
#endif
}

gpointer
mono_arch_load_function (MonoJitICallId jit_icall_id);

MonoGenericContext
mono_get_generic_context_from_stack_frame (MonoJitInfo *ji, gpointer generic_info);

G_END_DECLS

#endif /* __MONO_MINI_H__ */

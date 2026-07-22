/**
 * \file
 * translator-internal.hpp: the declarations shared between the translator's
 * translation units.
 *
 * translator.cpp was a single 9,496-line file, of which process_bb () alone was
 * 4,383 lines. It is now six translation units, split along the axes the code
 * already had:
 *
 *   translator.cpp             module init/cleanup, the mono_llvm_* entry
 *                              points, emit_method_inner (), the in-process JIT
 *                              module and the AOT refusal stubs.
 *   translator-types.cpp       MonoType/MonoTypeEnum/opcode -> LLVMTypeRef
 *                              mapping and the SIMD tables.
 *   translator-emit.cpp        the general IR-emission helpers: basic blocks,
 *                              convert (), loads and stores, signature
 *                              lowering, allocas, metadata flags.
 *   translator-call.cpp        the method prologue (emit_entry_bb ()), calls
 *                              (process_call ()) and the exception-emission
 *                              helpers.
 *   translator-bb.cpp          process_bb (), the per-instruction translator.
 *   translator-intrinsics.cpp  the llvm.* intrinsic declaration cache.
 *
 * The split is mechanical: every line of executable code is byte-identical to
 * its predecessor and sits in the same relative order. The only source edits
 * were removing the linkage keyword from the 74 functions and 5 objects that
 * now have callers in another translation unit, and relocating two forward
 * declarations so that they travel with the functions they describe.
 *
 * It is NOT byte-identical as an object file, and cannot be. eglib's g_assert ()
 * and g_assert_not_reached () pass __FILE__ and __LINE__ as runtime arguments,
 * so all 144 assertion sites in this translator bake their line number into
 * .text as an immediate; objcopy --strip-debug does not touch those. Moving a
 * line, or renaming the file it lives in, changes the emitted code by
 * construction. Widening linkage changes it again: 24 helpers that were
 * previously inlined away entirely now have out-of-line bodies, four lost their
 * IPA clones, and the two predicate tables moved from .rodata to .data.
 *
 * Everything declared here is internal to mono/mini/llvm. The extern "C"
 * boundary the rest of mono links against is backend.h, and it did not grow to
 * accommodate this split.
 *
 * Copyright 2009-2011 Novell Inc (http://www.novell.com)
 * Copyright 2011 Xamarin Inc (http://www.xamarin.com)
 * Licensed under the MIT license. See LICENSE file in the project root for full license information.
 */

#ifndef __MONO_MINI_LLVM_TRANSLATOR_INTERNAL_HPP__
#define __MONO_MINI_LLVM_TRANSLATOR_INTERNAL_HPP__

#include "config.h"

#include <mono/metadata/debug-helpers.h>
#include <mono/metadata/debug-internals.h>
#include <mono/metadata/mempool-internals.h>
#include <mono/metadata/environment.h>
#include <mono/metadata/object-internals.h>
#include <mono/metadata/abi-details.h>
#include <mono/utils/mono-tls.h>
#include <mono/utils/mono-dl.h>
#include <mono/utils/mono-time.h>
#include <mono/utils/freebsd-dwarf.h>

#ifndef __STDC_LIMIT_MACROS
#define __STDC_LIMIT_MACROS
#endif
#ifndef __STDC_CONSTANT_MACROS
#define __STDC_CONSTANT_MACROS
#endif

#include "llvm-c/BitWriter.h"
#include "llvm-c/Analysis.h"

#include "translator-cpp.hpp"
#include "backend.h"
#include "aot-compiler.h"
#include "mini-llvm.h"
#include "mini-runtime.h"
#include <mono/utils/mono-math.h>

#ifndef DISABLE_JIT


#if defined(TARGET_AMD64) && defined(TARGET_WIN32) && defined(HOST_WIN32) && defined(_MSC_VER)
#define TARGET_X86_64_WIN32_MSVC
#endif

#if defined(TARGET_X86_64_WIN32_MSVC)
#define TARGET_WIN32_MSVC
#endif

#if LLVM_API_VERSION < 610
#error "The version of the mono llvm repository is too old."
#endif

 /*
  * Information associated by mono with LLVM modules.
  */
typedef struct {
	LLVMModuleRef lmodule;
	LLVMValueRef throw_icall, rethrow, throw_corlib_exception;
	/*
	 * Cached signature of throw_icall/rethrow: void (object). Computed once --
	 * mono_metadata_signature_alloc () allocates from corlib's image mempool,
	 * which lives for the process and is never freed, so building it per throw
	 * site would leak (and take the image lock on the JIT path).
	 */
	LLVMTypeRef throw_sig_type;
	GHashTable *llvm_types;
	GHashTable *method_to_lmethod;
	/* Maps got slot index -> LLVMValueRef */
	GHashTable *aotconst_vars;
	char **bb_names;
	int bb_names_len;
	LLVMTypeRef ptr_type;
	MonoEERef *mono_ee;
	LLVMExecutionEngineRef ee;
	int max_got_offset;
	LLVMValueRef *intrins_by_id;
	gpointer gc_poll_cold_wrapper_compiled;

	char *global_prefix;
	/* Written by emit_gc_safepoint_poll ()'s AOT arm; kept with that function. */
	LLVMValueRef gc_poll_cold_wrapper;
	int max_method_idx;
	gboolean static_link;
	GHashTable *idx_to_lmethod;
	LLVMContextRef context;
	GHashTable *objc_selector_to_var;
	GHashTable *got_idx_to_type;
} MonoLLVMModule;

/*
 * Information associated by the backend with mono basic blocks.
 */
typedef struct {
	LLVMBasicBlockRef bblock, end_bblock;
	LLVMValueRef finally_ind;
	gboolean added, invoke_target;
	/* 
	 * If this bblock is the start of a finally clause, this is a list of bblocks it
	 * needs to branch to in ENDFINALLY.
	 */
	GSList *call_handler_return_bbs;
	/*
	 * If this bblock is the start of a finally clause, this is the bblock that
	 * CALL_HANDLER needs to branch to.
	 */
	LLVMBasicBlockRef call_handler_target_bb;
	/* The list of switch statements generated by ENDFINALLY instructions */
	GSList *endfinally_switch_ins_list;
	GSList *phi_nodes;
} BBInfo;

/*
 * A typed pointer value.
 *
 * Under opaque pointers an LLVMValueRef does not carry its pointee type, but
 * LLVMBuildLoad2/GEP2 need it. Pairing the two here means the element type is
 * recorded where the pointer is created and can never be re-derived (and so
 * silently mis-derived) at the point of use.
 */
typedef struct {
	LLVMValueRef value;
	/* The element type of the pointer */
	LLVMTypeRef type;
} Address;

/*
 * Structure containing emit state
 */
typedef struct {
	MonoMemPool *mempool;

	MonoCompile *cfg;
	LLVMValueRef lmethod;
	MonoLLVMModule *module;
	LLVMModuleRef lmodule;
	BBInfo *bblocks;
	int sindex, default_index, ex_index;
	LLVMBuilderRef builder;
	LLVMValueRef *values;
	Address **addresses;
	MonoType **vreg_cli_types;
	LLVMCallInfo *linfo;
	MonoMethodSignature *sig;
	GSList *builders;
	GHashTable *region_to_handler;
	GHashTable *clause_to_handler;
	LLVMBuilderRef alloca_builder;
	LLVMValueRef last_alloca;
	LLVMValueRef rgctx_arg;
	LLVMValueRef this_arg;
	LLVMTypeRef *vreg_types;
	gboolean *is_vphi;
	LLVMTypeRef method_type;
	gboolean *is_dead;
	gboolean *unreachable;
	gboolean has_safepoints;
	int this_arg_pindex, rgctx_arg_pindex;
	LLVMValueRef imt_rgctx_loc;
	GHashTable *llvm_types;
	LLVMValueRef ex_var;
	/*
	 * The function's landingpad personality (an i32-returning `mono_personality`
	 * stub). Created lazily by get_mono_personality() the first time a handler is
	 * emitted and pinned onto lmethod via LLVMSetPersonalityFn, so a method with
	 * several catch clauses defines it (and sets the personality fn) exactly once.
	 */
	LLVMValueRef personality;
	GPtrArray *phi_values;
	GPtrArray *bblock_list;
	char *method_name;
	GHashTable *jit_callees;
	LLVMValueRef long_bb_break_var;
} EmitContext;

typedef struct {
	MonoBasicBlock *bb;
	MonoInst *phi;
	MonoBasicBlock *in_bb;
	int sreg;
} PhiNode;

#if TARGET_SIZEOF_VOID_P == 4
#define GET_LONG_IMM(ins) ((ins)->inst_l)
#else
#define GET_LONG_IMM(ins) ((ins)->inst_imm)
#endif

#define LLVM_INS_INFO(opcode) (&mini_llvm_ins_info [((opcode) - OP_START - 1) * 4])

/*
 * Set to 1 to log every method the translator bails out on, together with the
 * reason recorded in cfg->exception_message. Can also be defined on the command
 * line: make CXXFLAGS='... -DMONO_LLVM_TRACE_FAILURE=1'.
 */
#ifndef MONO_LLVM_TRACE_FAILURE
#define MONO_LLVM_TRACE_FAILURE 0
#endif

#if MONO_LLVM_TRACE_FAILURE
#define TRACE_FAILURE_CFG(cfg, msg) do {					\
		char *trace_failure_name = mono_method_full_name ((cfg)->method, TRUE); \
		printf ("[mono-llvm] disabling llvm for %s: %s\n", trace_failure_name, (msg)); \
		fflush (stdout);						\
		g_free (trace_failure_name);					\
	} while (0)
#else
#define TRACE_FAILURE_CFG(cfg, msg) do { (void)(cfg); (void)(msg); } while (0)
#endif

/*
 * set_failure() traces off the EmitContext; the pre-flight gate in
 * mono_llvm_check_method_supported() only has a MonoCompile. Both funnel into
 * TRACE_FAILURE_CFG so a trace log reads identically for either decline path.
 */
#define TRACE_FAILURE(ctx, msg) TRACE_FAILURE_CFG ((ctx)->cfg, msg)

#ifdef TARGET_X86
#define IS_TARGET_X86 1
#else
#define IS_TARGET_X86 0
#endif

#ifdef TARGET_AMD64
#define IS_TARGET_AMD64 1
#else
#define IS_TARGET_AMD64 0
#endif

#define ctx_ok(ctx) (!(ctx)->cfg->disable_llvm)

/* Defined in translator.cpp. */
/*
 * NOTE: this declaration gives mini_llvm_ins_info external linkage. Without
 * it the definition in translator.cpp is a namespace-scope const array, which
 * in C++ is internal by default - so before the split it could not collide
 * with anything. mono/mini/mini-llvm.c:222 defines the same name at file scope
 * in a C file, i.e. externally. That file is excluded from _SOURCES, so there
 * is no collision today; if it is ever built again, this is where the duplicate
 * symbol will come from.
 */
extern const char mini_llvm_ins_info [];
extern LLVMIntPredicate cond_to_llvm_cond [];
extern LLVMRealPredicate fpcond_to_llvm_cond [];
extern MonoLLVMModule aot_module;
extern GHashTable *intrins_id_to_intrins;
extern LLVMTypeRef sse_i1_t, sse_i2_t, sse_i4_t, sse_i8_t, sse_r4_t, sse_r8_t;

/* Defined in translator-types.cpp. */
void
set_failure (EmitContext *ctx, const char *message);
LLVMValueRef
const_int32 (int v);
LLVMValueRef
const_int64 (int64_t v);
LLVMTypeRef
IntPtrType (void);
LLVMTypeRef
ObjRefType (void);
LLVMTypeRef
ThisType (void);
guint32
get_vtype_size (MonoType *t);
LLVMTypeRef
simd_class_to_llvm_type (EmitContext *ctx, MonoClass *klass);
G_GNUC_UNUSED LLVMTypeRef
type_to_sse_type (int type);
LLVMTypeRef
primitive_type_to_llvm_type (MonoTypeEnum type);
MonoTypeEnum
inst_c1_type (const MonoInst *ins);
LLVMTypeRef
type_to_llvm_type (EmitContext *ctx, MonoType *t);
bool
primitive_type_is_unsigned (MonoTypeEnum t);
bool
type_is_unsigned (EmitContext *ctx, MonoType *t);
LLVMTypeRef
type_to_llvm_arg_type (EmitContext *ctx, MonoType *t);
G_GNUC_UNUSED LLVMTypeRef
llvm_type_to_stack_type (MonoCompile *cfg, LLVMTypeRef type);
LLVMTypeRef
regtype_to_llvm_type (char c);
LLVMTypeRef
op_to_llvm_type (int opcode);
LLVMTypeRef
load_store_to_llvm_type (int opcode, int *size, gboolean *sext, gboolean *zext);
IntrinsicId
ovf_op_to_intrins (int opcode);
IntrinsicId
simd_ins_to_intrins (int opcode);
LLVMTypeRef
simd_op_to_llvm_type (int opcode);
void
set_cold_cconv (LLVMValueRef func);
void
set_call_cold_cconv (LLVMValueRef func);

/* Defined in translator-emit.cpp. */
LLVMBasicBlockRef
get_bb (EmitContext *ctx, MonoBasicBlock *bb);
LLVMBasicBlockRef
get_end_bb (EmitContext *ctx, MonoBasicBlock *bb);
LLVMBasicBlockRef
gen_bb (EmitContext *ctx, const char *prefix);
LLVMValueRef
convert_full (EmitContext *ctx, LLVMValueRef v, LLVMTypeRef dtype, gboolean is_unsigned);
LLVMValueRef
convert (EmitContext *ctx, LLVMValueRef v, LLVMTypeRef dtype);
void
emit_memset (EmitContext *ctx, LLVMBuilderRef builder, LLVMValueRef v, LLVMValueRef size, int alignment);
LLVMValueRef
emit_volatile_load (EmitContext *ctx, int vreg);
void
emit_volatile_store (EmitContext *ctx, int vreg);
LLVMTypeRef
sig_to_llvm_sig_full (EmitContext *ctx, MonoMethodSignature *sig, LLVMCallInfo *cinfo);
LLVMTypeRef
sig_to_llvm_sig (EmitContext *ctx, MonoMethodSignature *sig);
G_GNUC_UNUSED LLVMTypeRef
LLVMFunctionType0 (LLVMTypeRef ReturnType,
				   int IsVarArg);
LLVMBuilderRef
create_builder (EmitContext *ctx);
LLVMValueRef
get_aotconst (EmitContext *ctx, MonoJumpInfoType type, gconstpointer data, LLVMTypeRef llvm_type);
LLVMValueRef
get_jit_callee (EmitContext *ctx, const char *name, LLVMTypeRef llvm_sig, MonoJumpInfoType type, gconstpointer data);
void
set_metadata_flag (LLVMValueRef v, const char *flag_name);
void
set_nontemporal_flag (LLVMValueRef v);
void
set_invariant_load_flag (LLVMValueRef v);
LLVMValueRef
emit_call (EmitContext *ctx, MonoBasicBlock *bb, LLVMBuilderRef *builder_ref, LLVMTypeRef sig, LLVMValueRef callee, LLVMValueRef *args, int pindex);
LLVMValueRef
emit_load (EmitContext *ctx, MonoBasicBlock *bb, LLVMBuilderRef *builder_ref, int size, LLVMTypeRef type, LLVMValueRef addr, LLVMValueRef base, const char *name, gboolean is_faulting, gboolean is_volatile, BarrierKind barrier);
void
emit_store_general (EmitContext *ctx, MonoBasicBlock *bb, LLVMBuilderRef *builder_ref, int size, LLVMValueRef value, LLVMValueRef addr, LLVMValueRef base, gboolean is_faulting, gboolean is_volatile, BarrierKind barrier);
void
emit_store (EmitContext *ctx, MonoBasicBlock *bb, LLVMBuilderRef *builder_ref, int size, LLVMValueRef value, LLVMValueRef addr, LLVMValueRef base, gboolean is_faulting, gboolean is_volatile);
void
emit_cond_system_exception (EmitContext *ctx, MonoBasicBlock *bb, const char *exc_type, LLVMValueRef cmp, gboolean force_explicit);
void
emit_args_to_vtype (EmitContext *ctx, LLVMBuilderRef builder, MonoType *t, LLVMValueRef address, LLVMArgInfo *ainfo, LLVMValueRef *args);
void
emit_vtype_to_args (EmitContext *ctx, LLVMBuilderRef builder, MonoType *t, LLVMValueRef address, LLVMArgInfo *ainfo, LLVMValueRef *args, guint32 *nargs);
LLVMValueRef
build_alloca_llvm_type_name (EmitContext *ctx, LLVMTypeRef t, int align, const char *name);
LLVMValueRef
build_alloca_llvm_type (EmitContext *ctx, LLVMTypeRef t, int align);
LLVMValueRef
build_named_alloca (EmitContext *ctx, MonoType *t, char const *name);
Address*
create_address (EmitContext *ctx, LLVMValueRef value, LLVMTypeRef type);
Address*
build_alloca_address (EmitContext *ctx, MonoType *t);
Address*
build_named_alloca_address (EmitContext *ctx, MonoType *t, const char *name);
LLVMValueRef
emit_gsharedvt_ldaddr (EmitContext *ctx, int vreg);
LLVMValueRef
emit_icall_cold_wrapper (MonoLLVMModule *module, LLVMModuleRef lmodule, MonoJitICallId icall_id, gboolean aot);
void
emit_gc_safepoint_poll (MonoLLVMModule *module, LLVMModuleRef lmodule, MonoCompile *cfg);

/* Defined in translator-call.cpp. */
void
emit_div_check (EmitContext *ctx, LLVMBuilderRef builder, MonoBasicBlock *bb, MonoInst *ins, LLVMValueRef lhs, LLVMValueRef rhs);
void
emit_entry_bb (EmitContext *ctx, LLVMBuilderRef builder);
void
process_call (EmitContext *ctx, MonoBasicBlock *bb, LLVMBuilderRef *builder_ref, MonoInst *ins);
void
emit_throw (EmitContext *ctx, MonoBasicBlock *bb, gboolean rethrow, LLVMValueRef exc);
LLVMValueRef
create_const_vector (LLVMTypeRef t, const int *vals, int count);
LLVMValueRef
create_const_vector_i32 (const int *mask, int count);
LLVMValueRef
create_const_vector_4_i32 (int v0, int v1, int v2, int v3);
LLVMValueRef
create_const_vector_2_i32 (int v0, int v1);
void
emit_handler_start (EmitContext *ctx, MonoBasicBlock *bb, LLVMBuilderRef builder);
LLVMValueRef
get_double_const (MonoCompile *cfg, double val);
LLVMValueRef
get_float_const (MonoCompile *cfg, float val);
LLVMValueRef
call_intrins (EmitContext *ctx, int id, LLVMValueRef *args, const char *name);

/* Defined in translator-bb.cpp. */
void
process_bb (EmitContext *ctx, MonoBasicBlock *bb);

/* Defined in translator-intrinsics.cpp. */
LLVMValueRef
get_intrins (EmitContext *ctx, int id);
void
add_intrinsics (LLVMModuleRef module);
void
add_types (MonoLLVMModule *module);

#endif /* DISABLE_JIT */

#endif /* __MONO_MINI_LLVM_TRANSLATOR_INTERNAL_HPP__ */

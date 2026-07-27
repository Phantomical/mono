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

#include <atomic>
#include <memory>
#include <vector>
#include <unordered_map>
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallVector.h"

#include <llvm/ExecutionEngine/Orc/ThreadSafeModule.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Type.h>
#include <llvm/IR/Function.h>

#include "mono_lsda_format.hpp"
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
  *
  * One of these per compile, owning the LLVMContext that compile builds in.
  * That ownership is what lets several threads translate at once: an
  * LLVMContext serializes nothing internally, so its type/constant/metadata
  * uniquing tables are only safe while exactly one thread is building in it.
  * Everything cached here is derived from that context (LLVM types, the
  * intrinsic declarations in lmodule) and so cannot outlive it or be shared
  * with a compile running in another one.
  */
struct MonoLLVMModule {
	/*
	 * A ThreadSafeContext rather than a bare LLVMContext because the engine
	 * co-owns it: ORC is handed a copy of this, which shares the underlying
	 * context and keeps it alive for as long as ORC might still touch the
	 * module built in it.
	 */
	llvm::orc::ThreadSafeContext context;
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
	int max_got_offset;
	std::vector<LLVMValueRef> intrins_by_id;

	char *global_prefix;
	/* Written by emit_gc_safepoint_poll ()'s AOT arm; kept with that function. */
	LLVMValueRef gc_poll_cold_wrapper;
	int max_method_idx;
	gboolean static_link;
	GHashTable *idx_to_lmethod;
	GHashTable *objc_selector_to_var;
	GHashTable *got_idx_to_type;

	/*
	 * The clause table being assembled for this compile, indexed by the clause
	 * ids the IR carries (EmitContext::clause_id) - NOT by any one method's IL
	 * clause index. It starts as a copy of the root's own clause table and grows
	 * as clause-bearing callees are materialized in, so it ends up describing
	 * every clause in the finished function whichever method contributed it.
	 *
	 * It has to be a copy, not a pointer into a MonoMethodHeader:
	 * materialize_callee () destroys the callee's MonoCompile - and with it that
	 * header - as soon as the body is translated, long before anything is
	 * published.
	 *
	 * Only `flags` and `data.catch_class` are read back out of the entries. The
	 * IL offsets a MonoExceptionClause also carries describe positions in one
	 * method's IL, which says nothing once clauses from several methods share a
	 * table; nesting is carried by the landing pads instead
	 * (add_covering_clauses).
	 */
	std::vector<MonoExceptionClause> clauses;

	/* The context as a plain reference, for the llvm:: APIs that want one. */
	llvm::LLVMContext &ctx () { return *context.getContext (); }
};

/*
 * Information associated by the backend with mono basic blocks.
 */
typedef struct {
	LLVMBasicBlockRef bblock, end_bblock;
	llvm::Value *finally_ind;
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
 * IRBuilder::CreateLoad/CreateGEP need it. Pairing the two here means the element type is
 * recorded where the pointer is created and can never be re-derived (and so
 * silently mis-derived) at the point of use.
 */
typedef struct {
	llvm::Value *value;
	/* The element type of the pointer */
	llvm::Type *type;
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
	llvm::IRBuilder<> *builder;
	llvm::Value **values;
	Address **addresses;
	MonoType **vreg_cli_types;
	LLVMCallInfo *linfo;
	MonoMethodSignature *sig;
	std::vector<std::unique_ptr<llvm::IRBuilder<>>> builders;
	std::unordered_map<int, MonoBasicBlock*> region_to_handler;
	std::unordered_map<int, MonoBasicBlock*> clause_to_handler;
	/*
	 * The type_info_N global that names one clause on a landing pad. Every pad
	 * that a clause covers names it through the SAME global, so which clause a
	 * landingpad operand stands for is decided by identity rather than by
	 * content.
	 */
	std::unordered_map<int, LLVMValueRef> clause_type_info;
	/*
	 * What this body's own clause 0 is called in the root's numbering. Zero for a
	 * root; for a materialized callee, the slot its clauses were appended at.
	 *
	 * Everything a clause's identity is baked into - the type_info globals, the
	 * finally marker stackmap IDs, the landing pad selector cases - has to be
	 * unique across the WHOLE function the inliner eventually builds, not just
	 * within the body being translated. Two bodies both numbering from 0 would
	 * collide the moment one is folded into the other. cfg->header->clauses[] and
	 * clause_to_handler stay local: they are about the body being translated.
	 */
	int clause_id_base;
	llvm::IRBuilder<> *alloca_builder;
	llvm::Value *last_alloca;
	llvm::Value *rgctx_arg;
	llvm::Value *this_arg;
	llvm::Type **vreg_types;
	gboolean *is_vphi;
	LLVMTypeRef method_type;
	gboolean *is_dead;
	gboolean *unreachable;
	gboolean has_safepoints;
	/*
	 * Set when this context is translating a callee body into an already-open
	 * caller module for the tier-1 inliner (materialize). In that mode
	 * emit_method_inner () skips the root annotation, the finalize/optimize/JIT
	 * step and the method<->lmethod bookkeeping - the inliner owns the resulting
	 * Function and either inlines it or strips it.
	 */
	bool translate_only;
	/*
	 * Set when the caller passes this body a vtable/mrgctx argument that the body
	 * itself has no use for - a materialized callee is compiled specialized, so
	 * every runtime lookup the shared body would have made through that argument
	 * is a constant here. The parameter is kept anyway so the body's type still
	 * matches the declaration the call site was emitted against; nothing reads it.
	 */
	bool keep_rgctx_arg;
	int this_arg_pindex, rgctx_arg_pindex;
	llvm::Value *imt_rgctx_loc;
	GHashTable *llvm_types;
	llvm::Value *ex_var;
	/*
	 * The function's landingpad personality (an i32-returning `mono_personality`
	 * stub). Created lazily by get_mono_personality() the first time a handler is
	 * emitted and pinned onto lmethod via LLVMSetPersonalityFn, so a method with
	 * several catch clauses defines it (and sets the personality fn) exactly once.
	 */
	LLVMValueRef personality;
	std::vector<llvm::Value*> phi_values;
	std::vector<MonoBasicBlock*> bblock_list;
	char *method_name;
	llvm::DenseMap<MonoMethod*, llvm::Value*> jit_callees;
	llvm::Value *long_bb_break_var;

	/*
	 * The context this compile owns, as a C++ reference -- what every C++ call
	 * site here reaches for when it needs one: Type::getInt32Ty (C),
	 * ConstantInt::get (...), and so on.
	 */
	llvm::LLVMContext &llvm_ctx () const { return module->ctx (); }

	/* Translator liveness: false once a decline has disabled llvm for this method. */
	bool ok () const { return !cfg->disable_llvm; }

	/* Decline this method: record the reason and hand it back to the classic JIT. */
	void set_failure (const char *message);

	/* Emit code to convert the LLVM value V to DTYPE (widening/truncating/bitcasting). */
	llvm::Value *convert_full (llvm::Value *v, llvm::Type *dtype, gboolean is_unsigned);
	llvm::Value *convert (llvm::Value *v, llvm::Type *dtype);

	/*
	 * MonoType -> LLVM type mapping (defined in translator-types.cpp).
	 *
	 * These are members rather than free functions purely because every LLVM
	 * type they hand back is uniqued in a particular LLVMContext, and the one
	 * they must use is this compile's. Nothing else about them needs the
	 * EmitContext.
	 */
	LLVMTypeRef simd_class_to_llvm_type (MonoClass *klass);
	LLVMTypeRef type_to_llvm_type (MonoType *t);
	bool type_is_unsigned (MonoType *t);
	LLVMTypeRef type_to_llvm_arg_type (MonoType *t);
	LLVMValueRef const_int32 (int v);
	LLVMValueRef const_int64 (int64_t v);
	LLVMTypeRef IntPtrType ();
	LLVMTypeRef ObjRefType ();
	LLVMTypeRef ThisType ();
	LLVMTypeRef type_to_sse_type (int type);
	LLVMTypeRef primitive_type_to_llvm_type (MonoTypeEnum type);
	LLVMTypeRef llvm_type_to_stack_type (MonoCompile *cfg, LLVMTypeRef type);
	LLVMTypeRef regtype_to_llvm_type (char c);
	LLVMTypeRef op_to_llvm_type (int opcode);
	LLVMTypeRef load_store_to_llvm_type (int opcode, int *size, gboolean *sext, gboolean *zext);
	LLVMTypeRef simd_op_to_llvm_type (int opcode);

	/* Constant-vector builders (defined in translator-call.cpp). */
	LLVMValueRef create_const_vector (LLVMTypeRef t, const int *vals, int count);
	LLVMValueRef create_const_vector_i32 (const int *mask, int count);
	LLVMValueRef create_const_vector_4_i32 (int v0, int v1, int v2, int v3);
	LLVMValueRef create_const_vector_2_i32 (int v0, int v1);
	LLVMValueRef get_double_const (MonoCompile *cfg, double val);
	LLVMValueRef get_float_const (MonoCompile *cfg, float val);

	/* Mono signature -> LLVM function type (defined in translator-emit.cpp). */
	LLVMTypeRef sig_to_llvm_sig_no_cinfo (MonoMethodSignature *sig);
	LLVMTypeRef sig_to_llvm_sig_full (MonoMethodSignature *sig, LLVMCallInfo *cinfo);
	LLVMTypeRef sig_to_llvm_sig (MonoMethodSignature *sig);

	/* Basic-block helpers (defined in translator-emit.cpp). */
	LLVMBasicBlockRef get_bb (MonoBasicBlock *bb);
	LLVMBasicBlockRef get_end_bb (MonoBasicBlock *bb);
	LLVMBasicBlockRef gen_bb (const char *prefix);

	/* Builder / alloca / address helpers (defined in translator-emit.cpp). */
	llvm::IRBuilder<> *create_builder ();
	LLVMValueRef build_alloca_llvm_type_name (LLVMTypeRef t, int align, const char *name);
	LLVMValueRef build_alloca_llvm_type (LLVMTypeRef t, int align);
	LLVMValueRef build_named_alloca (MonoType *t, char const *name);
	Address *create_address (LLVMValueRef value, LLVMTypeRef type);
	Address *build_alloca_address (MonoType *t);

	/* IR-emission helpers (defined in translator-emit.cpp). */
	void emit_memset (llvm::IRBuilder<> *builder, LLVMValueRef v, LLVMValueRef size, int alignment);
	LLVMValueRef emit_volatile_load (int vreg);
	void emit_volatile_store (int vreg);
	LLVMValueRef emit_call (MonoBasicBlock *bb, llvm::IRBuilder<> **builder_ref, LLVMTypeRef sig, LLVMValueRef callee, LLVMValueRef *args, int pindex);
	LLVMValueRef emit_load (MonoBasicBlock *bb, llvm::IRBuilder<> **builder_ref, int size, LLVMTypeRef type, LLVMValueRef addr, LLVMValueRef base, const char *name, gboolean is_faulting, gboolean is_volatile, BarrierKind barrier);
	void emit_store_general (MonoBasicBlock *bb, llvm::IRBuilder<> **builder_ref, int size, LLVMValueRef value, LLVMValueRef addr, LLVMValueRef base, gboolean is_faulting, gboolean is_volatile, BarrierKind barrier);
	void emit_store (MonoBasicBlock *bb, llvm::IRBuilder<> **builder_ref, int size, LLVMValueRef value, LLVMValueRef addr, LLVMValueRef base, gboolean is_faulting, gboolean is_volatile);
	void emit_cond_system_exception (MonoBasicBlock *bb, const char *exc_type, LLVMValueRef cmp, gboolean force_explicit);
	void emit_args_to_vtype (llvm::IRBuilder<> *builder, MonoType *t, LLVMValueRef address, LLVMArgInfo *ainfo, LLVMValueRef *args);
	void emit_vtype_to_args (llvm::IRBuilder<> *builder, MonoType *t, LLVMValueRef address, LLVMArgInfo *ainfo, LLVMValueRef *args, guint32 *nargs);
	LLVMValueRef emit_gsharedvt_ldaddr (int vreg);

	/* Call / prologue / exception-emission helpers (defined in translator-call.cpp). */
	void emit_div_check (llvm::IRBuilder<> *builder, MonoBasicBlock *bb, MonoInst *ins, llvm::Value *lhs, llvm::Value *rhs);
	void emit_this_slot_stackmap (llvm::IRBuilder<> *builder, LLVMValueRef slot);
	void emit_finally_guard_stackmap (llvm::IRBuilder<> *builder, LLVMValueRef slot, int clause_index);
	void emit_finally_end_stackmap (llvm::IRBuilder<> *builder, int clause_index);
	void emit_il_seq_point_stackmap (llvm::IRBuilder<> *builder, guint32 il_offset);
	void emit_entry_bb (llvm::IRBuilder<> *builder);
	void emit_class_init_guards (llvm::IRBuilder<> *builder);
	void emit_throw (MonoBasicBlock *bb, gboolean rethrow, LLVMValueRef exc);
	void emit_handler_start (MonoBasicBlock *bb, llvm::IRBuilder<> *builder);
	void emit_resume_unwind (MonoBasicBlock *bb, llvm::IRBuilder<> **builder_ref);
	LLVMValueRef clause_type_info_global (int clause_index);
	void add_covering_clauses (LLVMValueRef landing_pad, int clause_index);
	/* This body's local clause index, as the root's numbering knows it. */
	int clause_id (int clause_index) const { return this->clause_id_base + clause_index; }
	bool is_supported_callconv (MonoCallInst *call);
	void process_call (MonoBasicBlock *bb, llvm::IRBuilder<> **builder_ref, MonoInst *ins);
	LLVMValueRef call_intrins (int id, LLVMValueRef *args, const char *name);
	LLVMValueRef get_mono_personality ();

	/* Intrinsic declaration cache (defined in translator-intrinsics.cpp). */
	LLVMValueRef get_intrins (int id);

	/* AOT-const / jit-callee / named-alloca helpers (defined in translator-emit.cpp). */
	LLVMValueRef get_aotconst (MonoJumpInfoType type, gconstpointer data, LLVMTypeRef llvm_type);
	LLVMValueRef get_jit_callee (const char *name, LLVMTypeRef llvm_sig, MonoJumpInfoType type, gconstpointer data);
	LLVMValueRef get_direct_callee (const char *name, LLVMTypeRef llvm_sig, gpointer target);
	Address *build_named_alloca_address (MonoType *t, const char *name);

	/* Per-instruction translation (defined in translator-bb.cpp). */
	void process_bb (MonoBasicBlock *bb);

	/* Whole-method emission (defined in translator.cpp). */
	void emit_method_inner ();
	void llvm_jit_finalize_method ();
} EmitContext;

/*
 * The context an already-built LLVM object belongs to. Used by the handful of
 * helpers that decorate a value or extend a module without an EmitContext in
 * scope -- the object itself is the authority on which context it came from,
 * which beats passing one alongside it and hoping the two agree.
 */
static inline llvm::LLVMContext &
value_ctx (LLVMValueRef v)
{
	return llvm::unwrap (v)->getContext ();
}

static inline llvm::LLVMContext &
module_ctx (LLVMModuleRef m)
{
	return llvm::unwrap (m)->getContext ();
}

/*
 * The llvm-c entry points below have context-implicit spellings
 * (LLVMAppendBasicBlock, LLVMMDNode, ...) that resolve to the process-global
 * context. Nothing here builds in that context any more, so those spellings
 * would attach objects from one context to a value in another - which is not
 * an error LLVM reports, it is a crash later on. These wrappers take the
 * context from the object at hand, so there is no global spelling left to
 * reach for by accident.
 */
static inline LLVMBasicBlockRef
append_basic_block (LLVMValueRef fn, const char *name)
{
	return LLVMAppendBasicBlockInContext (llvm::wrap (&value_ctx (fn)), fn, name);
}

static inline unsigned
md_kind_id (llvm::LLVMContext &c, const char *name)
{
	return LLVMGetMDKindIDInContext (llvm::wrap (&c), name, strlen (name));
}

static inline LLVMValueRef
md_string (llvm::LLVMContext &c, const char *str, unsigned len)
{
	return LLVMMDStringInContext (llvm::wrap (&c), str, len);
}

static inline LLVMValueRef
md_node (llvm::LLVMContext &c, LLVMValueRef *vals, unsigned count)
{
	return LLVMMDNodeInContext (llvm::wrap (&c), vals, count);
}

static inline LLVMTypeRef
struct_type (llvm::LLVMContext &c, LLVMTypeRef *members, unsigned count, LLVMBool packed)
{
	return LLVMStructTypeInContext (llvm::wrap (&c), members, count, packed);
}

/*
 * Marshal a C array of LLVMValueRef GEP indices into the ArrayRef<Value *> that
 * IRBuilder::CreateGEP expects.
 */
static inline llvm::SmallVector<llvm::Value *, 4>
gep_index_list (LLVMValueRef *idx, unsigned n)
{
	llvm::SmallVector<llvm::Value *, 4> v;
	v.reserve (n);
	for (unsigned i = 0; i < n; ++i)
		v.push_back (llvm::unwrap (idx [i]));
	return v;
}

/*
 * clause_encloses:
 *
 *   Does IL clause J strictly ENCLOSE clause C - i.e. is C's try region nested in
 * J's? Every stage that has to reason about nesting shares this one predicate: the
 * translator's crossing-clause gate, the landing-pad selector routing
 * (emit_handler_start) and the resume pad's dispatch (emit_resume_unwind).
 * The `.mono_lsda` synthesis in mono_lsda.cpp answers the same question over its
 * own clause type and must stay in step with this.
 *
 * SIBLINGS - identical try_offset AND try_len, i.e. try { } catch(A) catch(B) -
 * are excluded: they share one landing pad and are routed by the same-range loops,
 * not by nesting.
 *
 * The test is on the try regions' own extents. Handler PLACEMENT says nothing about
 * nesting: nothing requires a clause's handler to sit after its try region, and IL
 * that puts an enclosing clause's handler at a lower offset than its try inverts any
 * predicate that reads handler_offset as a stand-in for where the try region ends.
 * Comparing ends also keeps a clause that lives inside another's HANDLER body out -
 * its try region is past the end of the other's, so it is not contained, which is
 * right: a throw in a handler is not protected by that handler's own clause.
 */
static inline bool
clause_encloses (const MonoExceptionClause *c, const MonoExceptionClause *j)
{
	bool siblings = c->try_offset == j->try_offset && c->try_len == j->try_len;
	return !siblings &&
	       c->try_offset >= j->try_offset &&
	       (guint64)c->try_offset + c->try_len <= (guint64)j->try_offset + j->try_len;
}

typedef struct {
	MonoBasicBlock *bb;
	MonoInst *phi;
	MonoBasicBlock *in_bb;
	int sreg;
} PhiNode;

static inline auto
get_long_imm (MonoInst *ins)
{
#if TARGET_SIZEOF_VOID_P == 4
	return ins->inst_l;
#else
	return ins->inst_imm;
#endif
}

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
inline constexpr bool IS_TARGET_X86 = true;
#else
inline constexpr bool IS_TARGET_X86 = false;
#endif

#ifdef TARGET_AMD64
inline constexpr bool IS_TARGET_AMD64 = true;
#else
inline constexpr bool IS_TARGET_AMD64 = false;
#endif

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

static inline const char *
llvm_ins_info (int opcode)
{
	return &mini_llvm_ins_info [(opcode - OP_START - 1) * 4];
}

extern const LLVMIntPredicate cond_to_llvm_cond [];
extern const LLVMRealPredicate fpcond_to_llvm_cond [];

/*
 * Bridge the llvm-c predicate enums to llvm::CmpInst::Predicate for
 * IRBuilder::CreateICmp/CreateFCmp. The predicates in the translator are still
 * stored/passed as the C enums (LLVMIntPredicate/LLVMRealPredicate) - retyping
 * that storage is a later slice - so we cast here at the call boundary. The
 * llvm-c API guarantees the C enumerators share the C++ enum's integer values
 * (LLVMBuildICmp/LLVMBuildFCmp themselves rely on a plain cast), which the
 * static_asserts pin.
 */
static inline llvm::CmpInst::Predicate
to_llvm_pred (LLVMIntPredicate p)
{
	static_assert ((int) LLVMIntEQ  == (int) llvm::CmpInst::ICMP_EQ, "int-pred ABI");
	static_assert ((int) LLVMIntSLE == (int) llvm::CmpInst::ICMP_SLE, "int-pred ABI");
	return static_cast<llvm::CmpInst::Predicate> (p);
}
static inline llvm::CmpInst::Predicate
to_llvm_pred (LLVMRealPredicate p)
{
	static_assert ((int) LLVMRealOEQ == (int) llvm::CmpInst::FCMP_OEQ, "real-pred ABI");
	static_assert ((int) LLVMRealPredicateTrue == (int) llvm::CmpInst::FCMP_TRUE, "real-pred ABI");
	return static_cast<llvm::CmpInst::Predicate> (p);
}

/* Defined in translator-types.cpp. */
/*
 * The 128-bit SIMD type for the mono type TYPE, in context C. The intrinsic
 * table reaches for this without an EmitContext in scope, so the context is
 * explicit; EmitContext::type_to_sse_type () is the same thing spelled for
 * call sites that have one.
 */
LLVMTypeRef
sse_type (llvm::LLVMContext &c, int type);
/* The integer type with width == TARGET_SIZEOF_VOID_P, in context C. */
LLVMTypeRef
int_ptr_type (llvm::LLVMContext &c);
guint32
get_vtype_size (MonoType *t);
MonoTypeEnum
inst_c1_type (const MonoInst *ins);
bool
primitive_type_is_unsigned (MonoTypeEnum t);
IntrinsicId
ovf_op_to_intrins (int opcode);
IntrinsicId
simd_ins_to_intrins (int opcode);
void
set_cold_cconv (LLVMValueRef func);
void
set_call_cold_cconv (LLVMValueRef func);

/* Defined in translator-emit.cpp. */
G_GNUC_UNUSED LLVMTypeRef
LLVMFunctionType0 (LLVMTypeRef ReturnType,
				   int IsVarArg);
void
set_metadata_flag (LLVMValueRef v, const char *flag_name);
void
set_nontemporal_flag (LLVMValueRef v);
void
set_invariant_load_flag (LLVMValueRef v);
LLVMValueRef
emit_icall_cold_wrapper (MonoLLVMModule *module, LLVMModuleRef lmodule, MonoJitICallId icall_id, gboolean aot);
void
emit_gc_safepoint_poll (MonoLLVMModule *module, LLVMModuleRef lmodule, MonoCompile *cfg);

/*
 * llvm.experimental.stackmap patchpoint IDs. Every stackmap this translator
 * plants carries one of these, and each recovery pass in translator.cpp picks
 * out its own records by ID - a method can carry both kinds at once (a gshared
 * method with a finally), so neither pass may assume a record's position.
 *
 * The gshared this/mrgctx home slot uses ID 0; there is at most one per method.
 *
 * The thread-abort guard records (one at each finally body ENTRY and one at each
 * body TAIL) encode their clause_index plus a start/end bit in the low 32 bits,
 * under a marker in the high 32. LLVM may duplicate a finally body, so a clause
 * legitimately yields several entry/tail pairs; recovery pairs them up by PC.
 * clause_index is a 15-bit IL header field, so it fits well below the start bit.
 */
using mono::MONO_LLVM_THIS_SLOT_STACKMAP_ID;
using mono::MONO_LLVM_FINALLY_STACKMAP_ID_BASE;
using mono::MONO_LLVM_FINALLY_END_STACKMAP_ID_BASE;
using mono::MONO_LLVM_FINALLY_STACKMAP_ID_MASK;
using mono::MONO_LLVM_IL_SEQ_POINT_STACKMAP_ID_BASE;
using mono::MONO_LLVM_IL_SEQ_POINT_STACKMAP_ID_MASK;

/* Defined in translator-call.cpp. */
/*
 * A stable, linker-safe symbol name for METHOD, suitable for naming a direct
 * `call @name` edge to it. The same method always gets the same name back
 * (cached for the life of the process) and two distinct methods never
 * collide; see the definition for how that is arranged.
 */
const char *
mono_llvm_method_symbol (MonoMethod *method);

/* Inverse of mono_llvm_method_symbol (); NULL if NAME is not a method symbol. */
MonoMethod *
mono_llvm_method_from_symbol (const char *name);

namespace mono {

/*
 * The vtable whose cctor the body being compiled for CFG has to trigger itself,
 * NULL if there is none. *INDETERMINATE is set when the vtable cannot be named;
 * see the definition in translator.cpp.
 */
MonoVTable *
pending_class_init_vtable (MonoCompile *cfg, bool *indeterminate);

} // namespace mono

/* Defined in translator-intrinsics.cpp. */
void
add_types (MonoLLVMModule *module);

/*
 * The compiled GC-poll cold wrapper, JIT-compiling it on first use.
 *
 * Process-wide, not per-compile: it is a native code address, and every
 * compile that plants a safepoint poll calls the same one. Racing callers are
 * harmless - mono_jit_compile_method () hands both the same address.
 */
gpointer
gc_poll_cold_wrapper_code (void);

#endif /* DISABLE_JIT */

#endif /* __MONO_MINI_LLVM_TRANSLATOR_INTERNAL_HPP__ */

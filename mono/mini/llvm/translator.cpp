/**
 * \file
 * llvm "Backend" for the mono JIT
 *
 * Copyright 2009-2011 Novell Inc (http://www.novell.com)
 * Copyright 2011 Xamarin Inc (http://www.xamarin.com)
 * Licensed under the MIT license. See LICENSE file in the project root for full license information.
 */

#include "translator-internal.hpp"

#include <algorithm>
#include <map>
#include <mutex>
#include <utility>
#include <vector>

#include <llvm/IR/Module.h>
#include <llvm/Object/StackMapParser.h>

#include <mono/metadata/metadata-internals.h>
#include <mono/metadata/mono-endian.h>
/* mono-basic-block.h has no extern "C" guard of its own. */
extern "C" {
#include <mono/metadata/mono-basic-block.h>
}

#include "engine.hpp"
#include "mono_lsda.hpp"
#include "passes/inliner-support.hpp"
#include "passes/devirt-support.hpp"

#ifndef DISABLE_JIT
/*
 * Instruction metadata
 * This is the same as ins_info, but LREG != IREG.
 */
#ifdef MINI_OP
#undef MINI_OP
#endif
#ifdef MINI_OP3
#undef MINI_OP3
#endif
#define MINI_OP(a,b,dest,src1,src2) dest, src1, src2, ' ',
#define MINI_OP3(a,b,dest,src1,src2,src3) dest, src1, src2, src3,
#define NONE ' '
#define IREG 'i'
#define FREG 'f'
#define VREG 'v'
#define XREG 'x'
#define LREG 'l'
/* keep in sync with the enum in mini.h */
const char
mini_llvm_ins_info[] = {
#include "mini-ops.h"
};
#undef MINI_OP
#undef MINI_OP3


const LLVMIntPredicate cond_to_llvm_cond [] = {
	LLVMIntEQ,
	LLVMIntNE,
	LLVMIntSLE,
	LLVMIntSGE,
	LLVMIntSLT,
	LLVMIntSGT,
	LLVMIntULE,
	LLVMIntUGE,
	LLVMIntULT,
	LLVMIntUGT,
};

const LLVMRealPredicate fpcond_to_llvm_cond [] = {
	LLVMRealOEQ,
	LLVMRealUNE,
	LLVMRealOLE,
	LLVMRealOGE,
	LLVMRealOLT,
	LLVMRealOGT,
	LLVMRealULE,
	LLVMRealUGE,
	LLVMRealULT,
	LLVMRealUGT,
	LLVMRealORD,
	LLVMRealUNO
};

void set_invariant_load_flag (LLVMValueRef v);

/*
 * MONO_LLVM_METHOD: development/diagnostic filter restricting the LLVM path.
 *
 * When set, ONLY methods matching one of the ';'-separated descriptors are
 * allowed onto the LLVM path; every other method falls back to the classic
 * JIT. This is the inverse of the other gates in
 * mono_llvm_check_method_supported(): those name what LLVM cannot do, this
 * names the only thing it may attempt.
 *
 * It exists because running LLVM as the primary JIT compiles a method's
 * callees on the same stack (nesting depth reached 70 on a hello-world),
 * which exhausts the stack before anything executes. Restricting LLVM to a
 * handful of methods keeps the nesting shallow, so an LLVM-compiled method
 * can actually be run and its result checked. Tiered compilation removes the
 * underlying recursion - tier 1 compiles methods whose callees are already
 * tier-0 compiled - at which point this remains useful for bisecting a
 * miscompile down to a single method.
 *
 * Descriptor syntax matches MONO_VERBOSE_METHOD: either a full
 * 'Namespace.Class:Method' description, or a bare method name.
 *
 *   MONO_LLVM_METHOD='LlvmExec:Compute' mono --llvm test.exe
 */
static char **llvm_method_filter_names;
static std::once_flag llvm_method_filter_once;

static bool
llvm_method_filter_excludes (MonoMethod *method)
{
	int i;

	/* Read once for the process; several compile threads can arrive here. */
	std::call_once (llvm_method_filter_once, [] () {
		char *env = g_getenv ("MONO_LLVM_METHOD");
		if (env != nullptr)
			llvm_method_filter_names = g_strsplit (env, ";", -1);
	});

	if (!llvm_method_filter_names)
		return false;

	for (i = 0; llvm_method_filter_names [i] != nullptr; i++) {
		const char *name = llvm_method_filter_names [i];

		if ((strchr (name, '.') > name) || strchr (name, ':')) {
			MonoMethodDesc *desc = mono_method_desc_new (name, TRUE);
			if (desc) {
				bool match = mono_method_desc_full_match (desc, method);
				mono_method_desc_free (desc);
				if (match)
					return false;
			}
		} else {
			if (strcmp (method->name, name) == 0)
				return false;
		}
	}

	return true;
}

/*
 * mono_llvm_check_method_supported:
 *
 *   Do some quick checks to decide whenever cfg->method can be compiled by LLVM, to avoid
 * compiling a method twice.
 */
void
mono_llvm_check_method_supported (MonoCompile *cfg)
{
	int i, j;

	/**
	 * If optimization is disabled for this function then we don't want to run
	 * it through the tier1 optimizer.
	 */
	if (cfg->method->iflags & METHOD_IMPL_ATTRIBUTE_NOOPTIMIZATION)
	{
		TRACE_FAILURE_CFG(cfg, "NoOptimization");
		cfg->exception_message = g_strdup("optimization is disabled for this function");
		cfg->disable_llvm = TRUE;
		return;
	}

	/*
	 * Tier1 compilation does not support safepoints at the moment.
	 */
	if (mono_threads_are_safepoints_enabled ()) {
		TRACE_FAILURE_CFG (cfg, "the LLVM backend does not support GC safepoints");
		cfg->exception_message = g_strdup ("the LLVM backend does not support GC safepoints");
		cfg->disable_llvm = TRUE;
		return;
	}

	/*
	 * Non-root-domain gate. The engine keys a compiled method's code lifetime on
	 * cfg->domain and reclaims it wholesale when that domain unloads
	 * (mono_llvm_jit_release_domain -> release_owner). That is correct for code
	 * that dies with the domain, but domain-neutral methods - notably the shared
	 * remoting/proxy invoke wrappers - are cached globally and outlive the domain
	 * they happen to be first compiled in. Compiled into an unloadable child
	 * domain's slab, such a method's code is freed on AppDomain.Unload while a
	 * cached pointer to it stays live, so the next cross-domain call jumps into
	 * reclaimed (zeroed) slab memory and crashes. The classic JIT places these
	 * methods correctly, so decline anything compiled for a non-root domain to
	 * tier-0. Single-AppDomain programs (all corpora, the common case) run in the
	 * root domain and never hit this; multi-AppDomain LLVM support is deferred.
	 */
	if (cfg->domain && cfg->domain != mono_get_root_domain ()) {
		TRACE_FAILURE_CFG (cfg, "non-root AppDomain: code lifetime is domain-keyed (unload reclaim hazard)");
		cfg->exception_message = g_strdup ("non-root AppDomain (unload reclaim hazard)");
		cfg->disable_llvm = TRUE;
		return;
	}

	/* Diagnostic filter (MONO_LLVM_METHOD); a no-op unless the variable is set. */
	if (llvm_method_filter_excludes (cfg->method)) {
		TRACE_FAILURE_CFG (cfg, "not selected by MONO_LLVM_METHOD");
		cfg->exception_message = g_strdup ("not selected by MONO_LLVM_METHOD");
		cfg->disable_llvm = TRUE;
		return;
	}

	/*
	 * 3b EH gate: the custom-emit `.mono_lsda` path consumes a method's catch AND
	 * finally/fault geometry (doc 16, EH F2), so catch-only, standalone try/finally
	 * and standalone try/fault methods now go through LLVM. This is an ALLOWLIST:
	 * admit only NONE / FINALLY / FAULT and decline every other flags value to the
	 * classic JIT. FILTER (1) still defers - its resume/indicator machinery is not
	 * built yet - and a malformed flags value (the metadata loader reads the ECMA
	 * flags field raw, so bit0-set junk like 3/5/7 is possible) is declined here
	 * rather than reaching emit_handler_start and yielding an invalid zero-clause
	 * landing pad. The nested-clause decline below keeps this to the STANDALONE
	 * (non-nested) try/finally + try/fault shape; the save_lmf/dynamic declines
	 * still apply on top of this.
	 */
	for (i = 0; i < cfg->header->num_clauses; ++i) {
		guint32 clause_flags = cfg->header->clauses [i].flags;
		if (clause_flags != MONO_EXCEPTION_CLAUSE_NONE &&
		    clause_flags != MONO_EXCEPTION_CLAUSE_FINALLY &&
		    clause_flags != MONO_EXCEPTION_CLAUSE_FAULT) {
			TRACE_FAILURE_CFG (cfg, "filter EH clause (deferred to classic JIT, 3b)");
			cfg->exception_message = g_strdup ("filter EH clause (deferred to classic JIT, 3b)");
			cfg->disable_llvm = TRUE;
			return;
		}
	}

	/*
	 * Generic-shared methods ARE supported (design 3.1 / S6, #15). mini.c's
	 * generic-jit-info setup requires cfg->llvm_this_reg to be set for every
	 * gshared LLVM method (mini.c:2574, g_assert (cfg->llvm_this_reg != -1)) so a
	 * stack walk can reconstruct the frame's generic context. Stock LLVM has no
	 * built-in this-slot concept, so we supply it with
	 * llvm.experimental.stackmap: the translator
	 * plants a stackmap over the home slot of this (reference-type instance
	 * methods) or the mrgctx arg (static/valuetype/generic-method methods), and
	 * llvm_jit_finalize_method parses the `.llvm_stackmaps` section back into
	 * cfg->llvm_this_reg/offset. If that recovery fails for any reason, the method
	 * declines to the classic JIT there (CAP-EH-0), so no gate is needed here.
	 *
	 * gsharedvt is still excluded (mini.c sets disable_llvm for it earlier); this
	 * covers plain gshared only.
	 */

	if (cfg->method->save_lmf) {
		TRACE_FAILURE_CFG (cfg, "lmf");
		cfg->exception_message = g_strdup ("lmf");
		cfg->disable_llvm = TRUE;
	}
	if (cfg->disable_llvm)
		return;

	/*
	 * Crossing-clause gate. Nesting itself is fine at any depth: the runtime
	 * delivers a fault to the INNERMOST landing pad and the `.mono_lsda` synthesis
	 * (build_ex_info) hands it one entry per enclosing clause over the same range,
	 * in ascending clause_index (= innermost-encloser first), so it walks a nest
	 * outwards in the same order the classic JIT's inner-first clause array gives
	 * it. Where each of those entries sends control is the synthesis's job -
	 * either the pad already entered, whose selector switch re-dispatches RDX ==
	 * the enclosing clause_index, or, once a cleanup has run, that cleanup's
	 * resume pad. Nothing in either stage caps the depth.
	 *
	 * What CANNOT be encoded is a pair that overlaps but is neither siblings nor
	 * cleanly contained - a CROSSING / partial overlap, which only malformed IL
	 * produces. Synthesising an enclosing entry for it would publish a native
	 * range that strictly nests inside another's, and the runtime's first-match
	 * walk over the array would then be ambiguous, so decline instead (CAP-EH-0).
	 *
	 * True SIBLING catches - try { } catch(A) catch(B) - share the IDENTICAL
	 * protected region (same try_offset AND try_len) and are NOT nesting: they are
	 * emitted as one landing pad carrying a clause per sibling, routed by the
	 * selector. clause_encloses is false for siblings, so they pass the crossing
	 * test. That predicate is the one every stage shares, so the gate admits
	 * exactly the pairs the synthesis and the pad emission can represent.
	 */
	for (i = 0; i < cfg->header->num_clauses; ++i) {
		MonoExceptionClause *clause1 = &cfg->header->clauses [i];

		for (j = 0; j < cfg->header->num_clauses; ++j) {
			if (i == j)
				continue;
			MonoExceptionClause *clause2 = &cfg->header->clauses [j];

			bool siblings = clause1->try_offset == clause2->try_offset &&
			                    clause1->try_len == clause2->try_len;
			bool contained = clause_encloses (clause1, clause2) ||
			                     clause_encloses (clause2, clause1);
			bool overlap = clause1->try_offset < clause2->try_offset + clause2->try_len &&
			                   clause2->try_offset < clause1->try_offset + clause1->try_len;

			if (overlap && !siblings && !contained) {
				TRACE_FAILURE_CFG (cfg, "crossing EH clauses");
				cfg->exception_message = g_strdup ("crossing EH clauses");
				cfg->disable_llvm = TRUE;
				break;
			}
		}
		if (cfg->disable_llvm)
			break;
	}
	if (cfg->disable_llvm)
		return;

	/* FIXME: */
	if (cfg->method->dynamic) {
		TRACE_FAILURE_CFG (cfg, "dynamic.");
		cfg->exception_message = g_strdup ("dynamic.");
		cfg->disable_llvm = TRUE;
	}
	if (cfg->disable_llvm)
		return;
}

static LLVMCallInfo*
get_llvm_call_info (MonoCompile *cfg, MonoMethodSignature *sig)
{
	LLVMCallInfo *linfo;
	int i;

	linfo = mono_arch_get_llvm_call_info (cfg, sig);
	linfo->dummy_arg_pindex = -1;
	for (i = 0; i < sig->param_count; ++i)
		linfo->args [i + sig->hasthis].type = sig->params [i];

	return linfo;
}

static void
free_ctx (EmitContext *ctx)
{
	g_free (ctx->values);
	g_free (ctx->addresses);
	g_free (ctx->vreg_types);
	g_free (ctx->is_vphi);
	g_free (ctx->vreg_cli_types);
	g_free (ctx->is_dead);
	g_free (ctx->unreachable);
	g_free (ctx->bblocks);

	g_free (ctx->method_name);

	delete ctx;
}

/*
 * mono_llvm_emit_method:
 *
 *   Emit LLVM IL from the mono IL, and compile it to native code using LLVM.
 */
/*
 * Allocate an EmitContext and its per-vreg/per-bblock scratch arrays for CFG,
 * translating into MODULE. The caller still has to set ctx->lmodule (the LLVM
 * module the function is emitted into) before calling emit_method_inner ().
 */
static EmitContext *
alloc_emit_context (MonoCompile *cfg, MonoLLVMModule *module)
{
	EmitContext *ctx = new EmitContext ();
	ctx->cfg = cfg;
	ctx->mempool = cfg->mempool;
	/* A root numbers its clauses from 0; materialize_callee () rebases a callee. */
	ctx->clause_id_base = 0;

	/* This maps vregs to the LLVM instruction defining them */
	ctx->values = g_new0 (llvm::Value *, cfg->next_vreg);
	/*
	 * This maps vregs for volatile variables to the LLVM instruction defining
	 * their address.
	 */
	ctx->addresses = g_new0 (Address*, cfg->next_vreg);
	ctx->vreg_types = g_new0 (llvm::Type *, cfg->next_vreg);
	ctx->is_vphi = g_new0 (gboolean, cfg->next_vreg);
	ctx->vreg_cli_types = g_new0 (MonoType*, cfg->next_vreg);
	ctx->phi_values.reserve (256);
	/*
	 * This signals whenever the vreg was defined by a phi node with no input vars
	 * (i.e. all its input bblocks end with NOT_REACHABLE).
	 */
	ctx->is_dead = g_new0 (gboolean, cfg->next_vreg);
	/* Whenever the bblock is unreachable */
	ctx->unreachable = g_new0 (gboolean, cfg->max_block_num);
	ctx->bblock_list.reserve (256);

	ctx->module = module;
	ctx->method_name = mono_method_full_name (cfg->method, TRUE);

	return ctx;
}

/*
 * A declined/failed emit can leave a half-built function in the module. Wire up
 * any dangling phi nodes (they may be referenced by other values) and delete the
 * function so nothing malformed survives.
 */
static void
cleanup_failed_emit (EmitContext *ctx)
{
	if (!ctx->lmethod)
		return;

	/* Need to add unused phi nodes as they can be referenced by other values */
	LLVMBasicBlockRef phi_bb = append_basic_block (ctx->lmethod, "PHI_BB");
	llvm::IRBuilder<> *builder;

	builder = ctx->create_builder ();
	builder->SetInsertPoint (llvm::unwrap (phi_bb));

	for (llvm::Value *v : ctx->phi_values) {
		if (LLVMGetInstructionParent (llvm::wrap (v)) == nullptr)
			LLVMInsertIntoBuilder (llvm::wrap (builder), llvm::wrap (v));
	}

	LLVMDeleteFunction (ctx->lmethod);
	ctx->lmethod = nullptr;
}

/*
 * Build the per-compile MonoLLVMModule: a fresh LLVMContext, the LLVM module
 * the method is emitted into, and the caches derived from both.
 *
 * One of these per compile is what makes concurrent tier-1 compilation safe.
 * An LLVMContext does no locking of its own, so its type, constant and
 * metadata uniquing tables tolerate exactly one thread building in them; give
 * every compile its own and threads never meet. Nothing here is shared, so
 * nothing here needs a lock.
 */
static MonoLLVMModule *
create_compile_module (MonoCompile *cfg)
{
	MonoLLVMModule *module = new MonoLLVMModule ();

	auto ctx = std::make_unique<llvm::LLVMContext> ();
	module->context_ptr = ctx.get ();
	module->context = llvm::orc::ThreadSafeContext (std::move (ctx));
	module->intrins_by_id.resize (INTRINS_NUM, nullptr);
	module->llvm_types = g_hash_table_new (NULL, NULL);

	char *name = g_strdup_printf ("jit-module-%s", cfg->method->name);
	module->lmodule = LLVMModuleCreateWithNameInContext (name, llvm::wrap (&module->ctx ()));
	g_free (name);

	add_types (module);

	return module;
}

static void
free_compile_module (MonoLLVMModule *module)
{
	g_hash_table_destroy (module->llvm_types);

	if (module->objc_selector_to_var)
		g_hash_table_destroy (module->objc_selector_to_var);

	if (module->bb_names) {
		for (int i = 0; i < module->bb_names_len; ++i)
			g_free (module->bb_names [i]);
		g_free (module->bb_names);
	}

	/*
	 * Dispose the module but not the context: the engine holds a copy of the
	 * ThreadSafeContext, so dropping ours is enough. If ORC is still holding
	 * the module it copied, its copy keeps the context alive; if it isn't,
	 * ours was the last reference and the context goes with it.
	 */
	LLVMDisposeModule (module->lmodule);

	delete module;
}

void
mono_llvm_emit_method (MonoCompile *cfg)
{
	EmitContext *ctx;

	if (cfg->skip)
		return;

	MonoLLVMModule *module = create_compile_module (cfg);

	ctx = alloc_emit_context (cfg, module);
	ctx->lmodule = module->lmodule;

	ctx->emit_method_inner ();

	if (!ctx->ok ())
		cleanup_failed_emit (ctx);

	free_ctx (ctx);
	free_compile_module (module);
}

/*
 * Give this function a subprogram, creating the compile's debug info if this is
 * the first function to need it. Both roots and materialized inliner callees get
 * one: a callee that carries its own subprogram is what makes LLVM record a real
 * DW_TAG_inlined_subroutine when it folds the body in, instead of quietly
 * restamping the body with the caller's location.
 */
void
EmitContext::begin_il_debug_info ()
{
	if (!this->module->il_debug)
		this->module->il_debug = std::make_unique<mono::IlDebugModule> (
			llvm::unwrap (this->lmodule));

	this->il_debug_scope = this->module->il_debug->add_function (
		llvm::unwrap<llvm::Function> (this->lmethod), this->method_name);

	/*
	 * So the engine can turn a subprogram name from the emitted DWARF back into
	 * the method it describes. A materialized callee has no other route back:
	 * once it is inlined there is no call site left naming it.
	 */
	this->module->il_debug_methods[this->method_name] = this->cfg->method;
}

/*
 * Attribute everything emitted from here on to IL_OFFSET, until the next
 * OP_IL_SEQ_POINT moves it. This is the whole mapping: the line in effect at a
 * native address is what the debug info records, and the assembler writes those
 * rows in code order, so a run of seq points that optimization collapsed onto
 * one address resolves to the last one - the same "most recent point execution
 * passed" the classic JIT's own lookup has.
 */
void
EmitContext::set_il_debug_location (llvm::IRBuilder<> *builder, guint32 il_offset)
{
	mono::il_debug_set_location (this->il_debug_scope, builder, il_offset);
}

void
EmitContext::finish_il_debug_info ()
{
	/* The module's, not this function's: a callee shares the root's compile unit. */
	if (this->module->il_debug)
		this->module->il_debug->finish ();
}

void
EmitContext::emit_method_inner ()
{
	MonoCompile *cfg = this->cfg;
	MonoMethodSignature *sig;
	MonoBasicBlock *bb;
	LLVMTypeRef method_type;
	LLVMValueRef method = nullptr;
	llvm::Value **values = this->values;
	int i, max_block_num;
	/* Indexes cfg->bblocks (guint num_bblocks) */
	guint bb_index;
	LLVMCallInfo *linfo;
	LLVMModuleRef lmodule = this->lmodule;
	BBInfo *bblocks;
	std::vector<MonoBasicBlock*> &bblock_list = this->bblock_list;
	MonoMethodHeader *header;
	MonoExceptionClause *clause;
	char **names;
	llvm::IRBuilder<> *entry_builder = nullptr;
	LLVMBasicBlockRef entry_bb = nullptr;

	if (cfg->gsharedvt) {
		this->set_failure ("gsharedvt");
		return;
	}

	if (cfg->method->wrapper_type == MONO_WRAPPER_OTHER) {
		WrapperInfo *info = mono_marshal_get_wrapper_info (cfg->method);
		if (info->subtype == WRAPPER_SUBTYPE_LLVM_FUNC) {
			g_assert (info->d.llvm_func.subtype == LLVM_FUNC_WRAPPER_GC_POLL);

			method = emit_icall_cold_wrapper (this->module, lmodule, MONO_JIT_ICALL_mono_threads_state_poll, FALSE);
			this->lmethod = method;
			this->module->max_method_idx = MAX (this->module->max_method_idx, static_cast<int>(cfg->method_index));

			this->method_name = g_strdup (LLVMGetValueName (method));
			this->cfg->asm_symbol = g_strdup (this->method_name);

			goto after_codegen;
		}
	}

	sig = mono_method_signature_internal (cfg->method);
	this->sig = sig;

	linfo = get_llvm_call_info (cfg, sig);
	this->linfo = linfo;
	if (!this->ok ())
		return;

	if (cfg->rgctx_var || this->keep_rgctx_arg)
		linfo->rgctx_arg = TRUE;

	this->method_type = method_type = this->sig_to_llvm_sig_full (sig, linfo);
	if (!this->ok ())
		return;

	method = LLVMAddFunction (lmodule, this->method_name, method_type);
	/* 64 bytes matches the x86-64 cache line; JITLink honors this as the section alignment when placing the compiled code. */
	LLVMSetAlignment (method, 64);
	this->lmethod = method;

	/*
	 * Mark the top-down inliner's root, for the benefit of anyone reading an IR
	 * dump: every method the translator emits standalone is a tier-1 root (v1
	 * emits exactly one per module), while a callee materialized into a
	 * caller's module (translate_only) is a candidate body, never a root. The
	 * pass itself does not read this - it is handed its root directly (see
	 * Tier1Root, passes/inliner-support.hpp).
	 */
	if (!this->translate_only)
		llvm::unwrap<llvm::Function> (method)->addFnAttr ("mono-tier1-root");

	this->begin_il_debug_info ();

	/*
	 * Mark that this method's IL declared at least one exception clause, so
	 * MonoEHGatherPass (engine.cpp) can tell "this try/finally's protected
	 * calls all got optimized to nounwind, nothing to publish" apart from
	 * "this method never had a try block at all" - both look identical to the
	 * gather pass otherwise (zero landing pads). Only cfg->header (this
	 * method's own IL) is checked, never a callee's - an inlined callee's own
	 * clauses are irrelevant here (translate_only bodies never carry this
	 * attribute, matching mono-tier1-root above).
	 */
	if (!this->translate_only && cfg->header->num_clauses > 0)
		llvm::unwrap<llvm::Function> (method)->addFnAttr ("mono-has-eh-clauses");

	/*
	 * No calling-convention override: the rgctx/imt argument is tagged
	 * LLVM_ATTR_NEST instead, which stock LLVM pins to R10 on SysV. See the
	 * note in process_call ().
	 */

	/* if the method doesn't contain
	 *  (1) a call (so it's a leaf method)
	 *  (2) and no loops
	 * we can skip the GC safepoint on method entry. */
	bool requires_safepoint;
	requires_safepoint = cfg->has_calls;
	if (!requires_safepoint) {
		for (bb = cfg->bb_entry->next_bb; bb; bb = bb->next_bb) {
			if (bb->loop_body_start || (bb->flags & BB_EXCEPTION_HANDLER)) {
				requires_safepoint = true;
			}
		}
	}
	if (cfg->method->wrapper_type) {
		/*
		 * A WrapperInfo is optional: plenty of wrapper kinds are built through
		 * paths that pass no info at all (mono_mb_create_and_cache () hands NULL
		 * to every cominterop wrapper, and a native-to-managed wrapper built for
		 * a specific delegate target gets none either), so this is NULL far more
		 * often than not. Only the gsharedvt subtypes below matter here, and
		 * those always carry an info, so no info simply means "not one of them" -
		 * keep the safepoint.
		 */
		WrapperInfo *info = mono_marshal_get_wrapper_info (cfg->method);

		switch (info ? info->subtype : WRAPPER_SUBTYPE_NONE) {
		case WRAPPER_SUBTYPE_GSHAREDVT_IN:
		case WRAPPER_SUBTYPE_GSHAREDVT_OUT:
		case WRAPPER_SUBTYPE_GSHAREDVT_IN_SIG:
		case WRAPPER_SUBTYPE_GSHAREDVT_OUT_SIG:
			/* Arguments are not used after the call */
			requires_safepoint = false;
			break;
		default:
			break;
		}
	}
	this->has_safepoints = requires_safepoint;

	if (mono_threads_are_safepoints_enabled () && requires_safepoint) {
		if (cfg->method->wrapper_type != MONO_WRAPPER_ALLOC) {
			LLVMSetGC (method, "coreclr");
			emit_gc_safepoint_poll (this->module, this->lmodule, cfg);
		}
	}
	LLVMSetLinkage (method, LLVMPrivateLinkage);

	mono_llvm_add_func_attr (method, LLVM_ATTR_UW_TABLE);

	if (cfg->disable_omit_fp)
		mono_llvm_add_func_attr_nv (method, "no-frame-pointer-elim", "true");

	LLVMSetLinkage (method, LLVMExternalLinkage);

	if (cfg->method->save_lmf) {
		this->set_failure ("lmf");
		return;
	}

	if (sig->pinvoke && cfg->method->wrapper_type != MONO_WRAPPER_RUNTIME_INVOKE) {
		this->set_failure ("pinvoke signature");
		return;
	}

	header = cfg->header;
	for (i = 0; i < header->num_clauses; ++i) {
		clause = &header->clauses [i];
		/*
		 * Emission-time mirror of Gate A (doc 16, EH F2): custom-emit EH now covers
		 * catch (NONE), standalone FINALLY and standalone FAULT. This is an
		 * ALLOWLIST: anything not in {NONE, FINALLY, FAULT} that slipped Gate A -
		 * a FILTER, or a malformed bit0-set flags value - declines here rather than
		 * being mis-emitted as a zero-clause catch landing pad (invalid IR).
		 */
		if (clause->flags != MONO_EXCEPTION_CLAUSE_NONE &&
		    clause->flags != MONO_EXCEPTION_CLAUSE_FINALLY &&
		    clause->flags != MONO_EXCEPTION_CLAUSE_FAULT) {
			this->set_failure ("filter clause (custom-emit EH does not support filters).");
			return;
		}
		/*
		 * The stackmap and the EH pad each force a frame pointer anyway, so
		 * pinning it is free. It says nothing about where the exvar lands -
		 * LLVM still homes locals off SP once it realigns the frame.
		 */
		if (clause->flags == MONO_EXCEPTION_CLAUSE_FINALLY)
			mono_llvm_add_func_attr_nv (method, "frame-pointer", "all");
	}
	if ((cfg->method->iflags & METHOD_IMPL_ATTRIBUTE_NOINLINING) || cfg->no_inline)
		mono_llvm_add_func_attr (method, LLVM_ATTR_NO_INLINE);

	if (linfo->rgctx_arg) {
		this->rgctx_arg = llvm::unwrap (LLVMGetParam (method, linfo->rgctx_arg_pindex));
		this->rgctx_arg_pindex = linfo->rgctx_arg_pindex;
		/*
		 * We mark the rgctx parameter with the inreg attribute, which is mapped to
		 * MONO_ARCH_RGCTX_REG in the Mono calling convention in llvm, i.e.
		 * CC_X86_64_Mono in X86CallingConv.td.
		 */
		mono_llvm_add_param_attr (llvm::wrap (this->rgctx_arg), LLVM_ATTR_NEST);
		LLVMSetValueName (llvm::wrap (this->rgctx_arg), "rgctx");
	} else {
		this->rgctx_arg_pindex = -1;
	}
	if (cfg->vret_addr) {
		values [cfg->vret_addr->dreg] = llvm::unwrap (LLVMGetParam (method, linfo->vret_arg_pindex));
		LLVMSetValueName (llvm::wrap (values [cfg->vret_addr->dreg]), "vret");
		if (linfo->ret.storage == LLVMArgVtypeByRef) {
			mono_llvm_add_param_attr_with_type (LLVMGetParam (method, linfo->vret_arg_pindex), LLVM_ATTR_STRUCT_RET, this->type_to_llvm_type (sig->ret));
			mono_llvm_add_param_attr (LLVMGetParam (method, linfo->vret_arg_pindex), LLVM_ATTR_NO_ALIAS);
		}
	}

	if (sig->hasthis) {
		this->this_arg_pindex = linfo->this_arg_pindex;
		this->this_arg = llvm::unwrap (LLVMGetParam (method, linfo->this_arg_pindex));
		values [cfg->args [0]->dreg] = this->this_arg;
		LLVMSetValueName (llvm::wrap (values [cfg->args [0]->dreg]), "this");
	}
	if (linfo->dummy_arg)
		LLVMSetValueName (LLVMGetParam (method, linfo->dummy_arg_pindex), "dummy_arg");

	names = g_new (char *, sig->param_count);
	mono_method_get_param_names (cfg->method, const_cast<const char **>(names));

	/* Set parameter names/attributes */
	for (i = 0; i < sig->param_count; ++i) {
		LLVMArgInfo *ainfo = &linfo->args [i + sig->hasthis];
		char *name;
		int pindex = ainfo->pindex + ainfo->ndummy_fpargs;
		int j;

		for (j = 0; j < ainfo->ndummy_fpargs; ++j) {
			name = g_strdup_printf ("dummy_%d_%d", i, j);
			LLVMSetValueName (LLVMGetParam (method, ainfo->pindex + j), name);
			g_free (name);
		}

		if (ainfo->storage == LLVMArgVtypeInReg && ainfo->pair_storage [0] == LLVMArgNone && ainfo->pair_storage [1] == LLVMArgNone)
			continue;

		values [cfg->args [i + sig->hasthis]->dreg] = llvm::unwrap (LLVMGetParam (method, pindex));
		if (ainfo->storage == LLVMArgGsharedvtFixed || ainfo->storage == LLVMArgGsharedvtFixedVtype) {
			if (names [i] && names [i][0] != '\0')
				name = g_strdup_printf ("p_arg_%s", names [i]);
			else
				name = g_strdup_printf ("p_arg_%d", i);
		} else {
			if (names [i] && names [i][0] != '\0')
				name = g_strdup_printf ("arg_%s", names [i]);
			else
				name = g_strdup_printf ("arg_%d", i);
		}
		LLVMSetValueName (LLVMGetParam (method, pindex), name);
		g_free (name);
		if (ainfo->storage == LLVMArgVtypeByVal)
			mono_llvm_add_param_attr_with_type (LLVMGetParam (method, pindex), LLVM_ATTR_BY_VAL, this->type_to_llvm_arg_type (ainfo->type));

		if (ainfo->storage == LLVMArgVtypeByRef || ainfo->storage == LLVMArgVtypeAddr) {
			/* For OP_LDADDR */
			cfg->args [i + sig->hasthis]->opcode = OP_VTARG_ADDR;
		}
	}
	g_free (names);

	max_block_num = 0;
	for (bb = cfg->bb_entry; bb; bb = bb->next_bb)
		max_block_num = MAX (max_block_num, bb->block_num);
	this->bblocks = bblocks = g_new0 (BBInfo, max_block_num + 1);

	/* Add branches between non-consecutive bblocks */
	for (bb = cfg->bb_entry; bb; bb = bb->next_bb) {
		if (bb->last_ins && MONO_IS_COND_BRANCH_OP (bb->last_ins) &&
			bb->next_bb != bb->last_ins->inst_false_bb) {
			
			MonoInst *inst = static_cast<MonoInst*>(mono_mempool_alloc0 (cfg->mempool, sizeof (MonoInst)));
			inst->opcode = OP_BR;
			inst->inst_target_bb = bb->last_ins->inst_false_bb;
			mono_bblock_add_inst (bb, inst);
		}
	}

	/*
	 * Make a first pass over the code to precreate PHI nodes/set INDIRECT flags.
	 */
	for (bb = cfg->bb_entry; bb; bb = bb->next_bb) {
		MonoInst *ins;
		llvm::IRBuilder<> *builder;
		char *dname;
		char dname_buf[128];

		builder = this->create_builder ();

		for (ins = bb->code; ins; ins = ins->next) {
			switch (ins->opcode) {
			case OP_PHI:
			case OP_FPHI:
			case OP_VPHI:
			case OP_XPHI: {
				LLVMTypeRef phi_type = llvm_type_to_stack_type (cfg, this->type_to_llvm_type (m_class_get_byval_arg (ins->klass)));
				LLVMTypeRef phi_etype = nullptr;

				if (!this->ok ())
					return;

				if (ins->opcode == OP_VPHI) {
					/* Treat valuetype PHI nodes as operating on the address itself */
					g_assert (ins->klass);
					phi_etype = this->type_to_llvm_type (m_class_get_byval_arg (ins->klass));
					phi_type = llvm::wrap (llvm::PointerType::get (this->llvm_ctx (), 0));
				}

				/* 
				 * Have to precreate these, as they can be referenced by
				 * earlier instructions.
				 */
				sprintf (dname_buf, "t%d", ins->dreg);
				dname = dname_buf;
				values [ins->dreg] = builder->CreatePHI (llvm::unwrap (phi_type), 0, dname);

				if (ins->opcode == OP_VPHI)
					this->addresses [ins->dreg] = this->create_address (llvm::wrap (values [ins->dreg]), phi_etype);

				this->phi_values.push_back (values [ins->dreg]);

				/* 
				 * Set the expected type of the incoming arguments since these have
				 * to have the same type.
				 */
				for (i = 0; i < ins->inst_phi_args [0]; i++) {
					int sreg1 = ins->inst_phi_args [i + 1];
					
					if (sreg1 != -1) {
						if (ins->opcode == OP_VPHI)
							this->is_vphi [sreg1] = TRUE;
						this->vreg_types [sreg1] = llvm::unwrap (phi_type);
					}
				}
				break;
				}
			case OP_LDADDR:
				(static_cast<MonoInst*>(ins->inst_p0))->flags |= MONO_INST_INDIRECT;
				break;
			default:
				break;
			}
		}
	}

	/* 
	 * Create an ordering for bblocks, use the depth first order first, then
	 * put the exception handling bblocks last.
	 */
	for (bb_index = 0; bb_index < cfg->num_bblocks; ++bb_index) {
		bb = cfg->bblocks [bb_index];
		if (!(bb->region != static_cast<guint>(-1) && !MONO_BBLOCK_IS_IN_REGION (bb, MONO_REGION_TRY))) {
			bblock_list.push_back (bb);
			bblocks [bb->block_num].added = TRUE;
		}
	}

	for (bb = cfg->bb_entry; bb; bb = bb->next_bb) {
		if (!bblocks [bb->block_num].added)
			bblock_list.push_back (bb);
	}

	/*
	 * Second pass: generate code.
	 */
	// Emit entry point
	entry_builder = this->create_builder ();
	entry_bb = this->get_bb (cfg->bb_entry);
	entry_builder->SetInsertPoint (llvm::unwrap (entry_bb));
	emit_entry_bb (entry_builder);
	/* Runs the cctor this body owes; may decline the method outright. */
	emit_class_init_guards (entry_builder);
	if (!this->ok ())
		return;

	for (bb = cfg->bb_entry; bb; bb = bb->next_bb) {
		int clause_index;
		char name [128];

		if (this->cfg->interp_entry_only || !(bb->region != static_cast<guint>(-1) && (bb->flags & BB_EXCEPTION_HANDLER)))
			continue;

		clause_index = MONO_REGION_CLAUSE_INDEX (bb->region);
		this->region_to_handler [mono_get_block_region_notry (cfg, bb->region)] = bb;
		this->clause_to_handler [clause_index] = bb;

		/*
		 * Create a new bblock which CALL_HANDLER/landing pads can branch to, because branching to the
		 * LLVM bblock containing a landing pad causes problems for the
		 * LLVM optimizer passes.
		 */
		sprintf (name, "BB%d_CALL_HANDLER_TARGET", bb->block_num);
		this->bblocks [bb->block_num].call_handler_target_bb = append_basic_block (this->lmethod, name);
	}

	for (MonoBasicBlock *bb : bblock_list) {
		// Prune unreachable mono BBs.
		if (!(bb == cfg->bb_entry || bb->in_count > 0))
			continue;

		this->process_bb (bb);
		if (!this->ok ())
			return;
	}
	mono_memory_barrier ();

	/* Add incoming phi values */
	for (bb = cfg->bb_entry; bb; bb = bb->next_bb) {
		GSList *l, *ins_list;

		ins_list = bblocks [bb->block_num].phi_nodes;

		for (l = ins_list; l; l = l->next) {
			PhiNode *node = static_cast<PhiNode*>(l->data);
			MonoInst *phi = node->phi;
			int sreg1 = node->sreg;
			LLVMBasicBlockRef in_bb;

			if (sreg1 == -1)
				continue;

			in_bb = this->get_end_bb (node->in_bb);

			if (this->unreachable [node->in_bb->block_num])
				continue;

			if (phi->opcode == OP_VPHI) {
				/*
				 * Under opaque pointers comparing LLVMTypeOf () of the two values
				 * is vacuous (ptr == ptr). Compare the tracked element types
				 * instead -- this is the only check that an incoming address's
				 * pointee matches the type the VPHI was created with.
				 */
				g_assert (this->addresses [sreg1]);
				g_assert (this->addresses [phi->dreg]);
				g_assert (this->addresses [sreg1]->type == this->addresses [phi->dreg]->type);
				llvm::cast<llvm::PHINode> (values [phi->dreg])->addIncoming (this->addresses [sreg1]->value, llvm::unwrap (in_bb));
			} else {
				if (!values [sreg1]) {
					/* Can happen with values in EH clauses */
					this->set_failure ("incoming phi sreg1");
					return;
				}
				if (values [sreg1]->getType () != values [phi->dreg]->getType ()) {
					this->set_failure ("incoming phi arg type mismatch");
					return;
				}
				g_assert (values [sreg1]->getType () == values [phi->dreg]->getType ());
				llvm::cast<llvm::PHINode> (values [phi->dreg])->addIncoming (values [sreg1], llvm::unwrap (in_bb));
			}
		}
	}

	/* Nullify empty phi instructions */
	for (bb = cfg->bb_entry; bb; bb = bb->next_bb) {
		GSList *l, *ins_list;

		ins_list = bblocks [bb->block_num].phi_nodes;

		for (l = ins_list; l; l = l->next) {
			PhiNode *node = static_cast<PhiNode*>(l->data);
			MonoInst *phi = node->phi;
			LLVMValueRef phi_ins = llvm::wrap (values [phi->dreg]);

			if (!phi_ins)
				/* Already removed */
				continue;

			if (LLVMCountIncoming (phi_ins) == 0) {
				mono_llvm_replace_uses_of (phi_ins, llvm::wrap (llvm::Constant::getNullValue (llvm::unwrap (LLVMTypeOf (phi_ins)))));
				LLVMInstructionEraseFromParent (phi_ins);
				values [phi->dreg] = nullptr;
			}
		}
	}

	/* Create the SWITCH statements for ENDFINALLY instructions */
	for (bb = cfg->bb_entry; bb; bb = bb->next_bb) {
		BBInfo *info = &bblocks [bb->block_num];
		GSList *l;
		for (l = info->endfinally_switch_ins_list; l; l = l->next) {
			llvm::SwitchInst *switch_ins = static_cast<llvm::SwitchInst*>(l->data);
			GSList *bb_list = info->call_handler_return_bbs;

			GSList *bb_list_iter;
			i = 0;
			for (bb_list_iter = bb_list; bb_list_iter; bb_list_iter = g_slist_next (bb_list_iter)) {
				switch_ins->addCase (llvm::cast<llvm::ConstantInt> (llvm::ConstantInt::get (llvm::Type::getInt32Ty (this->llvm_ctx ()), i + 1, false)), llvm::unwrap (static_cast<LLVMBasicBlockRef>(bb_list_iter->data)));
				i ++;
			}
		}
	}

	this->module->max_method_idx = MAX (this->module->max_method_idx, static_cast<int>(cfg->method_index));

	if (mini_get_debug_options ()->llvm_disable_inlining)
		mono_llvm_add_func_attr (method, LLVM_ATTR_NO_INLINE);

after_codegen:
	if (cfg->verbose_level > 1) {
		g_print ("\n*** Unoptimized LLVM IR for %s ***\n", mono_method_full_name (cfg->method, TRUE));
		mono_llvm_dump_module (this->lmodule);
		g_print ("***\n\n");
	}

	{
		LLVMValueRef md_args [16];
		LLVMValueRef md_node;

		md_args [0] = md_string (this->llvm_ctx (), this->method_name, strlen (this->method_name));
		md_args [1] = llvm::wrap (llvm::ConstantInt::get (llvm::Type::getInt32Ty (this->llvm_ctx ()), 1, false));
		md_node = ::md_node (this->llvm_ctx (), md_args, 2);
		LLVMAddNamedMetadataOperand (lmodule, "mono.function_indexes", md_node);
	}

	//LLVMVerifyFunction (method, 0);

	this->finish_il_debug_info ();

	if (this->translate_only)
		/*
		 * Materialize-into-caller path: the callee body is done; the tier-1
		 * inliner owns it from here (inlines or strips it). Skip finalize
		 * (optimize + JIT the module) and the method<->lmethod bookkeeping.
		 */
		return;

	this->llvm_jit_finalize_method ();

	if (this->module->method_to_lmethod)
		g_hash_table_insert (this->module->method_to_lmethod, cfg->method, this->lmethod);

	if (this->module->idx_to_lmethod)
		g_hash_table_insert (this->module->idx_to_lmethod, GINT_TO_POINTER (cfg->method_index), this->lmethod);
}

/*
 * mono_llvm_create_vars:
 *
 *   Same as mono_arch_create_vars () for LLVM.
 */
void
mono_llvm_create_vars (MonoCompile *cfg)
{
	mono_arch_create_vars (cfg);

	cfg->lmf_ir = TRUE;
}

/*
 * mono_llvm_emit_call:
 *
 *   Same as mono_arch_emit_call () for LLVM.
 */
void
mono_llvm_emit_call (MonoCompile *cfg, MonoCallInst *call)
{
	MonoInst *in;
	MonoMethodSignature *sig;
	int i, n;
	LLVMArgInfo *ainfo;

	sig = call->signature;
	n = sig->param_count + sig->hasthis;

	call->cinfo = get_llvm_call_info (cfg, sig);

	if (cfg->disable_llvm)
		return;

	if (sig->call_convention == MONO_CALL_VARARG) {
		cfg->exception_message = g_strdup ("varargs");
		cfg->disable_llvm = TRUE;
	}

	for (i = 0; i < n; ++i) {
		MonoInst *ins;

		ainfo = call->cinfo->args + i;

		in = call->args [i];
			
		/* Simply remember the arguments */
		switch (ainfo->storage) {
		case LLVMArgNormal: {
			MonoType *t = (sig->hasthis && i == 0) ? m_class_get_byval_arg (mono_get_intptr_class ()) : ainfo->type;
			int opcode;

			opcode = mono_type_to_regmove (cfg, t);
			if (opcode == OP_FMOVE) {
				MONO_INST_NEW (cfg, ins, OP_FMOVE);
				ins->dreg = mono_alloc_freg (cfg);
			} else if (opcode == OP_LMOVE) {
				MONO_INST_NEW (cfg, ins, OP_LMOVE);
				ins->dreg = mono_alloc_lreg (cfg);
			} else if (opcode == OP_RMOVE) {
				MONO_INST_NEW (cfg, ins, OP_RMOVE);
				ins->dreg = mono_alloc_freg (cfg);
			} else {
				MONO_INST_NEW (cfg, ins, OP_MOVE);
				ins->dreg = mono_alloc_ireg (cfg);
			}
			ins->sreg1 = in->dreg;
			break;
		}
		case LLVMArgVtypeByVal:
		case LLVMArgVtypeByRef:
		case LLVMArgVtypeInReg:
		case LLVMArgVtypeAddr:
		case LLVMArgVtypeAsScalar:
		case LLVMArgAsIArgs:
		case LLVMArgAsFpArgs:
		case LLVMArgGsharedvtVariable:
		case LLVMArgGsharedvtFixed:
		case LLVMArgGsharedvtFixedVtype:
			MONO_INST_NEW (cfg, ins, OP_LLVM_OUTARG_VT);
			ins->dreg = mono_alloc_ireg (cfg);
			ins->sreg1 = in->dreg;
			ins->inst_p0 = mono_mempool_alloc0 (cfg->mempool, sizeof (LLVMArgInfo));
			memcpy (ins->inst_p0, ainfo, sizeof (LLVMArgInfo));
			ins->inst_vtype = ainfo->type;
			ins->klass = mono_class_from_mono_type_internal (ainfo->type);
			break;
		default:
			cfg->exception_message = g_strdup ("ainfo->storage");
			cfg->disable_llvm = TRUE;
			return;
		}

		if (!cfg->disable_llvm) {
			MONO_ADD_INS (cfg->cbb, ins);
			mono_call_inst_add_outarg_reg (cfg, call, ins->dreg, 0, FALSE);
		}
	}
}
void
mono_llvm_init (gboolean enable_jit)
{
	if (enable_jit)
		mono_llvm_jit_init ();
}

void
mono_llvm_cleanup (void)
{
	/*
	 * Nothing to do: the translator holds no state outliving a compile. Tearing
	 * down what does outlive one - the compile workers, then the engine and the
	 * code it owns - is mini_cleanup ()'s job, and happens before this runs.
	 */
}

void
mono_llvm_free_domain_info (MonoDomain *domain)
{
	/*
	 * Nothing to do: the translator's LLVM state is per-compile, created and
	 * destroyed inside mono_llvm_emit_method (), so a domain never accumulates
	 * any. Reclaiming the domain's compiled CODE is a separate path -
	 * mono_llvm_jit_release_domain () in the engine.
	 */
}

/*
 * AOT support has been removed from this backend. --aot=llvm cannot work
 * against unmodified LLVM 18 (its pipeline needs out-of-tree mono passes --
 * -place-safepoints / -spp-all-backedges -- that stock LLVM lacks), so the AOT
 * code paths were dead. These entry points remain only because aot-compiler.c
 * references them unconditionally; they must never be reached.
 */

void
mono_llvm_create_aot_module (MonoAssembly *assembly, const char *global_prefix, int initial_got_size, LLVMModuleFlags flags)
{
	g_error ("LLVM AOT compilation is not supported by this build.");
}

void
mono_llvm_fixup_aot_module (void)
{
	g_error ("LLVM AOT compilation is not supported by this build.");
}

void
mono_llvm_emit_aot_file_info (MonoAotFileInfo *info, gboolean has_jitted_code)
{
	g_error ("LLVM AOT compilation is not supported by this build.");
}

gpointer
mono_llvm_emit_aot_data_aligned (const char *symbol, guint8 *data, int data_len, int align)
{
	g_error ("LLVM AOT compilation is not supported by this build.");
	return NULL;
}

gpointer
mono_llvm_emit_aot_data (const char *symbol, guint8 *data, int data_len)
{
	return mono_llvm_emit_aot_data_aligned (symbol, data, data_len, 8);
}

void
mono_llvm_emit_aot_module (const char *filename, const char *cu_name)
{
	g_error ("LLVM AOT compilation is not supported by this build.");
}

/* ------------------------------------------------------------------------ *
 * Tier-1 inliner support: root eligibility + lazy callee materialization.
 *
 * These are the mono-aware half of the top-down inliner; the pure-LLVM pass
 * (passes/inliner.cpp) reaches them through passes/inliner-support.hpp.
 * ------------------------------------------------------------------------ */

namespace mono {

bool
tier1_root_allows_inlining (MonoCompile *cfg)
{
	if (!cfg)
		return false;
	/*
	 * Debug method (DebuggableAttribute.IsJITOptimizerDisabled), or -O=inline
	 * cleared. A NOOPTIMIZATION method can't reach here as a root: it never
	 * compiles under LLVM at all, since mono_llvm_check_method_supported
	 * declines it up front.
	 */
	if (cfg->disable_inline)
		return false;
	if (!(cfg->opt & MONO_OPT_INLINE))
		return false;
	return true;
}

const char *
tier1_root_refusal_reason (MonoCompile *cfg)
{
	if (cfg->disable_inline)
		return "refuse-root-disable-inline";
	if (!(cfg->opt & MONO_OPT_INLINE))
		return "refuse-root-no-opt-inline";
	return "refuse-root";
}

MonoMethod *
managed_method_from_symbol (const char *sym)
{
	return mono_llvm_method_from_symbol (sym);
}

/* Defined below, next to the other callee-eligibility predicates. */
static bool callee_needs_generic_context (MonoMethod *method);

MonoMethod *
resolve_exact_virtual_target (MonoMethod *declared, MonoClass *klass, const char **reason)
{
	*reason = "refuse-devirt";

	if (!declared || !klass)
		return nullptr;

	/*
	 * A remoted receiver dispatches through a proxy: mini_emit_method_call_full ()
	 * routes these to a remoting_invoke_with_check wrapper rather than the method
	 * itself, and resolving the slot here would skip it.
	 */
	if (mono_class_is_marshalbyref (klass) || mono_class_is_marshalbyref (declared->klass)) {
		*reason = "refuse-devirt-marshalbyref";
		return nullptr;
	}

	/*
	 * A signature that will not load leaves nothing to decide from.
	 *
	 * A generic virtual method is fine here even though it dispatches through the
	 * imt argument rather than the plain vtable slot: mono_class_get_virtual_method ()
	 * resolves it the same way the runtime does - the generic method DEFINITION
	 * holds the slot, and the result is inflated with the call site's own method
	 * context afterwards.
	 */
	if (!mono_method_signature_internal (declared)) {
		*reason = "refuse-devirt-no-signature";
		return nullptr;
	}

	/*
	 * An abstract or open class is not something an object is ever exactly an
	 * instance of, so being asked to resolve against one means the receiver
	 * proof is wrong rather than merely imprecise.
	 */
	if (mono_class_get_flags (klass) & TYPE_ATTRIBUTE_ABSTRACT) {
		*reason = "refuse-devirt-abstract-receiver";
		return nullptr;
	}

	/*
	 * The slot holds an unbox trampoline when the receiver is boxed, and the
	 * method behind it expects `this` already advanced past the object header.
	 * Adjusting for that is a later slice; refuse rather than pass a boxed
	 * pointer to a body that will treat it as unboxed.
	 */
	if (m_class_is_valuetype (klass)) {
		*reason = "refuse-devirt-valuetype-receiver";
		return nullptr;
	}

	ERROR_DECL (error);
	/*
	 * Reads the vtable, so it has to be built first. This runs on a compile
	 * thread: setup takes metadata locks and can fail a type load, but it runs
	 * no cctors, which is the constraint that matters here.
	 */
	mono_class_setup_vtable (klass);
	if (mono_class_has_failure (klass)) {
		*reason = "refuse-devirt-typeload";
		return nullptr;
	}

	MonoMethod *target = mono_class_get_virtual_method (klass, declared, FALSE, error);
	if (!target || !is_ok (error)) {
		mono_error_cleanup (error);
		*reason = "refuse-devirt-unresolved";
		return nullptr;
	}

	if (target->flags & METHOD_ATTRIBUTE_ABSTRACT) {
		*reason = "refuse-devirt-abstract-target";
		return nullptr;
	}

	/*
	 * A wrapper target means the runtime interposes something on the call
	 * (remoting, COM, unbox); the direct edge would bypass whatever that is.
	 */
	if (target->wrapper_type != MONO_WRAPPER_NONE) {
		*reason = "refuse-devirt-wrapper-target";
		return nullptr;
	}

	/*
	 * Refused for the same reason materialize_callee () refuses it: there is no
	 * single body to name, and the trampoline the direct edge would go through
	 * expects a context this site has no way to supply.
	 */
	if (callee_needs_generic_context (target)) {
		*reason = "refuse-devirt-needs-rgctx";
		return nullptr;
	}

	*reason = nullptr;
	return target;
}

bool
target_needs_rgctx (MonoMethod *target, const Tier1Root &root)
{
	if (!target || !root.cfg)
		return true;                 /* cannot tell -> refuse the site */

	return mini_method_call_passes_rgctx (root.cfg, target) != FALSE;
}

llvm::Function *
direct_callee_decl (MonoMethod *target, const llvm::CallBase &site, const Tier1Root &root)
{
	if (!target || !root.cfg || !root.module)
		return nullptr;

	llvm::FunctionType *sig = site.getFunctionType ();

	ERROR_DECL (error);
	gpointer tramp = mono_create_jit_trampoline (root.cfg->domain, target, error);
	if (!tramp || !is_ok (error)) {
		mono_error_cleanup (error);
		return nullptr;
	}

	const char *name = mono_llvm_method_symbol (target);
	mono_llvm_jit_register_symbol (name, tramp);

	llvm::Module *module = llvm::unwrap (root.module->lmodule);
	auto callee = module->getOrInsertFunction (name, sig);
	auto *fn = llvm::dyn_cast<llvm::Function> (callee.getCallee ());

	/*
	 * Check the type rather than trusting getOrInsertFunction (). Under opaque
	 * pointers every function is a `ptr`, so an existing declaration under this
	 * name comes back as-is even when its type is not the one we asked for -
	 * and the same method legitimately gets declared under more than one
	 * signature (with and without 'this'; see the note in process_call ()).
	 * Handing that back would be worse than declining: setCalledFunction ()
	 * adopts the callee's type, silently reinterpreting the site's arguments.
	 */
	if (!fn || fn->getFunctionType () != sig)
		return nullptr;

	/*
	 * Mirror the site's `nest` parameter onto the declaration. A FunctionType
	 * carries types and nothing else, so this attribute is the only record that
	 * the extra parameter is an imt argument rather than a real one - and
	 * materialize_callee () has to know, or it builds a body of the wrong shape.
	 */
	for (unsigned i = 0; i < site.arg_size (); ++i)
		if (site.paramHasAttr (i, llvm::Attribute::Nest))
			fn->addParamAttr (i, llvm::Attribute::Nest);

	return fn;
}

/*
 * True if METHOD still needs a generic context to be compiled - it is open
 * (a generic type/method definition) or shared over type parameters.
 *
 * Such a callee cannot be folded into the root. mini_method_compile () only
 * turns on cfg->gshared for a method it is about to share itself, and an
 * already-shared method is not sharable again, so the front-end would compile
 * the type-parameter-bearing body as if it were concrete and abort on the first
 * call site it cannot resolve without a context. Roots that carry an rgctx are
 * fine and expected - it is the callee that has to be context-free.
 *
 * This is checked up front, before that compile, because the context can arrive
 * via `this`/the receiver type or the method instantiation rather than an
 * explicit rgctx argument, none of which the pass's call-site nest check sees.
 */
static bool
callee_needs_generic_context (MonoMethod *method)
{
	/*
	 * The authoritative test, and the same one method-to-ir.c asserts on at
	 * every call site it imports. It catches the case none of the cheaper
	 * checks below do: a method whose context lives purely in its own
	 * instantiation, with no trace of it in the declaring class or the
	 * signature - ExecuteEvents.GetEventList<T> (GameObject, IList<Handler>)
	 * being the canonical shape.
	 */
	if (mono_method_check_context_used (method))
		return true;
	/* Open declaring type: the generic type definition itself (Box`1<T>). */
	if (mono_class_is_gtd (method->klass))
		return true;
	/* Declaring type instantiated over type parameters (the shared Box`1<T_REF>). */
	if (mono_class_is_open_constructed_type (m_class_get_byval_arg (method->klass)))
		return true;
	/*
	 * A generic method definition (uninstantiated). It has no context of its
	 * own for the check above to find, so it needs naming separately.
	 */
	if (method->is_generic)
		return true;
	MonoMethodSignature *sig = mono_method_signature_internal (method);
	if (!sig)
		return true;                 /* cannot tell -> be conservative */
	if (sig->has_type_parameters)
		return true;
	return false;
}

/*
 * True if CALLEE is a shared body over exactly the type parameters ROOT_CFG is
 * itself shared over - the same class instantiation and the same method
 * instantiation, on the same declaring class.
 *
 * Such a callee can be materialized as the shared body it is, rather than
 * refused by callee_needs_generic_context (). Its runtime generic context is the
 * root's own: the vtable behind `this`, or the vtable/mrgctx the root's front-end
 * already computed out of its rgctx for the call site (check_method_sharing (),
 * method-to-ir.c). Nothing in the folded body has to be re-derived, so folding it
 * in is the same substitution as for a context-free callee.
 *
 * Sharing the declaring class is also what covers the cctor: shared code cannot
 * bake a vtable, so emit_class_init_guards () plants no trigger in a shared body
 * (translator-call.cpp), and folding one in would otherwise drop the trigger its
 * own trampoline entry carried. On the same class instantiation there is nothing
 * to drop - the root was entered through that class's own trampoline. This is
 * the same line the front-end draws when it decides whether a gshared call site
 * needs an explicit class init (`cmethod->klass != method->klass`, the shared-
 * callee cctor check in mono_method_to_ir ()).
 *
 * MonoGenericInst is interned, so comparing the insts by pointer compares them by
 * value; and because a shared inst is built out of its own container's type
 * parameters, equality here also pins the instantiation to that container.
 */
static bool
callee_shares_root_context (MonoMethod *callee, MonoCompile *root_cfg)
{
	if (!root_cfg->gshared || root_cfg->gsharedvt)
		return false;
	/* gsharedvt bodies are declined wholesale by the backend. */
	if (mini_is_gsharedvt_sharable_method (callee))
		return false;
	if (callee->klass != root_cfg->method->klass)
		return false;

	MonoGenericContext *root_ctx = mono_method_get_context (root_cfg->method);
	MonoGenericContext *callee_ctx = mono_method_get_context (callee);
	if (!root_ctx || !callee_ctx)
		return false;

	return root_ctx->class_inst == callee_ctx->class_inst &&
	       root_ctx->method_inst == callee_ctx->method_inst;
}

/*
 * The vtable whose cctor the body being compiled for CFG has to trigger itself,
 * or NULL if there is nothing to do.
 *
 * Calling a method is one of the runtime's class-init triggers: the first call
 * lands in a jit trampoline, which compiles the method and then runs the
 * declaring class's cctor (mono_jit_compile_method_inner_1 ()). A tier-1 body
 * that the inliner folds into a caller has no such call left, so it carries the
 * trigger in its own prologue instead - see emit_class_init_guards (). Instance
 * methods included: .ctor is exactly the callee whose inlining would otherwise
 * lose the trigger, and passing METHOD as the caller is what keeps a cctor from
 * guarding against itself.
 *
 * Sets *INDETERMINATE when the answer cannot be established - an open or
 * context-dependent class has one vtable per instantiation, none of which is the
 * one to bake into the code, and a vtable that will not build is no better. The
 * caller decides what to do about that.
 */
MonoVTable *
pending_class_init_vtable (MonoCompile *cfg, bool *indeterminate)
{
	MonoMethod *method = cfg->method;
	MonoClass *klass = method->klass;

	if (!mono_class_needs_cctor_run (klass, method))
		return nullptr;

	if (mono_class_is_gtd (klass) ||
	    mono_class_is_open_constructed_type (m_class_get_byval_arg (klass)) ||
	    mini_class_check_context_used (cfg, klass)) {
		*indeterminate = true;
		return nullptr;
	}

	ERROR_DECL (error);
	MonoVTable *vtable = mono_class_vtable_checked (cfg->domain, klass, error);
	if (!vtable || !is_ok (error)) {
		mono_error_cleanup (error);
		*indeterminate = true;
		return nullptr;
	}

	/*
	 * Already initialized - nothing for a guard to trigger. This is the
	 * overwhelmingly common case: by the time a method is hot enough to promote,
	 * its class was initialized long ago.
	 */
	return vtable->initialized ? nullptr : vtable;
}

/*
 * Whether DECL was declared to take an imt argument in `nest`.
 *
 * Only devirt_callee_decl () marks one - an ordinary direct-call declaration is
 * built from a bare FunctionType and carries no parameter attributes at all, so
 * a false here means "nothing to add on top of what METHOD's own metadata says".
 */
static bool
decl_takes_nest_arg (const llvm::Function *decl)
{
	if (!decl)
		return false;

	const llvm::AttributeList attrs = decl->getAttributes ();
	for (unsigned i = 0; i < decl->getFunctionType ()->getNumParams (); ++i)
		if (attrs.hasParamAttr (i, llvm::Attribute::Nest))
			return true;

	return false;
}

llvm::Function *
materialize_callee (MonoMethod *method, const Tier1Root &root, const llvm::Function *decl)
{
	if (!method || !root.cfg)
		return nullptr;

	/*
	 * Conservative mono-side refusals, on top of whatever the front-end declines
	 * and the pass's own LLVM-level gates:
	 *  - wrappers have no directly-inlinable managed body (the real work lives in
	 *    the wrapped method), and inlining e.g. a synchronized wrapper's raw body
	 *    would drop the monitor enter/exit;
	 *  - synchronized methods, same reason;
	 *  - a callee that still needs a generic context, see below.
	 */
	if (method->wrapper_type != MONO_WRAPPER_NONE)
		return nullptr;
	if (method->iflags & METHOD_IMPL_ATTRIBUTE_SYNCHRONIZED)
		return nullptr;
	/*
	 * A callee that still needs a generic context has to be decided BEFORE its
	 * front-end runs: mini_method_compile () on a type-parameter-bearing body it
	 * was not told to share aborts partway rather than failing cleanly, so the
	 * cfg->gshared check below cannot cover it - by then the compile is over.
	 *
	 * The exception is a callee shared over exactly the root's own type
	 * parameters. Its context is the root's, which the call site already passes,
	 * so it is compiled as the shared body it is (JIT_FLAG_METHOD_IS_GSHARED)
	 * rather than refused.
	 */
	bool shared = false;
	if (callee_needs_generic_context (method)) {
		if (!callee_shares_root_context (method, root.cfg))
			return nullptr;
		shared = true;
	}

	/*
	 * For everything else, compile the exact instantiation rather than the shared
	 * body mono hands every reference-type instantiation of a sharable method.
	 * Clearing MONO_OPT_GSHARED is the whole switch: mini_method_compile () only
	 * redirects through mini_get_shared_method_full () when that bit is set.
	 *
	 * The specialized body is also the better one to fold in: constant type
	 * handles, no rgctx loads. It costs only the compile, since a materialized
	 * body is either inlined into the root or stripped - it is never published as
	 * the method's own code, so the shared body the runtime calls is untouched.
	 *
	 * JIT_FLAG_LLVM_IR_ONLY stops mini_method_compile before it emits. No cctors
	 * on this thread.
	 */
	guint32 opt = shared ? root.cfg->opt : (root.cfg->opt & ~MONO_OPT_GSHARED);
	int jit_flags = JIT_FLAG_LLVM | JIT_FLAG_LLVM_IR_ONLY | JIT_FLAG_NO_LLVM_FALLBACK;
	if (shared)
		jit_flags |= JIT_FLAG_METHOD_IS_GSHARED;

	/*
	 * Whether the call sites carry something in `nest`. Asked of the ROOT's cfg
	 * because that is the caller whose sites are being matched, and asked before
	 * the compile because the body has to be built to the same shape.
	 *
	 * A devirtualized site is the case metadata cannot answer: it passes an imt
	 * argument there, which is not a fact about METHOD at all, so the declaration
	 * it was rewritten against has to be consulted as well.
	 */
	bool passes_rgctx = mini_method_call_passes_rgctx (root.cfg, method) ||
	                    decl_takes_nest_arg (decl);

	MonoCompile *cfg = mini_method_compile (
		method, opt, root.cfg->domain, (JitFlags) jit_flags, 0, -1);
	if (!cfg)
		return nullptr;

	llvm::Function *result = nullptr;

	/* The front-end may have declined LLVM or produced a shared/gshared body. */
	if (cfg->disable_llvm || cfg->exception_type != MONO_EXCEPTION_NONE)
		goto done;
	if (cfg->gsharedvt)
		goto done;
	/* Shared where we did not ask for it, or unshared where we did. */
	if (!!cfg->gshared != shared)
		goto done;
	/*
	 * A body that wants an rgctx of its own from a call site that has none to give
	 * cannot be wired up: the two signatures would differ by that parameter. The
	 * two answers are made by the same rules from the same metadata
	 * (check_method_sharing () vs mono_get_vtable_var ()), so this is a backstop
	 * rather than an expected outcome.
	 */
	if (cfg->rgctx_var && !passes_rgctx)
		goto done;

	{
		/*
		 * Translate into the root's module, sharing its MonoLLVMModule - and so
		 * its LLVMContext, which that module and every type in it belong to. A
		 * fresh one here would build the callee body out of types from a
		 * different context, which is not a thing an LLVM module can hold.
		 */
		MonoLLVMModule *module = root.module;

		EmitContext *ctx = alloc_emit_context (cfg, module);
		/*
		 * Drop the intrinsic cache first. Its entries are LLVM Functions the
		 * root emitted into this same module, but we run from inside the
		 * optimization pipeline, and a pass that folds away the last use of an
		 * intrinsic leaves the declaration dead for GlobalOpt to delete - so a
		 * cached pointer can already be dangling by the time we get here.
		 * Rebuilding costs nothing: Intrinsic::getDeclaration () hands back the
		 * declaration the module still has, and only creates one when it really
		 * is gone.
		 */
		std::fill (module->intrins_by_id.begin (), module->intrins_by_id.end (), nullptr);
		ctx->lmodule = module->lmodule;
		ctx->translate_only = true;
		/*
		 * A specialized body has no use for the `nest` parameter, and a
		 * devirtualized interface site's imt argument is meaningless to any body
		 * at all - but it has to be accepted either way, or the two types would
		 * not match and the site could not be rewired. Dead is fine: it inlines
		 * away with everything that computed it. A shared body reads its context
		 * out of it.
		 */
		ctx->keep_rgctx_arg = passes_rgctx;

		/*
		 * Give this body's clauses a block of ids of their own in the root's
		 * numbering, so nothing it bakes into the IR - type_info globals, finally
		 * marker stackmap IDs, landing pad selector cases - can collide with the
		 * root's clauses or another callee's once the inliner folds them into one
		 * function. The entries are COPIED because cfg dies at the end of this
		 * call, taking its header with it.
		 */
		if (cfg->header && cfg->header->num_clauses) {
			std::vector<MonoExceptionClause> &table = root.module->clauses;

			ctx->clause_id_base = (int) table.size ();
			table.insert (table.end (), cfg->header->clauses,
			              cfg->header->clauses + cfg->header->num_clauses);

			/*
			 * Two things the root decided about itself back when it was emitted
			 * are now wrong, because they were answers about its OWN IL and this
			 * body's clauses are about to become part of it.
			 *
			 * mono-has-eh-clauses is what lets MonoEHGatherPass tell "every
			 * protected call optimized away" apart from "never had a try block",
			 * and a clause-free root would otherwise publish no section at all -
			 * so the inlined handler would simply never run.
			 *
			 * A finally's guard reads its exvar at an RBP-relative offset a
			 * stackmap hands back, so the frame pointer has to stay put whether
			 * the finally came from the root's IL or from here.
			 */
			root.func->addFnAttr ("mono-has-eh-clauses");

			for (int i = 0; i < cfg->header->num_clauses; ++i) {
				if (cfg->header->clauses [i].flags == MONO_EXCEPTION_CLAUSE_FINALLY)
					root.func->addFnAttr ("frame-pointer", "all");
			}
		}

		ctx->emit_method_inner ();

		if (!ctx->ok () || !ctx->lmethod) {
			cleanup_failed_emit (ctx);
		} else {
			result = llvm::unwrap<llvm::Function> (ctx->lmethod);
		}

		free_ctx (ctx);
	}

done:
	mono_destroy_compile (cfg);
	return result;
}

} // namespace mono


/*
  DESIGN:
  - Emit LLVM IR from the mono IR using the LLVM C API.
  - The original arch specific code remains, so we can fall back to it if we run
    into something we can't handle.
*/

/*  
  A partial list of issues:
  - Handling of opcodes which can throw exceptions.

      In the mono JIT, these are implemented using code like this:
	  method:
      <compare>
	  throw_pos:
	  b<cond> ex_label
	  <rest of code>
      ex_label:
	  push throw_pos - method
	  call <exception trampoline>

	  The problematic part is push throw_pos - method, which cannot be represented
      in the LLVM IR, since it does not support label values.
	  -> this can be implemented in AOT mode using inline asm + labels, but cannot
	  be implemented in JIT mode ?
	  -> a possible but slower implementation would use the normal exception 
      throwing code but it would need to control the placement of the throw code
      (it needs to be exactly after the compare+branch).
	  -> perhaps add a PC offset intrinsics ?

  - efficient implementation of .ovf opcodes.

	  These are currently implemented as:
	  <ins which sets the condition codes>
	  b<cond> ex_label

	  Some overflow opcodes are now supported by LLVM SVN.

  - exception handling, unwinding.
    - SSA is disabled for methods with exception handlers    
	- How to obtain unwind info for LLVM compiled methods ?
	  -> this is now solved by converting the unwind info generated by LLVM
	     into our format.
	- LLVM uses the c++ exception handling framework, while we use our home grown
      code, and couldn't use the c++ one:
      - its not supported under VC++, other exotic platforms.
	  - it might be impossible to support filter clauses with it.

  - trampolines.
  
    The trampolines need a predictable call sequence, since they need to disasm
    the calling code to obtain register numbers / offsets.

    LLVM currently generates this code in non-JIT mode:
	   mov    -0x98(%rax),%eax
	   callq  *%rax
    Here, the vtable pointer is lost. 
    -> solution: use one vtable trampoline per class.

  - passing/receiving the IMT pointer/RGCTX.
    -> solution: pass them as normal arguments ?

  - argument passing.
  
	  LLVM does not allow the specification of argument registers etc. This means
      that all calls are made according to the platform ABI.

  - passing/receiving vtypes.

      Vtypes passed/received in registers are handled by the front end by using
	  a signature with scalar arguments, and loading the parts of the vtype into those
	  arguments.

	  Vtypes passed on the stack are handled using the 'byval' attribute.

  - ldaddr.

    Supported though alloca, we need to emit the load/store code.

  - types.

    The mono JIT uses pointer sized iregs/double fregs, while LLVM uses precisely
    typed registers, so we have to keep track of the precise LLVM type of each vreg.
    This is made easier because the IR is already in SSA form.
    An additional problem is that our IR is not consistent with types, i.e. i32/i64 
	types are frequently used incorrectly.
*/

/*
  AOT SUPPORT:
  Emit LLVM bytecode into a .bc file, compile it using llc into a .s file, then link
  it with the file containing the methods emitted by the JIT and the AOT data
  structures.
*/

/* FIXME: Normalize some aspects of the mono IR to allow easier translation, like:
 *   - each bblock should end with a branch
 *   - setting the return value, making cfg->ret non-volatile
 * - avoid some transformations in the JIT which make it harder for us to generate
 *   code.
 * - use pointer types to help optimizations.
 */

#else /* DISABLE_JIT */

void
mono_llvm_cleanup (void)
{
}

void
mono_llvm_free_domain_info (MonoDomain *domain)
{
}

void
mono_llvm_init (gboolean enable_jit)
{
}

#endif /* DISABLE_JIT */

#if !defined(DISABLE_JIT) && !defined(MONO_CROSS_COMPILE)

/* LLVM JIT support */

gpointer
gc_poll_cold_wrapper_code (void)
{
	static std::atomic<gpointer> compiled {nullptr};

	gpointer code = compiled.load (std::memory_order_acquire);
	if (code)
		return code;

	ERROR_DECL (error);
	/* Compiling a method here is a bit ugly, but it works */
	MonoMethod *wrapper = mono_marshal_get_llvm_func_wrapper (LLVM_FUNC_WRAPPER_GC_POLL);
	code = mono_jit_compile_method (wrapper, error);
	mono_error_assert_ok (error);

	/*
	 * Two threads can get here at once and both compile the wrapper. That is
	 * fine and not worth a lock: mono_jit_compile_method () publishes one body
	 * per method, so they arrive at the same address and whichever store lands
	 * second stores the same value.
	 */
	compiled.store (code, std::memory_order_release);

	return code;
}

/*
 * One `.llvm_stackmaps` record, reduced to what the recovery passes below need:
 * which stackmap it came from (ID), where in the code it sits, and the frame
 * home of its first location operand.
 */
struct StackmapRecord {
	guint64 id;
	guint32 instr_off;
	bool loc_is_direct;
	guint16 loc_dwarf_reg;
	gint32 loc_offset;
};

/*
 * Decode every record in the `.llvm_stackmaps` section into OUT, or return
 * false if the section is absent or its header does not validate.
 *
 * Records carrying no location operand are skipped: both consumers key off a
 * location, and LLVM is free to emit bare records of its own.
 */
static bool
parse_stackmap_records (const guint8 *stackmaps, guint32 size, std::vector<StackmapRecord> &out)
{
	out.clear ();

	if (!stackmaps)
		return false;

	llvm::ArrayRef<uint8_t> section (stackmaps, size);

	/*
	 * The parser itself only asserts on a bad header, and this LLVM is built
	 * without assertions - so check first rather than walk a malformed section.
	 */
	if (llvm::Error err = llvm::StackMapParser<llvm::endianness::little>::validateHeader (section)) {
		llvm::consumeError (std::move (err));
		return false;
	}

	llvm::StackMapParser<llvm::endianness::little> parser (section);

	for (const auto &record : parser.records ()) {
		if (record.getNumLocations () == 0)
			continue;

		auto loc = record.getLocation (0);
		StackmapRecord sr;

		sr.id = record.getID ();
		sr.instr_off = record.getInstructionOffset ();
		sr.loc_is_direct = loc.getKind () ==
			llvm::StackMapParser<llvm::endianness::little>::LocationKind::Direct;
		sr.loc_dwarf_reg = loc.getDwarfRegNum ();
		/* getOffset () asserts unless the location is Direct or Indirect. */
		sr.loc_offset = sr.loc_is_direct ? loc.getOffset () : 0;
		out.push_back (sr);
	}

	return true;
}

/*
 * recover_gshared_this_slot:
 *
 *   Parse the `.llvm_stackmaps` section LLVM emitted for a gshared method and
 * publish the home location of the this/mrgctx slot into cfg->llvm_this_reg /
 * cfg->llvm_this_offset, the fields mini.c's generic-jit-info setup reads
 * (mini.c:2573-2577) so that a stack walk over a live frame of this method can
 * reconstruct its generic context (mini-exceptions.c:831-835).
 *
 * The translator planted exactly one llvm.experimental.stackmap over the alloca
 * that holds this/mrgctx (emit_this_slot_stackmap, translator-call.cpp), tagged
 * with MONO_LLVM_THIS_SLOT_STACKMAP_ID. LLVM lowers an alloca operand to a
 * Direct location {DwarfReg, Offset} whose value is the slot's ADDRESS =
 * reg+offset; the consumer dereferences it, reading the stored this/mrgctx.
 * this_in_reg is forced to 0 by the LLVM branch in mini.c, matching Direct.
 *
 * Returns FALSE (declining the method to the classic JIT, per CAP-EH-0: a
 * plausible-but-wrong this-slot is worse than a fallback) if the section is
 * absent, malformed, carries no this-slot record, or the location is anything
 * other than Direct - in which case *(reg+offset) would not equal this and
 * stack walks would read garbage.
 */
static bool
recover_gshared_this_slot (MonoCompile *cfg, guint8 *stackmaps, guint32 size)
{
	std::vector<StackmapRecord> records;
	const StackmapRecord *this_slot = nullptr;

	/*
	 * A gshared reference-type instance default-interface-method with
	 * context_used == 0 has rgctx_var == NULL, so the translator stackmapped the
	 * `this` slot (translator-call.cpp:321). But the stack-walk consumer routes by
	 * method kind and treats a default method as carrying an mrgctx, not `this`
	 * (get_generic_info_from_stack_frame, mini-exceptions.c:839). The recovered
	 * `this` value would then be misread as an mrgctx, so decline to the classic
	 * JIT instead of publishing it (CAP-EH-0).
	 */
	if (!cfg->rgctx_var && mini_method_is_default_method (cfg->method)) {
		TRACE_FAILURE_CFG (cfg, "gshared default-interface-method this-slot would be misread as mrgctx");
		return false;
	}

	if (!parse_stackmap_records (stackmaps, size, records))
		return false;

	/*
	 * Select by ID rather than by position: a method that also has a finally
	 * carries the abort-guard records in the same section, and nothing orders
	 * this one first.
	 */
	for (const StackmapRecord &r : records) {
		if (r.id == MONO_LLVM_THIS_SLOT_STACKMAP_ID) {
			this_slot = &r;
			break;
		}
	}
	if (!this_slot)
		return false;

	if (!this_slot->loc_is_direct) {
		TRACE_FAILURE_CFG (cfg, "gshared this-slot stackmap not Direct");
		return false;
	}

	/*
	 * mono_dwarf_reg_to_hw_reg () indexes map_dwarf_reg_to_hw_reg [NUM_DWARF_REGS]
	 * with no bounds check. dwarf_reg is only ever RBP/RSP for Direct locations, so
	 * this never fires in practice, but guard it like every other read above rather
	 * than index out of bounds on a malformed stackmap.
	 */
	if (!mono_dwarf_reg_is_valid (this_slot->loc_dwarf_reg)) {
		TRACE_FAILURE_CFG (cfg, "gshared this-slot stackmap dwarf reg out of range");
		return false;
	}

	cfg->llvm_this_reg = mono_dwarf_reg_to_hw_reg (this_slot->loc_dwarf_reg);
	cfg->llvm_this_offset = this_slot->loc_offset;
	return true;
}

/*
 * Can a stack walk rebuild HW_REG for the frame it is looking at? True for SP
 * and for the callee-saved registers the unwind info restores; a caller-saved
 * register's value is long gone by then.
 */
static bool
exvar_base_reg_is_recoverable (int hw_reg)
{
	switch (hw_reg) {
	case AMD64_RSP:
	case AMD64_RBP:
	case AMD64_RBX:
	case AMD64_R12:
	case AMD64_R13:
	case AMD64_R14:
	case AMD64_R15:
		return true;
	default:
		return false;
	}
}

/*
 * One opening marker: which FINALLY clause it belongs to, where it ended up, and
 * the frame slot it named.
 */
struct FinallyMarker {
	guint32 clause_index = 0;
	guint32 pc = 0;
	gint32 offset = 0;
	int base_reg = -1;
};

static bool
same_exvar_slot (const FinallyMarker &a, const FinallyMarker &b)
{
	return a.offset == b.offset && a.base_reg == b.base_reg;
}

/*
 * recover_finally_markers:
 *
 *   Recover the frame slot the thread-abort guard flags a running finally
 * through - the byte install_handler_block_guard () writes and the shared IR
 * checks once the finally returns - for every opening marker that survived.
 *
 * Which PCs the handler body occupies is a separate question, about where the
 * code ended up rather than about the frame, and MonoFinallyRangePass
 * (engine.cpp) answers it after LLVM has finished moving code around.
 *
 * One record per surviving marker, NOT one per clause. A clause's body can end
 * up in the frame more than once - the optimizer duplicates it along its entry
 * paths, and inlining the same body at two call sites brings two copies in - and
 * those are not always the same slot. A plain code clone reuses the one alloca,
 * but two inlined copies each get their own, so the slot belongs to the body run
 * rather than to the clause. The caller joins each run to the marker inside it.
 *
 * A FINALLY clause with no record is not an error - its body was optimized away
 * entirely, so there is nothing for a thread to be stopped inside.
 *
 * Returns false if a marker named a slot the guard could not reach, in which
 * case the caller declines the method to the classic JIT (CAP-EH-0).
 */
static bool
recover_finally_markers (const std::vector<MonoExceptionClause> &clauses,
                         guint8 *stackmaps, guint32 size,
                         std::vector<FinallyMarker> &out)
{
	std::vector<StackmapRecord> records;
	bool has_finally = false;

	out.clear ();

	for (const MonoExceptionClause &c : clauses) {
		if (c.flags == MONO_EXCEPTION_CLAUSE_FINALLY)
			has_finally = true;
	}
	if (!has_finally)
		return true;

	/*
	 * No section at all means every finally body optimized away, so there is
	 * nothing for a thread to be stopped inside and no guard to publish. A
	 * section that is present but unreadable is our own emission breaking.
	 */
	if (!stackmaps || !size)
		return true;
	g_assert (parse_stackmap_records (stackmaps, size, records));

	for (const StackmapRecord &r : records) {
		if ((r.id >> 32) != (MONO_LLVM_FINALLY_STACKMAP_ID_BASE >> 32))
			continue;

		guint32 clause_index = static_cast<guint32>(r.id & MONO_LLVM_FINALLY_STACKMAP_ID_MASK);

		/* We built this ID out of the same clause table we are checking it against. */
		g_assert (clause_index < clauses.size ());
		g_assert (clauses [clause_index].flags == MONO_EXCEPTION_CLAUSE_FINALLY);

		/* An alloca operand always lowers to Direct: reg+offset is its address. */
		g_assert (r.loc_is_direct);
		g_assert (mono_dwarf_reg_is_valid (r.loc_dwarf_reg));

		/*
		 * Which register LLVM homed the slot against is its own choice - FP
		 * normally, SP once it realigns the frame, a base pointer if that frame
		 * also has var-sized objects - so carry the register, don't assume one.
		 */
		int base_reg = mono_dwarf_reg_to_hw_reg (r.loc_dwarf_reg);

		if (!exvar_base_reg_is_recoverable (base_reg))
			return false;

		FinallyMarker m;
		m.clause_index = clause_index;
		m.pc = r.instr_off;
		m.offset = r.loc_offset;
		m.base_reg = base_reg;
		out.push_back (m);
	}

	return true;
}

/*
 * Fill in CFG's tier-1 native_offset -> il_offset map from ROWS, the line table
 * the engine read out of this method's `.debug_line` (each row's line is the IL
 * offset, biased by one). Fills cfg->llvm_seq_points/n_llvm_seq_points, which
 * create_jit_info () (mini.c) copies onto the method's MonoJitInfo.
 *
 * This is diagnostics-quality data - stack traces, profiler attribution - not
 * something whose absence makes execution wrong, so an empty or missing line
 * table just leaves the map empty rather than declining the method to the
 * classic JIT.
 */
static void
recover_il_seq_points (MonoCompile *cfg, const std::vector<mono::MonoIlLineRow> &rows)
{
	cfg->llvm_seq_points = NULL;
	cfg->n_llvm_seq_points = 0;

	if (rows.empty ())
		return;

	/*
	 * Allocated out of cfg->mem_manager, the same pool the MonoJitInfo itself
	 * comes from (create_jit_info (), mini.c), so it is reclaimed alongside it
	 * with no separate free needed.
	 */
	MonoLLVMSeqPoint *map = (MonoLLVMSeqPoint *) mono_mem_manager_alloc (
		cfg->mem_manager, rows.size () * sizeof (MonoLLVMSeqPoint));

	for (size_t i = 0; i < rows.size (); ++i) {
		map [i].native_offset = rows [i].native_offset;
		map [i].il_offset = rows [i].il_offset;
	}

	cfg->llvm_seq_points = map;
	cfg->n_llvm_seq_points = (guint32) rows.size ();
}

/*
 * Fill in CFG's inlined-frame table from ROWS, the inline chains the engine
 * recovered. NAMES maps a subprogram name back to the method it describes - the
 * translator built it while translating, since an inlined callee leaves no call
 * site naming it.
 *
 * A row whose method cannot be resolved is dropped rather than guessed at: this
 * is diagnostic data, and a frame attributed to the wrong method is worse than
 * one that is missing.
 */
static void
recover_il_inline_frames (MonoCompile *cfg, const std::vector<mono::MonoIlInlineRow> &rows,
                          const std::map<std::string, MonoMethod *> &names)
{
	cfg->llvm_inline_frames = NULL;
	cfg->n_llvm_inline_frames = 0;

	if (rows.empty ())
		return;

	std::vector<const mono::MonoIlInlineRow *> resolved;
	std::vector<MonoMethod *> methods;
	resolved.reserve (rows.size ());
	methods.reserve (rows.size ());

	for (const mono::MonoIlInlineRow &r : rows) {
		auto it = names.find (r.method);
		if (it == names.end ())
			continue;
		resolved.push_back (&r);
		methods.push_back (it->second);
	}

	if (resolved.empty ())
		return;

	MonoLLVMInlineFrame *map = (MonoLLVMInlineFrame *) mono_mem_manager_alloc (
		cfg->mem_manager, resolved.size () * sizeof (MonoLLVMInlineFrame));

	for (size_t i = 0; i < resolved.size (); ++i) {
		map [i].native_offset = resolved [i]->native_offset;
		map [i].il_offset = resolved [i]->il_offset;
		map [i].depth = resolved [i]->depth;
		map [i].method = methods [i];
	}

	cfg->llvm_inline_frames = map;
	cfg->n_llvm_inline_frames = (guint32) resolved.size ();
}

void
EmitContext::llvm_jit_finalize_method ()
{
	MonoCompile *cfg = this->cfg;
	MonoDomain *domain = mono_domain_get ();
	MonoJitDomainInfo *domain_info;
	int i;

	/*
	 * Compute the addresses of the LLVM globals pointing to the
	 * methods called by the current method. Pass it to the trampoline
	 * code so it can update them after their corresponding method was
	 * compiled.
	 */
	std::vector<llvm::GlobalVariable *> callee_vars;
	callee_vars.reserve (this->jit_callees.size ());
	for (const auto &kv : this->jit_callees)
		callee_vars.push_back (llvm::cast<llvm::GlobalVariable> (kv.second));
	std::vector<uint64_t> callee_addrs (callee_vars.size ());

	/*
	 * Run the -O2 module pipeline over the method's IR in place, before codegen.
	 * mono_llvm_compile_method () below clones this module and generates code
	 * from the clone, so it must see the optimized IR; optimizing in place also
	 * makes the "Optimized LLVM IR" dump further down truthful. The callee
	 * globals gathered just above (and the entry function itself) are all
	 * external-linkage, so the pipeline's GlobalOpt/GlobalDCE neither deletes,
	 * renames, nor constant-folds them - the by-name symbol resolution the
	 * compile path performs afterwards still finds every one.
	 */

	/*
	 * phi_values caches the phi nodes we pre-created during emission so a decline
	 * that happens mid-emission can re-home any still-floating ones before deleting
	 * the half-built function (see cleanup_failed_emit). By this point emission is
	 * complete and the optimizer below is about to rewrite the IR in place -
	 * mem2reg and friends will delete most of these phis outright. That leaves the
	 * cached pointers dangling, so a post-codegen decline (e.g. an unparseable
	 * .mono_lsda clause table) that reaches cleanup_failed_emit would feed freed
	 * values back into the IRBuilder. The cache has served its purpose; drop it now
	 * so cleanup on any later decline simply deletes the finished function.
	 */
	this->phi_values.clear ();

	/*
	 * Run the pipeline for THIS compile: the inliner needs the root's cfg (to
	 * drive callee materialization) and this compile's translator state (whose
	 * LLVMContext every callee body must be built in).
	 */
	mono::Tier1Root root { llvm::unwrap<llvm::Function> (this->lmethod), cfg, this->module };

	/*
	 * Seed the compile's clause table with the root's own clauses, at the ids its
	 * IR already names them by (a root emits with clause_id_base 0). Inlining
	 * appends to this, so by publish time it describes every clause in the
	 * finished function whatever method each one came from.
	 */
	g_assert (this->module->clauses.empty ());
	if (cfg->header)
		this->module->clauses.assign (cfg->header->clauses,
		                              cfg->header->clauses + cfg->header->num_clauses);

	mono::MonoLLVMJIT::get_singleton ()->optimize (root);

	mono_codeman_enable_write ();
	/*
	 * cfg->domain is the lifetime key for the code about to be emitted: it is
	 * the domain the body is published into, and mono_domain_free () reclaims
	 * everything compiled under it.
	 *
	 * The context goes along by shared_ptr: ORC keeps the module alive inside
	 * its dylib and must be able to keep the context with it. Handing over this
	 * compile's own context - rather than one the engine shares between
	 * compiles - is what lets several of these run at once.
	 */
	mono::CompileResult res = mono::MonoLLVMJIT::get_singleton ()->compile (
		llvm::unwrap<llvm::Function> (this->lmethod), callee_vars, callee_addrs.data (),
		"mono_eh_frame", this->module->context, cfg->domain);

	cfg->native_code = (guint8 *) (gsize) res.entry;
	/* Stock LLVM 18 emits a standard DWARF `.eh_frame` (consumed below by the
	 * unwind-ops transcoder), not a mono clause global, so res.mono_eh_frame is
	 * always 0 and not read here. */
	guint32 llvm_code_size = (guint32) res.code_size;
	gpointer dwarf_eh_frame = res.eh_frame.addr;
	guint32 dwarf_eh_frame_size = (guint32) res.eh_frame.size;
	gpointer stackmaps = res.stackmaps.addr;
	guint32 stackmaps_size = (guint32) res.stackmaps.size;
	/* C3 captures the `.mono_lsda` section (mono's own target-neutral clause
	 * table); the reader below (C6) parses/publishes it into cfg->llvm_ex_info for
	 * every admitted clause-bearing method. */
	gpointer mono_lsda = res.mono_lsda.addr;
	guint32 mono_lsda_size = (guint32) res.mono_lsda.size;
	mono_llvm_remove_gc_safepoint_poll (this->lmodule);
	mono_codeman_disable_write ();
	if (cfg->verbose_level > 1) {
		g_print ("\n*** Optimized LLVM IR for %s ***\n", mono_method_full_name (cfg->method, TRUE));
		mono_llvm_dump_module (this->lmodule);
		g_print ("***\n\n");
	}

	/*
	 * Three non-EH outputs the method's MonoJitInfo needs are recovered here,
	 * each from the stock object LLVM 18 emits:
	 *
	 * 1. cfg->code_len - RESTORED HERE. It sizes the method's MonoJitInfo, and a
	 *    zero-length jit-info makes mini_jit_info_table_find() unable to find the
	 *    method at all (g_assert (ji) in mini-runtime.c). The size comes from the
	 *    emitted object's ELF symbol table, returned by mono_llvm_compile_method().
	 *
	 * 2. cfg->encoded_unwind_ops - RESTORED HERE, by transcoding the stock DWARF
	 *    .eh_frame LLVM emits (see llvm/ehframe.cpp). Without it mini.c falls
	 *    back to cfg->unwind_ops, which is empty for an LLVM-compiled method, so
	 *    the jinfo would get an effectively empty unwind descriptor - and that
	 *    does NOT fail safe: a frame needs unwind info even with no EH clauses
	 *    of its own, because GC stack scanning walks through it and exceptions
	 *    thrown by callees propagate through it.
	 *
	 * 3. cfg->llvm_this_reg - recovered below from `.llvm_stackmaps` for gshared.
	 *
	 * The EH clause array itself (cfg->llvm_ex_info) is built below from the
	 * custom-emit `.mono_lsda` section.
	 */
	cfg->code_len = llvm_code_size;
	/*
	 * Fail loudly rather than silently building a zero-byte jit-info again: an
	 * emitted method always has a non-empty body.
	 */
	g_assert (cfg->code_len > 0);

	{
		GSList *unwind_ops = nullptr;

		if (!mono_llvm_eh_frame_to_unwind_ops (static_cast<guint8*>(dwarf_eh_frame), dwarf_eh_frame_size,
						       cfg->native_code, cfg->code_len, &unwind_ops)) {
			/*
			 * No usable FDE, or CFI we cannot represent. Publishing the method
			 * with no unwind information is the one outcome we must avoid, so
			 * bail out and let the classic JIT compile it instead.
			 */
			this->set_failure ("no usable DWARF unwind info");
			return;
		}

		cfg->encoded_unwind_ops = mono_unwind_ops_encode_full (unwind_ops, &cfg->encoded_unwind_ops_len, FALSE);
		mono_free_unwind_info (unwind_ops);

		if (cfg->verbose_level > 1) {
			g_print ("UNWIND INFO FOR %s:\n", mono_method_full_name (cfg->method, TRUE));
			mono_print_unwind_info (cfg->encoded_unwind_ops, cfg->encoded_unwind_ops_len);
			g_print ("\n");
		}
	}

	/*
	 * EH clauses: build cfg->llvm_ex_info from the custom-emit `.mono_lsda` the
	 * MonoLSDAStreamer wrote into this method's object (plan 12). The mono clause
	 * geometry is parsed, validated against cfg->header->clauses[] and the loaded
	 * code extent, then joined into the MonoJitExceptionInfo[] mini.c copies
	 * verbatim into jinfo->clauses (from_llvm = 1).
	 *
	 * On ANY uncertainty - an ABSENT `.mono_lsda` for a clause-bearing method
	 * (MonoEHGatherPass declined it, engine.cpp), bad magic or truncation, an
	 * offset past the code, a join key out of range, a non-catch clause
	 * slipping the gate - decline to the classic JIT (CAP-EH-0). The dispatcher
	 * cannot detect a wrong clause array (doc 11 11.4), so a plausible-but-wrong
	 * table must never be published.
	 *
	 * An EMPTY (but present) `.mono_lsda` is a separate, confirmed-safe case,
	 * not uncertainty: it means every protected call under this method's IL
	 * clause(s) optimized to a nounwind `call`, so nothing survived that could
	 * ever reach a handler. mono_lsda.cpp's build_ex_info () publishes it as
	 * zero clauses rather than declining.
	 */
	/*
	 * The clause table for the function that actually got emitted: the root's own
	 * clauses plus any an inlined callee brought with it.
	 */
	const std::vector<MonoExceptionClause> &clauses = this->module->clauses;

	if (!clauses.empty ()) {
		std::vector<mono::MonoLsdaEntry> entries;
		std::vector<mono::MonoFinallyGuard> guards;
		std::vector<FinallyMarker> markers;

		if (!mono::parse_mono_lsda (static_cast<const guint8*>(mono_lsda), mono_lsda_size, entries)) {
			this->set_failure ("could not parse .mono_lsda clause table");
			return;
		}

		/*
		 * The thread-abort guard for this method's finallys, assembled from the
		 * two halves that describe a handler body: MonoFinallyRangePass
		 * (engine.cpp) put its PC ranges in the section, and the marker stackmap
		 * named the frame slot to flag an abort through. Empty for a method with
		 * no finally.
		 */
		if (!recover_finally_markers (clauses, static_cast<guint8*>(stackmaps), stackmaps_size, markers)) {
			this->set_failure ("finally exvar slot is homed against a register a stack walk cannot rebuild");
			return;
		}

		/*
		 * Join each body run to the marker sitting inside it, rather than to its
		 * clause. A clause can occupy the frame several times over - the optimizer
		 * duplicates a body along its entry paths, and inlining one body at two
		 * call sites brings two copies - and inlined copies do NOT share a slot,
		 * since the alloca is cloned along with the code. The run is what the
		 * guard is really about, so the run is what names the slot.
		 */
		for (const mono::MonoLsdaEntry &e : entries) {
			if (e.kind != mono::MONO_LSDA_KIND_FINALLY_BODY)
				continue;

			const FinallyMarker *found = nullptr;

			for (const FinallyMarker &m : markers) {
				if (m.clause_index != e.clause_index)
					continue;
				if (m.pc < e.try_start_off || m.pc > e.try_start_off + e.try_len)
					continue;
				/*
				 * A run is bracketed by one opening marker, so a second one
				 * inside it means the two readings of the markers disagree
				 * about where the runs are - unless it names the same slot,
				 * which is just a marker the optimizer duplicated in place.
				 */
				g_assert (!found || same_exvar_slot (*found, m));
				found = &m;
			}

			if (!found) {
				/*
				 * A run with no opening marker of its own. The optimizer can
				 * split a body so a later piece is only ever entered from inside
				 * it - BranchFolding merging a tail, say - and that piece is
				 * still the clause's body and still needs guarding.
				 *
				 * Its slot is the clause's, which is unambiguous as long as the
				 * clause has only one. Several distinct slots means several
				 * copies of the body in this frame (inlining clones the alloca),
				 * and nothing here says which copy this run belongs to. Guessing
				 * would make the abort guard write into an unrelated frame byte,
				 * so decline the method and let the classic JIT have it.
				 */
				bool ambiguous = false;

				for (const FinallyMarker &m : markers) {
					if (m.clause_index != e.clause_index)
						continue;
					if (found && !same_exvar_slot (*found, m)) {
						ambiguous = true;
						break;
					}
					found = &m;
				}

				if (ambiguous) {
					this->set_failure ("could not attribute a finally body run to its exvar slot");
					return;
				}
			}

			/*
			 * A body only reaches the section because MonoFinallyRangePass found
			 * the markers bracketing it, so the two halves are present together
			 * or not at all.
			 */
			g_assert (found);

			/*
			 * An empty range is a body the optimizer emptied out between its two
			 * markers. There is no PC for a thread to be stopped at, so there is
			 * nothing to guard - but it still counts as covering its own marker
			 * in the check below.
			 */
			if (e.try_len == 0)
				continue;

			mono::MonoFinallyGuard g;
			g.clause_index = e.clause_index;
			g.handler_start_off = e.try_start_off;
			g.handler_end_off = e.try_start_off + e.try_len;
			g.exvar_offset = found->offset;
			g.exvar_base_reg = static_cast<std::uint8_t> (found->base_reg);

			guards.push_back (g);
		}

		/*
		 * Cross-check the two halves against each other. Every opening marker sits
		 * at the start of a body run, so it must fall inside one of the ranges
		 * recorded for its clause; one that does not means a run was missed and
		 * some of the body is unguarded.
		 */
		for (const FinallyMarker &m : markers) {
			bool covered = false;

			for (const mono::MonoLsdaEntry &e : entries) {
				if (e.kind != mono::MONO_LSDA_KIND_FINALLY_BODY ||
				    e.clause_index != m.clause_index)
					continue;
				if (m.pc >= e.try_start_off && m.pc <= e.try_start_off + e.try_len)
					covered = true;
			}

			g_assert (covered);
		}

		if (!mono::publish_mono_lsda (cfg, clauses, entries, cfg->native_code, cfg->code_len, guards)) {
			this->set_failure ("could not publish .mono_lsda clause table");
			return;
		}
	}

	/*
	 * gshared: recover cfg->llvm_this_reg/offset from the `.llvm_stackmaps` section
	 * (the translator planted a stackmap over the this/mrgctx home slot). mini.c's
	 * generic-jit-info setup asserts this is set for every gshared LLVM method
	 * (mini.c:2574). If recovery fails, decline to the classic JIT rather than
	 * publish a wrong this-slot (CAP-EH-0).
	 */
	if (cfg->gshared) {
		if (!recover_gshared_this_slot (cfg, static_cast<guint8*>(stackmaps), stackmaps_size)) {
			this->set_failure ("gshared this-slot not recoverable from stackmap");
			return;
		}
	}

	recover_il_seq_points (cfg, res.il_lines);
	recover_il_inline_frames (cfg, res.il_inline_frames, this->module->il_debug_methods);

	mono_domain_lock (domain);
	domain_info = domain_jit_info (domain);
	if (!domain_info->llvm_jit_callees)
		domain_info->llvm_jit_callees = g_hash_table_new (NULL, NULL);
	i = 0;
	for (const auto &kv : this->jit_callees) {
		MonoMethod *callee = kv.first;
		GSList *addrs = static_cast<GSList*>(g_hash_table_lookup (domain_info->llvm_jit_callees, callee));
		addrs = g_slist_prepend (addrs, (gpointer) (gsize) callee_addrs [i]);
		g_hash_table_insert (domain_info->llvm_jit_callees, callee, addrs);
		i ++;
	}
	mono_domain_unlock (domain);

	/*
	 * Report the body to perf from here, past every decline above, rather than
	 * from the publish site: this is the last point at which the .eh_frame
	 * behind it is still in hand, and a promotion never reaches mini.c's own
	 * mono_emit_jit_dump () call anyway.
	 */
	mono_llvm_jitdump_emit_method (cfg->method, cfg->native_code, cfg->code_len,
	                               static_cast<const guint8*>(dwarf_eh_frame), dwarf_eh_frame_size,
	                               cfg->llvm_seq_points, cfg->n_llvm_seq_points);
}

#else

void
EmitContext::llvm_jit_finalize_method ()
{
	g_assert_not_reached ();
}

#endif

static std::atomic<MonoCPUFeatures> cpu_features;

MonoCPUFeatures mono_llvm_get_cpu_features (void)
{
	static const CpuFeatureAliasFlag flags_map [] = {
#if defined(TARGET_X86) || defined(TARGET_AMD64)
		{ "sse",	MONO_CPU_X86_SSE },
		{ "sse2",	MONO_CPU_X86_SSE2 },
		{ "pclmul",	MONO_CPU_X86_PCLMUL },
		{ "aes",	MONO_CPU_X86_AES },
		{ "sse2",	MONO_CPU_X86_SSE2 },
		{ "sse3",	MONO_CPU_X86_SSE3 },
		{ "ssse3",	MONO_CPU_X86_SSSE3 },
		{ "sse4.1",	MONO_CPU_X86_SSE41 },
		{ "sse4.2",	MONO_CPU_X86_SSE42 },
		{ "popcnt",	MONO_CPU_X86_POPCNT },
		{ "avx",	MONO_CPU_X86_AVX },
		{ "avx2",	MONO_CPU_X86_AVX2 },
		{ "fma",	MONO_CPU_X86_FMA },
		{ "lzcnt",	MONO_CPU_X86_LZCNT },
		{ "bmi",	MONO_CPU_X86_BMI1 },
		{ "bmi2",	MONO_CPU_X86_BMI2 },
#endif
#if defined(TARGET_ARM64)
		{ "crc",	MONO_CPU_ARM64_CRC },
		{ "crypto",	MONO_CPU_ARM64_CRYPTO },
		{ "neon",	MONO_CPU_ARM64_ADVSIMD }
#endif
	};
	MonoCPUFeatures features = cpu_features.load (std::memory_order_relaxed);
	if (!features) {
		/*
		 * Racing callers both probe the CPU and store; the answer does not
		 * depend on who is asking, so they store the same bits.
		 */
		features = MONO_CPU_INITED | static_cast<MonoCPUFeatures>(mono_llvm_check_cpu_features (flags_map, G_N_ELEMENTS (flags_map)));
		cpu_features.store (features, std::memory_order_relaxed);
	}

	return features;
}

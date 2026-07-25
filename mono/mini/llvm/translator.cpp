/**
 * \file
 * llvm "Backend" for the mono JIT
 *
 * Copyright 2009-2011 Novell Inc (http://www.novell.com)
 * Copyright 2011 Xamarin Inc (http://www.xamarin.com)
 * Licensed under the MIT license. See LICENSE file in the project root for full license information.
 */

#include "translator-internal.hpp"

#include <vector>

#include <llvm/IR/Module.h>

#include <mono/metadata/metadata-internals.h>
#include <mono/metadata/mono-endian.h>
/* mono-basic-block.h has no extern "C" guard of its own. */
extern "C" {
#include <mono/metadata/mono-basic-block.h>
}

#include "mono_lsda.hpp"
#include "inliner-support.hpp"

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


LLVMIntPredicate cond_to_llvm_cond [] = {
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

LLVMRealPredicate fpcond_to_llvm_cond [] = {
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

MonoLLVMModule aot_module;

GHashTable *intrins_id_to_intrins;
LLVMTypeRef sse_i1_t, sse_i2_t, sse_i4_t, sse_i8_t, sse_r4_t, sse_r8_t;

static void init_jit_module (MonoDomain *domain);

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
static bool llvm_method_filter_inited;

static bool
llvm_method_filter_excludes (MonoMethod *method)
{
	int i;

	/*
	 * Lazy init, unsynchronized, exactly as MONO_VERBOSE_METHOD does it in
	 * mini.c. Compilation is single-threaded today; a race would at worst
	 * parse the variable twice and leak one strv.
	 */
	if (!llvm_method_filter_inited) {
		char *env = g_getenv ("MONO_LLVM_METHOD");
		if (env != nullptr)
			llvm_method_filter_names = g_strsplit (env, ";", -1);
		llvm_method_filter_inited = true;
	}

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
 * clause_encloses:
 *
 *   Does IL clause J strictly ENCLOSE clause C - i.e. is C's try region nested in
 * J's? This is byte-for-byte the containment predicate the .mono_lsda synthesis
 * (mono_lsda.cpp, EH N1) and the landing-pad emission (translator-call.cpp, EH N2)
 * use, with SIBLINGS (identical try_offset AND try_len - try { } catch(A) catch(B))
 * excluded. Keeping the nesting gate's admit set in lock-step with those two stages
 * is load-bearing: the gate must admit exactly the pairs the synthesis + switch can
 * represent, or a live nested method would consume an array that mis-dispatches.
 */
static inline bool
clause_encloses (const MonoExceptionClause *c, const MonoExceptionClause *j)
{
	bool siblings = c->try_offset == j->try_offset && c->try_len == j->try_len;
	return !siblings &&
	       c->try_offset >= j->try_offset &&
	       c->handler_offset <= j->handler_offset;
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
	 * Nesting gate (arbitrary depth, EH N6). The `.mono_lsda` synthesis
	 * (build_ex_info, EH N1) plus the landing-pad selector routing
	 * (emit_handler_start, EH N2) represent a clause whose try region is strictly
	 * contained in an enclosing clause's try: for each faulting PC the runtime
	 * still delivers to the INNERMOST landing pad, whose selector switch
	 * re-dispatches RDX == the enclosing clause_index to that enclosing handler's
	 * body (doc 21 5/6). The synthesised enclosing entries reuse the inner landing
	 * pad and copy the inner base entry's exact range, so the published native
	 * ranges stay equal-or-disjoint (doc 21 2.2). All try-containment of
	 * NONE/FINALLY/FAULT clauses is admitted to ANY depth: a clause with several
	 * enclosers (a 3-or-more-deep nest) gets one synthesised enclosing entry per
	 * encloser, appended in ascending clause_index (= innermost-encloser first,
	 * doc 21 4.1) so pass-2 runs the intervening finallys innermost-first and
	 * reaches enclosing catches in precedence order - matching the classic JIT's
	 * inner-first clause array. The N1 synthesis + N2 emission carry no depth cap
	 * (the selector switch adds one value-keyed case per encloser regardless of
	 * count), so nothing here limits depth either.
	 *
	 * The only residual decline (CAP-EH-0, doc 21 7 / 8.1) is defensive: a pair
	 * that overlaps but is neither siblings nor cleanly contained (neither
	 * encloses the other) - a CROSSING / partial overlap the synthesis cannot
	 * faithfully encode (malformed IL). Gate-A(FILTER)/save_lmf/dynamic declines
	 * still apply on top of this.
	 *
	 * True SIBLING catches - try { } catch(A) catch(B) - share the IDENTICAL
	 * protected region (same try_offset AND try_len) and are NOT nesting: they are
	 * emitted as one landing pad carrying a clause per sibling, routed by the
	 * selector (doc 11 3/6). clause_encloses is false for siblings, so they pass
	 * the crossing test - admitted, as before. The crossing test uses
	 * clause_encloses, byte-for-byte the predicate the N1 synthesis and N2
	 * emission use, so the gate admits exactly the pairs those two stages can
	 * represent.
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
 * and point it at the domain's shared MonoLLVMModule. The caller still has to
 * set ctx->lmodule (the LLVM module the function is emitted into) before calling
 * emit_method_inner ().
 */
static EmitContext *
alloc_emit_context (MonoCompile *cfg)
{
	EmitContext *ctx = new EmitContext ();
	ctx->cfg = cfg;
	ctx->mempool = cfg->mempool;

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

	init_jit_module (cfg->domain);
	ctx->module = static_cast<MonoLLVMModule*>(domain_jit_info (cfg->domain)->llvm_module);
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
	LLVMBasicBlockRef phi_bb = LLVMAppendBasicBlock (ctx->lmethod, "PHI_BB");
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

void
mono_llvm_emit_method (MonoCompile *cfg)
{
	EmitContext *ctx;

	if (cfg->skip)
		return;

	/* The code below might acquire the loader lock, so use it for global locking */
	mono_loader_lock ();

	ctx = alloc_emit_context (cfg);

	ctx->lmodule = LLVMModuleCreateWithName (g_strdup_printf ("jit-module-%s", cfg->method->name));
	/* Reset this as it contains values from lmodule */
	memset (ctx->module->intrins_by_id, 0, sizeof (LLVMValueRef) * INTRINS_NUM);

	ctx->emit_method_inner ();

	if (!ctx->ok ())
		cleanup_failed_emit (ctx);

	free_ctx (ctx);

	mono_loader_unlock ();
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

#if 0
	{
		static int count = 0;
		count ++;

		char *llvm_count_str = g_getenv ("LLVM_COUNT");
		if (llvm_count_str) {
			int lcount = atoi (llvm_count_str);
			g_free (llvm_count_str);
			if (count == lcount) {
				printf ("LAST: %s\n", mono_method_full_name (cfg->method, TRUE));
				fflush (stdout);
			}
			if (count > lcount) {
				this->set_failure ("count");
				return;
			}
		}
	}
#endif

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

	if (cfg->rgctx_var)
		linfo->rgctx_arg = TRUE;

	this->method_type = method_type = this->sig_to_llvm_sig_full (sig, linfo);
	if (!this->ok ())
		return;

	method = LLVMAddFunction (lmodule, this->method_name, method_type);
	/* 64 bytes matches the x86-64 cache line; JITLink honors this as the section alignment when placing the compiled code. */
	LLVMSetAlignment (method, 64);
	this->lmethod = method;

	/*
	 * Mark the top-down inliner's root. Every method the translator emits
	 * standalone is a tier-1 root (v1 emits exactly one per module); a callee
	 * materialized into a caller's module (translate_only) is not - it is a
	 * candidate body, never a root.
	 */
	if (!this->translate_only)
		llvm::unwrap<llvm::Function> (method)->addFnAttr ("mono-tier1-root");

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
		WrapperInfo *info = mono_marshal_get_wrapper_info (cfg->method);

		switch (info->subtype) {
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
	}
	if (header->num_clauses || (cfg->method->iflags & METHOD_IMPL_ATTRIBUTE_NOINLINING) || cfg->no_inline)
		/* We can't handle inlined methods with clauses */
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
		this->bblocks [bb->block_num].call_handler_target_bb = LLVMAppendBasicBlock (this->lmethod, name);
	}

	for (MonoBasicBlock *bb : bblock_list) {
		// Prune unreachable mono BBs.
		if (!(bb == cfg->bb_entry || bb->in_count > 0))
			continue;

		process_bb (this, bb);
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

		md_args [0] = LLVMMDString (this->method_name, strlen (this->method_name));
		md_args [1] = llvm::wrap (llvm::ConstantInt::get (llvm::Type::getInt32Ty (this->llvm_ctx ()), 1, false));
		md_node = LLVMMDNode (md_args, 2);
		LLVMAddNamedMetadataOperand (lmodule, "mono.function_indexes", md_node);
	}

	//LLVMVerifyFunction (method, 0);

	if (this->translate_only)
		/*
		 * Materialize-into-caller path: the callee body is done; the tier-1
		 * inliner owns it from here (inlines or strips it). Skip finalize
		 * (optimize + JIT the module) and the method<->lmethod bookkeeping.
		 */
		return;

	/*
	 * Register the root so the top-down inliner (which runs inside the finalize
	 * below, during the module optimization pipeline) can reach this method's
	 * MonoCompile to drive callee materialization. Unregister once optimization
	 * has returned - the cfg does not outlive this compile.
	 */
	mono::register_tier1_root (llvm::unwrap<llvm::Function> (this->lmethod), cfg);

	this->llvm_jit_finalize_method ();

	mono::unregister_tier1_root (llvm::unwrap<llvm::Function> (this->lmethod));

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
	sse_i1_t = type_to_sse_type (MONO_TYPE_I1);
	sse_i2_t = type_to_sse_type (MONO_TYPE_I2);
	sse_i4_t = type_to_sse_type (MONO_TYPE_I4);
	sse_i8_t = type_to_sse_type (MONO_TYPE_I8);
	sse_r4_t = type_to_sse_type (MONO_TYPE_R4);
	sse_r8_t = type_to_sse_type (MONO_TYPE_R8);

	intrins_id_to_intrins = g_hash_table_new (NULL, NULL);

	if (enable_jit)
		mono_llvm_jit_init ();
}

void
mono_llvm_cleanup (void)
{
	MonoLLVMModule *module = &aot_module;

	if (module->lmodule)
		LLVMDisposeModule (module->lmodule);

	if (module->context)
		LLVMContextDispose (module->context);
}

void
mono_llvm_free_domain_info (MonoDomain *domain)
{
	MonoJitDomainInfo *info = domain_jit_info (domain);
	MonoLLVMModule *module = static_cast<MonoLLVMModule*>(info->llvm_module);
	int i;

	if (!module)
		return;

	g_hash_table_destroy (module->llvm_types);

	mono_llvm_dispose_ee (module->mono_ee);

	if (module->bb_names) {
		for (i = 0; i < module->bb_names_len; ++i)
			g_free (module->bb_names [i]);
		g_free (module->bb_names);
	}
	//LLVMDisposeModule (module->module);

	g_free (module);

	info->llvm_module = NULL;
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
 * Tier-1 inliner support: root registry + lazy callee materialization.
 *
 * These are the mono-aware half of the top-down inliner. The pure-LLVM pass
 * (inliner.cpp) reaches them through inliner-support.hpp with everything mono
 * passed as an opaque void *, because that TU has no mono headers.
 * ------------------------------------------------------------------------ */

namespace mono {

/*
 * The root registry, keyed by the root's LLVM Function. Only ever holds the
 * root(s) currently being optimized (v1: exactly one at a time), since a root is
 * unregistered as soon as its optimization returns. A plain map under the loader
 * lock (held across the whole emit) is enough - no separate lock needed.
 */
static std::unordered_map<llvm::Function *, MonoCompile *> *tier1_roots;

void
register_tier1_root (llvm::Function *root, void *root_cfg)
{
	if (!tier1_roots)
		tier1_roots = new std::unordered_map<llvm::Function *, MonoCompile *> ();
	(*tier1_roots) [root] = static_cast<MonoCompile *> (root_cfg);
}

void
unregister_tier1_root (llvm::Function *root)
{
	if (tier1_roots)
		tier1_roots->erase (root);
}

void *
tier1_root_cfg (llvm::Function *root)
{
	if (!tier1_roots)
		return nullptr;
	auto found = tier1_roots->find (root);
	return found == tier1_roots->end () ? nullptr : found->second;
}

bool
tier1_root_allows_inlining (void *root_cfg)
{
	MonoCompile *cfg = static_cast<MonoCompile *> (root_cfg);
	if (!cfg)
		return false;
	/* #1/#19: NOOPTIMIZATION/debug method, or -O=inline cleared. */
	if (cfg->disable_inline)
		return false;
	if (!(cfg->opt & MONO_OPT_INLINE))
		return false;
	/*
	 * #2/#26: shared generic code. gsharedvt layout is frame-local and
	 * unrecoverable once folded; a plain gshared root can reach an open,
	 * type-parameter-bearing callee whose front-end aborts (method-to-ir.c's
	 * !sig->has_type_parameters assert) the moment we try to materialize it -
	 * see materialize_callee (). Refuse both outright at this slice: we do not
	 * inline anything that touches generic sharing.
	 */
	if (cfg->gshared || cfg->gsharedvt)
		return false;
	return true;
}

void *
managed_method_from_symbol (const char *sym)
{
	return mono_llvm_method_from_symbol (sym);
}

/*
 * True if METHOD still needs a generic context to be compiled - it is open
 * (a generic type/method definition) or shared over type parameters. Such a
 * callee cannot be folded into the root: mini_method_compile () asserts
 * !sig->has_type_parameters for a non-gshared compile, so running its front-end
 * would abort inside the compile. This is checked up front, before that compile,
 * because the context can arrive via `this`/the receiver type rather than an
 * explicit rgctx argument, which the pass's call-site nest check does not see.
 */
static bool
callee_needs_generic_context (MonoMethod *method)
{
	/* Open declaring type: the generic type definition itself (Box`1<T>). */
	if (mono_class_is_gtd (method->klass))
		return true;
	/* Declaring type instantiated over type parameters (the shared Box`1<T_REF>). */
	if (mono_class_is_open_constructed_type (m_class_get_byval_arg (method->klass)))
		return true;
	/* A generic method definition (uninstantiated). */
	if (method->is_generic)
		return true;
	MonoMethodSignature *sig = mono_method_signature_internal (method);
	if (!sig)
		return true;                 /* cannot tell -> be conservative */
	/* The exact condition method-to-ir.c asserts against for a non-gshared compile. */
	if (sig->has_type_parameters)
		return true;
	return false;
}

/*
 * True if METHOD's body reads or writes a static field of a class that still
 * needs its cctor to run (relative to METHOD).
 *
 * A static-field access is normally protected by a class-init barrier, but the
 * front-end elides that barrier inside the accessor itself in the common cases
 * (a beforefieldinit type, an accessor defined in the field's own class, an
 * already-initialized vtable) on the assumption the accessor is only ever
 * reached through its own managed call - which itself carries the guarantee the
 * cctor ran. Folding such an accessor into a caller removes that call, so the
 * static read happens with no guarantee the cctor has run yet, yielding a stale
 * value: a silent miscompile.
 *
 * We detect this from the IL/metadata rather than the materialized IR precisely
 * because the elided case leaves NO class-init call in the body for the leaf
 * gate to catch. This is deliberately conservative: any touch of a static field
 * whose declaring class has a non-trivial cctor disqualifies the callee, even
 * where the barrier would in fact have survived.
 */
bool
callee_reads_cctor_guarded_static (void *target)
{
	MonoMethod *method = static_cast<MonoMethod *> (target);
	if (!method)
		return true;

	/*
	 * Only non-generic callees reach the inliner's cctor gate (generic-context
	 * callees are refused earlier). For those the method's generic context is
	 * empty, so field tokens resolve with a NULL context; leave the
	 * generic-context refusal to materialize_callee ().
	 */
	if (callee_needs_generic_context (method))
		return false;

	ERROR_DECL (error);
	MonoMethodHeader *header = mono_method_get_header_internal (method, error);
	if (!header || !is_ok (error)) {
		mono_error_cleanup (error);
		return true;                 /* cannot inspect the body -> be conservative */
	}

	MonoImage *image = m_class_get_image (method->klass);
	const unsigned char *ip = header->code;
	const unsigned char *end = ip + header->code_size;

	bool guarded = false;
	while (ip < end) {
		MonoOpcodeEnum il_op = MonoOpcodeEnum_Invalid;
		const unsigned char *tmp = ip;
		const int op_size = mono_opcode_value_and_size (&tmp, end, &il_op);
		if (op_size <= 0)
			break;                   /* malformed IL -> stop scanning */
		const unsigned char *next_ip = ip + op_size;

		switch (il_op) {
		case MONO_CEE_LDFLD:
		case MONO_CEE_LDFLDA:
		case MONO_CEE_STFLD:
		case MONO_CEE_LDSFLD:
		case MONO_CEE_LDSFLDA:
		case MONO_CEE_STSFLD: {
			/* InlineField: a 4-byte token sits just before the next instruction. */
			guint32 token = read32 (next_ip - 4);
			MonoClass *fklass = NULL;
			ERROR_DECL (ferror);
			MonoClassField *field =
				mono_field_from_token_checked (image, token, &fklass, NULL, ferror);
			if (!field || !is_ok (ferror)) {
				mono_error_cleanup (ferror);
				guarded = true;      /* unresolved field access -> conservative */
				break;
			}
			/*
			 * LDFLD/STFLD on a static field is legal IL and the front-end treats
			 * it as static, so gate on the field's actual staticness, not the
			 * opcode. Use the resolved access class (fklass) for the cctor test,
			 * exactly as the field-access path in method-to-ir.c does.
			 */
			MonoType *ftype = mono_field_get_type_internal (field);
			if (ftype && (ftype->attrs & FIELD_ATTRIBUTE_STATIC) &&
			    mono_class_needs_cctor_run (fklass ? fklass : field->parent, method))
				guarded = true;
			break;
		}
		default:
			break;
		}

		if (guarded)
			break;
		ip = next_ip;
	}

	mono_metadata_free_mh (header);
	return guarded;
}

llvm::Function *
materialize_callee (void *target, void *root_cfg, llvm::Module *into)
{
	MonoMethod *method = static_cast<MonoMethod *> (target);
	MonoCompile *root = static_cast<MonoCompile *> (root_cfg);

	if (!method || !root)
		return nullptr;

	/*
	 * Conservative mono-side refusals, on top of whatever the front-end declines
	 * and the pass's own LLVM-level gates:
	 *  - wrappers have no directly-inlinable managed body (the real work lives in
	 *    the wrapped method), and inlining e.g. a synchronized wrapper's raw body
	 *    would drop the monitor enter/exit;
	 *  - synchronized methods, same reason;
	 *  - anything that would come back gshared/gsharedvt is the rgctx
	 *    generic-context gate (#26/#2): the call-site nest check in the pass is the
	 *    primary guard, this is belt-and-suspenders in case a gshared callee is
	 *    ever reached without a nest arg.
	 */
	if (method->wrapper_type != MONO_WRAPPER_NONE)
		return nullptr;
	if (method->iflags & METHOD_IMPL_ATTRIBUTE_SYNCHRONIZED)
		return nullptr;
	/*
	 * #26/#2: refuse an open/shared-generic callee BEFORE its front-end runs.
	 * A gshared root can reach a type-parameter-bearing callee with no nest arg
	 * (the context rides in via `this`/the receiver type), and mini_method_compile
	 * would then abort on !sig->has_type_parameters. The cfg->gshared check below
	 * only catches this after the compile returns - too late; this is the gate.
	 */
	if (callee_needs_generic_context (method))
		return nullptr;

	/*
	 * Run the callee front-end into LLVM-ready MonoIR (JIT_FLAG_LLVM_IR_ONLY
	 * stops mini_method_compile before it emits). No cctors on this thread.
	 */
	MonoCompile *cfg = mini_method_compile (
		method, root->opt, root->domain,
		(JitFlags) (JIT_FLAG_LLVM | JIT_FLAG_LLVM_IR_ONLY | JIT_FLAG_NO_LLVM_FALLBACK),
		0, -1);
	if (!cfg)
		return nullptr;

	llvm::Function *result = nullptr;

	/* The front-end may have declined LLVM or produced a shared/gshared body. */
	if (cfg->disable_llvm || cfg->exception_type != MONO_EXCEPTION_NONE)
		goto done;
	if (cfg->gshared || cfg->gsharedvt)
		goto done;

	{
		EmitContext *ctx = alloc_emit_context (cfg);
		/*
		 * Translate into the caller's module. The intrinsic cache on the shared
		 * MonoLLVMModule is already keyed to this lmodule (the root filled it),
		 * so - unlike the standalone emit path - it must NOT be reset here.
		 */
		ctx->lmodule = llvm::wrap (into);
		ctx->translate_only = true;

		ctx->emit_method_inner ();

		if (!ctx->ok () || !ctx->lmethod) {
			cleanup_failed_emit (ctx);
		} else {
			result = llvm::unwrap<llvm::Function> (ctx->lmethod);
			/*
			 * The body is only ever referenced from within this module (inlined
			 * or stripped), so internal linkage - and it must be DCE-able if the
			 * inliner declines it.
			 */
			result->setLinkage (llvm::GlobalValue::InternalLinkage);
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

static void
init_jit_module (MonoDomain *domain)
{
	MonoJitDomainInfo *dinfo;
	MonoLLVMModule *module;

	dinfo = domain_jit_info (domain);
	if (dinfo->llvm_module)
		return;

	mono_loader_lock ();

	if (dinfo->llvm_module) {
		mono_loader_unlock ();
		return;
	}

	module = g_new0 (MonoLLVMModule, 1);

	module->context = LLVMGetGlobalContext ();
	module->intrins_by_id = g_new0 (LLVMValueRef, INTRINS_NUM);

	module->mono_ee = static_cast<MonoEERef*>(mono_llvm_create_ee (&module->ee));

	// This contains just the intrinsics
	module->lmodule = LLVMModuleCreateWithName ("jit-global-module");
	add_intrinsics (module->lmodule);
	add_types (module);

	module->llvm_types = g_hash_table_new (NULL, NULL);

	mono_memory_barrier ();

	dinfo->llvm_module = module;

	mono_loader_unlock ();
}

/* Unaligned little-endian reads for the stackmap parser below. */
static inline guint16
read_le16 (const guint8 *p)
{
	return static_cast<guint16>(p [0] | (p [1] << 8));
}

static inline guint32
read_le32 (const guint8 *p)
{
	return static_cast<guint32>(p [0]) | (static_cast<guint32>(p [1]) << 8) | (static_cast<guint32>(p [2]) << 16) | (static_cast<guint32>(p [3]) << 24);
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
 * that holds this/mrgctx (emit_this_slot_stackmap, translator-call.cpp), so the
 * first record's first location is that slot. LLVM lowers an alloca operand to a
 * Direct location {DwarfReg, Offset} whose value is the slot's ADDRESS =
 * reg+offset; the consumer dereferences it, reading the stored this/mrgctx.
 * this_in_reg is forced to 0 by the LLVM branch in mini.c, matching Direct.
 *
 * Returns FALSE (declining the method to the classic JIT, per CAP-EH-0: a
 * plausible-but-wrong this-slot is worse than a fallback) if the section is
 * absent, malformed, or the location is anything other than Direct - in which
 * case *(reg+offset) would not equal this and stack walks would read garbage.
 *
 * Stackmap format is version 3 (LLVM's StackMapParser layout), little-endian.
 */
static bool
recover_gshared_this_slot (MonoCompile *cfg, guint8 *stackmaps, guint32 size)
{
	/* StackMap location kinds (llvm/CodeGen/StackMaps.h). */
	enum { LOC_REGISTER = 1, LOC_DIRECT = 2, LOC_INDIRECT = 3, LOC_CONSTANT = 4, LOC_CONST_INDEX = 5 };

	if (!stackmaps || size < 16)
		return false;

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

	guint8 version = stackmaps [0];
	if (version != 3)
		return false;

	guint32 num_functions = read_le32 (stackmaps + 4);
	guint32 num_constants = read_le32 (stackmaps + 8);
	guint32 num_records = read_le32 (stackmaps + 12);
	if (num_records == 0)
		return false;

	/* Header (16) + StkSizeRecord[num_functions] (24 each) + Constants (8 each). */
	guint64 rec_off = static_cast<guint64>(16) + static_cast<guint64>(num_functions) * 24 + static_cast<guint64>(num_constants) * 8;
	/* First record: u64 id, u32 instr_offset, u16 pad, u16 num_locations, then locations. */
	if (rec_off + 16 > size)
		return false;
	guint8 *rec = stackmaps + rec_off;

	guint16 num_locations = read_le16 (rec + 14);
	if (num_locations == 0)
		return false;

	/* Location[0]: u8 kind, u8 reserved, u16 size, u16 dwarf_reg, u16 reserved, i32 offset. */
	if (rec_off + 16 + 12 > size)
		return false;
	guint8 *loc = rec + 16;
	guint8 kind = loc [0];
	guint16 dwarf_reg = read_le16 (loc + 4);
	gint32 offset = static_cast<gint32>(read_le32 (loc + 8));

	if (kind != LOC_DIRECT) {
		TRACE_FAILURE_CFG (cfg, "gshared this-slot stackmap not Direct");
		return false;
	}

	/*
	 * mono_dwarf_reg_to_hw_reg () indexes map_dwarf_reg_to_hw_reg [NUM_DWARF_REGS]
	 * with no bounds check. dwarf_reg is only ever RBP/RSP for Direct locations, so
	 * this never fires in practice, but guard it like every other read above rather
	 * than index out of bounds on a malformed stackmap.
	 */
	if (!mono_dwarf_reg_is_valid (dwarf_reg)) {
		TRACE_FAILURE_CFG (cfg, "gshared this-slot stackmap dwarf reg out of range");
		return false;
	}

	cfg->llvm_this_reg = mono_dwarf_reg_to_hw_reg (dwarf_reg);
	cfg->llvm_this_offset = offset;
	return true;
}

void
EmitContext::llvm_jit_finalize_method ()
{
	MonoCompile *cfg = this->cfg;
	MonoDomain *domain = mono_domain_get ();
	MonoJitDomainInfo *domain_info;
	int nvars = this->jit_callees.size ();
	LLVMValueRef *callee_vars = g_new0 (LLVMValueRef, nvars);
	gpointer *callee_addrs = g_new0 (gpointer, nvars);
	gpointer eh_frame;
	int i;

	/*
	 * Compute the addresses of the LLVM globals pointing to the
	 * methods called by the current method. Pass it to the trampoline
	 * code so it can update them after their corresponding method was
	 * compiled.
	 */
	i = 0;
	for (const auto &kv : this->jit_callees)
		callee_vars [i ++] = llvm::wrap (kv.second);

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

	mono_llvm_optimize_method (this->lmethod);

	mono_codeman_enable_write ();
	guint32 llvm_code_size = 0;
	gpointer dwarf_eh_frame = nullptr;
	guint32 dwarf_eh_frame_size = 0;
	gpointer stackmaps = nullptr;
	guint32 stackmaps_size = 0;
	/* C3 captures the `.mono_lsda` section (mono's own target-neutral clause
	 * table); the reader below (C6) parses/publishes it into cfg->llvm_ex_info for
	 * every admitted clause-bearing method. */
	gpointer mono_lsda = nullptr;
	guint32 mono_lsda_size = 0;
	cfg->native_code = static_cast<guint8*>(mono_llvm_compile_method (this->module->mono_ee, cfg, this->lmethod, nvars, callee_vars, callee_addrs, &eh_frame, &llvm_code_size, &dwarf_eh_frame, &dwarf_eh_frame_size, &stackmaps, &stackmaps_size, &mono_lsda, &mono_lsda_size));
	/* Stock LLVM 18 emits a standard DWARF `.eh_frame` (consumed below by the
	 * unwind-ops transcoder), not a mono clause global, so eh_frame is always NULL
	 * and not read here. */
	(void) eh_frame;
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
	 * On ANY uncertainty - an absent/empty `.mono_lsda` for a clause-bearing
	 * method (every protected call optimised to a nounwind `call`), bad magic or
	 * truncation, an offset past the code, a join key out of range, a non-catch
	 * clause slipping the gate - decline to the classic JIT (CAP-EH-0). The
	 * dispatcher cannot detect a wrong clause array (doc 11 11.4), so a
	 * plausible-but-wrong table must never be published.
	 */
	if (cfg->header->num_clauses > 0) {
		std::vector<mono::MonoLsdaEntry> entries;

		if (!mono::parse_mono_lsda (static_cast<const guint8*>(mono_lsda), mono_lsda_size, entries)) {
			this->set_failure ("could not parse .mono_lsda clause table");
			return;
		}
		if (!mono::publish_mono_lsda (cfg, entries, cfg->native_code, cfg->code_len)) {
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

	mono_domain_lock (domain);
	domain_info = domain_jit_info (domain);
	if (!domain_info->llvm_jit_callees)
		domain_info->llvm_jit_callees = g_hash_table_new (NULL, NULL);
	i = 0;
	for (const auto &kv : this->jit_callees) {
		MonoMethod *callee = kv.first;
		GSList *addrs = static_cast<GSList*>(g_hash_table_lookup (domain_info->llvm_jit_callees, callee));
		addrs = g_slist_prepend (addrs, callee_addrs [i]);
		g_hash_table_insert (domain_info->llvm_jit_callees, callee, addrs);
		i ++;
	}
	mono_domain_unlock (domain);
}

#else

static void
init_jit_module (MonoDomain *domain)
{
	g_assert_not_reached ();
}

void
EmitContext::llvm_jit_finalize_method ()
{
	g_assert_not_reached ();
}

#endif

static MonoCPUFeatures cpu_features;

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
	if (!cpu_features)
		cpu_features = MONO_CPU_INITED | static_cast<MonoCPUFeatures>(mono_llvm_check_cpu_features (flags_map, G_N_ELEMENTS (flags_map)));

	return cpu_features;
}

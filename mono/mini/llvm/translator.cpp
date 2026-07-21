/**
 * \file
 * llvm "Backend" for the mono JIT
 *
 * Copyright 2009-2011 Novell Inc (http://www.novell.com)
 * Copyright 2011 Xamarin Inc (http://www.xamarin.com)
 * Licensed under the MIT license. See LICENSE file in the project root for full license information.
 */

#include "translator-internal.hpp"

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

void emit_cond_system_exception (EmitContext *ctx, MonoBasicBlock *bb, const char *exc_type, LLVMValueRef cmp, gboolean force_explicit);
LLVMValueRef get_intrins (EmitContext *ctx, int id);
static void llvm_jit_finalize_method (EmitContext *ctx);
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
static gboolean llvm_method_filter_inited;

static gboolean
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
		if (env != NULL)
			llvm_method_filter_names = g_strsplit (env, ";", -1);
		llvm_method_filter_inited = TRUE;
	}

	if (!llvm_method_filter_names)
		return FALSE;

	for (i = 0; llvm_method_filter_names [i] != NULL; i++) {
		const char *name = llvm_method_filter_names [i];

		if ((strchr (name, '.') > name) || strchr (name, ':')) {
			MonoMethodDesc *desc = mono_method_desc_new (name, TRUE);
			if (desc) {
				gboolean match = mono_method_desc_full_match (desc, method);
				mono_method_desc_free (desc);
				if (match)
					return FALSE;
			}
		} else {
			if (strcmp (method->name, name) == 0)
				return FALSE;
		}
	}

	return TRUE;
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

	/* Diagnostic filter (MONO_LLVM_METHOD); a no-op unless the variable is set. */
	if (llvm_method_filter_excludes (cfg->method)) {
		cfg->exception_message = g_strdup ("not selected by MONO_LLVM_METHOD");
		cfg->disable_llvm = TRUE;
		return;
	}

	/*
	 * 3b EH gate: methods with exception-handling clauses are deferred to the
	 * classic JIT for now. The stock .eh_frame / .gcc_except_table consumption
	 * (design 3.1) is not yet ported, so LLVM-compiling a clause-bearing method
	 * would mis-handle unwind. Remove this gate once EH consumption lands.
	 */
	if (cfg->header->num_clauses) {
		cfg->exception_message = g_strdup ("EH clauses (deferred to classic JIT, 3b)");
		cfg->disable_llvm = TRUE;
		return;
	}

	/*
	 * Generic-shared methods are deferred to the classic JIT for now.
	 *
	 * mini.c's generic-jit-info setup does, for every gshared method compiled by
	 * LLVM:
	 *
	 *	g_assert (cfg->llvm_this_reg != -1);
	 *	gi->this_reg = cfg->llvm_this_reg;
	 *
	 * and the ONLY producer of cfg->llvm_this_reg is decode_llvm_eh_info(),
	 * reading it out of the forked LLVM's mono-format LSDA header. Stock LLVM
	 * emits no such header, so the field is never set.
	 *
	 * mini.c now initializes it to -1, so the miscompile that would otherwise
	 * follow (registering the method with this_reg = 0, a valid-looking but
	 * wrong register) is already ruled out - the assert fires instead. This gate
	 * therefore chooses between aborting on every generic-shared method and
	 * quietly falling back to the classic JIT, and the fallback is the useful
	 * behaviour while the slot has no source.
	 *
	 * Note gsharedvt is already excluded (mini.c sets disable_llvm for it), but
	 * plain gshared is not, so this gate is load-bearing rather than theoretical.
	 *
	 * Recovering the slot properly needs llvm.experimental.stackmap (design 3.1).
	 * That is worth doing: design 6 (S6) singles out this exclusion as the one to
	 * resist, since it removes List<T>/Dictionary<K,V>/LINQ-over-reference-types
	 * - a large share of real hot paths - from the LLVM tier.
	 */
	if (cfg->gshared) {
		cfg->exception_message = g_strdup ("gshared (needs the llvm_this_reg slot, 3c)");
		cfg->disable_llvm = TRUE;
		return;
	}

	if (cfg->method->save_lmf) {
		cfg->exception_message = g_strdup ("lmf");
		cfg->disable_llvm = TRUE;
	}
	if (cfg->disable_llvm)
		return;

	/*
	 * Nested clauses where one of the clauses is a finally clause is
	 * not supported, because LLVM can't figure out the control flow,
	 * probably because we resume exception handling by calling our
	 * own function instead of using the 'resume' llvm instruction.
	 */
	for (i = 0; i < cfg->header->num_clauses; ++i) {
		for (j = 0; j < cfg->header->num_clauses; ++j) {
			MonoExceptionClause *clause1 = &cfg->header->clauses [i];
			MonoExceptionClause *clause2 = &cfg->header->clauses [j];

			// FIXME: Nested try clauses fail in some cases too, i.e. #37273
			if (i != j && clause1->try_offset >= clause2->try_offset && clause1->handler_offset <= clause2->handler_offset) {
				//(clause1->flags == MONO_EXCEPTION_CLAUSE_FINALLY || clause2->flags == MONO_EXCEPTION_CLAUSE_FINALLY)) {
				cfg->exception_message = g_strdup ("nested clauses");
				cfg->disable_llvm = TRUE;
				break;
			}
		}
	}
	if (cfg->disable_llvm)
		return;

	/* FIXME: */
	if (cfg->method->dynamic) {
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
emit_method_inner (EmitContext *ctx);

static void
free_ctx (EmitContext *ctx)
{
	GSList *l;

	g_free (ctx->values);
	g_free (ctx->addresses);
	g_free (ctx->vreg_types);
	g_free (ctx->is_vphi);
	g_free (ctx->vreg_cli_types);
	g_free (ctx->is_dead);
	g_free (ctx->unreachable);
	g_ptr_array_free (ctx->phi_values, TRUE);
	g_free (ctx->bblocks);
	g_hash_table_destroy (ctx->region_to_handler);
	g_hash_table_destroy (ctx->clause_to_handler);
	g_hash_table_destroy (ctx->jit_callees);

	g_free (ctx->method_name);
	g_ptr_array_free (ctx->bblock_list, TRUE);

	for (l = ctx->builders; l; l = l->next) {
		LLVMBuilderRef builder = (LLVMBuilderRef)l->data;
		LLVMDisposeBuilder (builder);
	}

	g_free (ctx);
}

/*
 * mono_llvm_emit_method:
 *
 *   Emit LLVM IL from the mono IL, and compile it to native code using LLVM.
 */
void
mono_llvm_emit_method (MonoCompile *cfg)
{
	EmitContext *ctx;
	char *method_name;
	int i;

	if (cfg->skip)
		return;

	/* The code below might acquire the loader lock, so use it for global locking */
	mono_loader_lock ();

	ctx = g_new0 (EmitContext, 1);
	ctx->cfg = cfg;
	ctx->mempool = cfg->mempool;

	/*
	 * This maps vregs to the LLVM instruction defining them
	 */
	ctx->values = g_new0 (LLVMValueRef, cfg->next_vreg);
	/*
	 * This maps vregs for volatile variables to the LLVM instruction defining their
	 * address.
	 */
	ctx->addresses = g_new0 (Address*, cfg->next_vreg);
	ctx->vreg_types = g_new0 (LLVMTypeRef, cfg->next_vreg);
	ctx->is_vphi = g_new0 (gboolean, cfg->next_vreg);
	ctx->vreg_cli_types = g_new0 (MonoType*, cfg->next_vreg);
	ctx->phi_values = g_ptr_array_sized_new (256);
	/* 
	 * This signals whenever the vreg was defined by a phi node with no input vars
	 * (i.e. all its input bblocks end with NOT_REACHABLE).
	 */
	ctx->is_dead = g_new0 (gboolean, cfg->next_vreg);
	/* Whenever the bblock is unreachable */
	ctx->unreachable = g_new0 (gboolean, cfg->max_block_num);
	ctx->bblock_list = g_ptr_array_sized_new (256);

	ctx->region_to_handler = g_hash_table_new (NULL, NULL);
	ctx->clause_to_handler = g_hash_table_new (NULL, NULL);
	ctx->jit_callees = g_hash_table_new (NULL, NULL);
	init_jit_module (cfg->domain);
	ctx->module = (MonoLLVMModule*)domain_jit_info (cfg->domain)->llvm_module;
	method_name = mono_method_full_name (cfg->method, TRUE);
	ctx->method_name = method_name;

	ctx->lmodule = LLVMModuleCreateWithName (g_strdup_printf ("jit-module-%s", cfg->method->name));
	/* Reset this as it contains values from lmodule */
	memset (ctx->module->intrins_by_id, 0, sizeof (LLVMValueRef) * INTRINS_NUM);

	emit_method_inner (ctx);

	if (!ctx_ok (ctx)) {
		if (ctx->lmethod) {
			/* Need to add unused phi nodes as they can be referenced by other values */
			LLVMBasicBlockRef phi_bb = LLVMAppendBasicBlock (ctx->lmethod, "PHI_BB");
			LLVMBuilderRef builder;

			builder = create_builder (ctx);
			LLVMPositionBuilderAtEnd (builder, phi_bb);

			for (i = 0; i < (int)ctx->phi_values->len; ++i) {
				LLVMValueRef v = (LLVMValueRef)g_ptr_array_index (ctx->phi_values, i);
				if (LLVMGetInstructionParent (v) == NULL)
					LLVMInsertIntoBuilder (builder, v);
			}

			LLVMDeleteFunction (ctx->lmethod);
		}
	}

	free_ctx (ctx);

	mono_loader_unlock ();
}

static void
emit_method_inner (EmitContext *ctx)
{
	MonoCompile *cfg = ctx->cfg;
	MonoMethodSignature *sig;
	MonoBasicBlock *bb;
	LLVMTypeRef method_type;
	LLVMValueRef method = NULL;
	LLVMValueRef *values = ctx->values;
	int i, max_block_num;
	/* Indexes cfg->bblocks (guint num_bblocks) and bblock_list (guint len) */
	guint bb_index;
	LLVMCallInfo *linfo;
	LLVMModuleRef lmodule = ctx->lmodule;
	BBInfo *bblocks;
	GPtrArray *bblock_list = ctx->bblock_list;
	MonoMethodHeader *header;
	MonoExceptionClause *clause;
	char **names;
	LLVMBuilderRef entry_builder = NULL;
	LLVMBasicBlockRef entry_bb = NULL;

	if (cfg->gsharedvt) {
		set_failure (ctx, "gsharedvt");
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
				set_failure (ctx, "count");
				return;
			}
		}
	}
#endif

	if (cfg->method->wrapper_type == MONO_WRAPPER_OTHER) {
		WrapperInfo *info = mono_marshal_get_wrapper_info (cfg->method);
		if (info->subtype == WRAPPER_SUBTYPE_LLVM_FUNC) {
			g_assert (info->d.llvm_func.subtype == LLVM_FUNC_WRAPPER_GC_POLL);

			method = emit_icall_cold_wrapper (ctx->module, lmodule, MONO_JIT_ICALL_mono_threads_state_poll, FALSE);
			ctx->lmethod = method;
			ctx->module->max_method_idx = MAX (ctx->module->max_method_idx, (int)cfg->method_index);

			ctx->method_name = g_strdup (LLVMGetValueName (method));
			ctx->cfg->asm_symbol = g_strdup (ctx->method_name);

			goto after_codegen;
		}
	}

	sig = mono_method_signature_internal (cfg->method);
	ctx->sig = sig;

	linfo = get_llvm_call_info (cfg, sig);
	ctx->linfo = linfo;
	if (!ctx_ok (ctx))
		return;

	if (cfg->rgctx_var)
		linfo->rgctx_arg = TRUE;

	ctx->method_type = method_type = sig_to_llvm_sig_full (ctx, sig, linfo);
	if (!ctx_ok (ctx))
		return;

	method = LLVMAddFunction (lmodule, ctx->method_name, method_type);
	ctx->lmethod = method;

	/*
	 * No calling-convention override: the rgctx/imt argument is tagged
	 * LLVM_ATTR_NEST instead, which stock LLVM pins to R10 on SysV. See the
	 * note in process_call (). The forked CallingConv::Mono is gone.
	 */

	/* if the method doesn't contain
	 *  (1) a call (so it's a leaf method)
	 *  (2) and no loops
	 * we can skip the GC safepoint on method entry. */
	gboolean requires_safepoint;
	requires_safepoint = cfg->has_calls;
	if (!requires_safepoint) {
		for (bb = cfg->bb_entry->next_bb; bb; bb = bb->next_bb) {
			if (bb->loop_body_start || (bb->flags & BB_EXCEPTION_HANDLER)) {
				requires_safepoint = TRUE;
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
			requires_safepoint = FALSE;
			break;
		default:
			break;
		}
	}
	ctx->has_safepoints = requires_safepoint;

	if (mono_threads_are_safepoints_enabled () && requires_safepoint) {
		if (cfg->method->wrapper_type != MONO_WRAPPER_ALLOC) {
			LLVMSetGC (method, "coreclr");
			emit_gc_safepoint_poll (ctx->module, ctx->lmodule, cfg);
		}
	}
	LLVMSetLinkage (method, LLVMPrivateLinkage);

	mono_llvm_add_func_attr (method, LLVM_ATTR_UW_TABLE);

	if (cfg->disable_omit_fp)
		mono_llvm_add_func_attr_nv (method, "no-frame-pointer-elim", "true");

	LLVMSetLinkage (method, LLVMExternalLinkage);

	if (cfg->method->save_lmf) {
		set_failure (ctx, "lmf");
		return;
	}

	if (sig->pinvoke && cfg->method->wrapper_type != MONO_WRAPPER_RUNTIME_INVOKE) {
		set_failure (ctx, "pinvoke signature");
		return;
	}

	header = cfg->header;
	for (i = 0; i < header->num_clauses; ++i) {
		clause = &header->clauses [i];
		if (clause->flags != MONO_EXCEPTION_CLAUSE_FINALLY && clause->flags != MONO_EXCEPTION_CLAUSE_FAULT && clause->flags != MONO_EXCEPTION_CLAUSE_NONE) {
			set_failure (ctx, "non-finally/catch/fault clause.");
			return;
		}
	}
	if (header->num_clauses || (cfg->method->iflags & METHOD_IMPL_ATTRIBUTE_NOINLINING) || cfg->no_inline)
		/* We can't handle inlined methods with clauses */
		mono_llvm_add_func_attr (method, LLVM_ATTR_NO_INLINE);

	if (linfo->rgctx_arg) {
		ctx->rgctx_arg = LLVMGetParam (method, linfo->rgctx_arg_pindex);
		ctx->rgctx_arg_pindex = linfo->rgctx_arg_pindex;
		/*
		 * We mark the rgctx parameter with the inreg attribute, which is mapped to
		 * MONO_ARCH_RGCTX_REG in the Mono calling convention in llvm, i.e.
		 * CC_X86_64_Mono in X86CallingConv.td.
		 */
		mono_llvm_add_param_attr (ctx->rgctx_arg, LLVM_ATTR_NEST);
		LLVMSetValueName (ctx->rgctx_arg, "rgctx");
	} else {
		ctx->rgctx_arg_pindex = -1;
	}
	if (cfg->vret_addr) {
		values [cfg->vret_addr->dreg] = LLVMGetParam (method, linfo->vret_arg_pindex);
		LLVMSetValueName (values [cfg->vret_addr->dreg], "vret");
		if (linfo->ret.storage == LLVMArgVtypeByRef) {
			mono_llvm_add_param_attr_with_type (LLVMGetParam (method, linfo->vret_arg_pindex), LLVM_ATTR_STRUCT_RET, type_to_llvm_type (ctx, sig->ret));
			mono_llvm_add_param_attr (LLVMGetParam (method, linfo->vret_arg_pindex), LLVM_ATTR_NO_ALIAS);
		}
	}

	if (sig->hasthis) {
		ctx->this_arg_pindex = linfo->this_arg_pindex;
		ctx->this_arg = LLVMGetParam (method, linfo->this_arg_pindex);
		values [cfg->args [0]->dreg] = ctx->this_arg;
		LLVMSetValueName (values [cfg->args [0]->dreg], "this");
	}
	if (linfo->dummy_arg)
		LLVMSetValueName (LLVMGetParam (method, linfo->dummy_arg_pindex), "dummy_arg");

	names = g_new (char *, sig->param_count);
	mono_method_get_param_names (cfg->method, (const char **) names);

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

		values [cfg->args [i + sig->hasthis]->dreg] = LLVMGetParam (method, pindex);
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
			mono_llvm_add_param_attr_with_type (LLVMGetParam (method, pindex), LLVM_ATTR_BY_VAL, type_to_llvm_arg_type (ctx, ainfo->type));

		if (ainfo->storage == LLVMArgVtypeByRef || ainfo->storage == LLVMArgVtypeAddr) {
			/* For OP_LDADDR */
			cfg->args [i + sig->hasthis]->opcode = OP_VTARG_ADDR;
		}
	}
	g_free (names);

	max_block_num = 0;
	for (bb = cfg->bb_entry; bb; bb = bb->next_bb)
		max_block_num = MAX (max_block_num, bb->block_num);
	ctx->bblocks = bblocks = g_new0 (BBInfo, max_block_num + 1);

	/* Add branches between non-consecutive bblocks */
	for (bb = cfg->bb_entry; bb; bb = bb->next_bb) {
		if (bb->last_ins && MONO_IS_COND_BRANCH_OP (bb->last_ins) &&
			bb->next_bb != bb->last_ins->inst_false_bb) {
			
			MonoInst *inst = (MonoInst*)mono_mempool_alloc0 (cfg->mempool, sizeof (MonoInst));
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
		LLVMBuilderRef builder;
		char *dname;
		char dname_buf[128];

		builder = create_builder (ctx);

		for (ins = bb->code; ins; ins = ins->next) {
			switch (ins->opcode) {
			case OP_PHI:
			case OP_FPHI:
			case OP_VPHI:
			case OP_XPHI: {
				LLVMTypeRef phi_type = llvm_type_to_stack_type (cfg, type_to_llvm_type (ctx, m_class_get_byval_arg (ins->klass)));
				LLVMTypeRef phi_etype = NULL;

				if (!ctx_ok (ctx))
					return;

				if (ins->opcode == OP_VPHI) {
					/* Treat valuetype PHI nodes as operating on the address itself */
					g_assert (ins->klass);
					phi_etype = type_to_llvm_type (ctx, m_class_get_byval_arg (ins->klass));
					phi_type = LLVMPointerType (phi_etype, 0);
				}

				/* 
				 * Have to precreate these, as they can be referenced by
				 * earlier instructions.
				 */
				sprintf (dname_buf, "t%d", ins->dreg);
				dname = dname_buf;
				values [ins->dreg] = LLVMBuildPhi (builder, phi_type, dname);

				if (ins->opcode == OP_VPHI)
					ctx->addresses [ins->dreg] = create_address (ctx, values [ins->dreg], phi_etype);

				g_ptr_array_add (ctx->phi_values, values [ins->dreg]);

				/* 
				 * Set the expected type of the incoming arguments since these have
				 * to have the same type.
				 */
				for (i = 0; i < ins->inst_phi_args [0]; i++) {
					int sreg1 = ins->inst_phi_args [i + 1];
					
					if (sreg1 != -1) {
						if (ins->opcode == OP_VPHI)
							ctx->is_vphi [sreg1] = TRUE;
						ctx->vreg_types [sreg1] = phi_type;
					}
				}
				break;
				}
			case OP_LDADDR:
				((MonoInst*)ins->inst_p0)->flags |= MONO_INST_INDIRECT;
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
		if (!(bb->region != (guint)-1 && !MONO_BBLOCK_IS_IN_REGION (bb, MONO_REGION_TRY))) {
			g_ptr_array_add (bblock_list, bb);
			bblocks [bb->block_num].added = TRUE;
		}
	}

	for (bb = cfg->bb_entry; bb; bb = bb->next_bb) {
		if (!bblocks [bb->block_num].added)
			g_ptr_array_add (bblock_list, bb);
	}

	/*
	 * Second pass: generate code.
	 */
	// Emit entry point
	entry_builder = create_builder (ctx);
	entry_bb = get_bb (ctx, cfg->bb_entry);
	LLVMPositionBuilderAtEnd (entry_builder, entry_bb);
	emit_entry_bb (ctx, entry_builder);

	for (bb = cfg->bb_entry; bb; bb = bb->next_bb) {
		int clause_index;
		char name [128];

		if (ctx->cfg->interp_entry_only || !(bb->region != (guint)-1 && (bb->flags & BB_EXCEPTION_HANDLER)))
			continue;

		clause_index = MONO_REGION_CLAUSE_INDEX (bb->region);
		g_hash_table_insert (ctx->region_to_handler, GUINT_TO_POINTER (mono_get_block_region_notry (cfg, bb->region)), bb);
		g_hash_table_insert (ctx->clause_to_handler, GINT_TO_POINTER (clause_index), bb);

		/*
		 * Create a new bblock which CALL_HANDLER/landing pads can branch to, because branching to the
		 * LLVM bblock containing a landing pad causes problems for the
		 * LLVM optimizer passes.
		 */
		sprintf (name, "BB%d_CALL_HANDLER_TARGET", bb->block_num);
		ctx->bblocks [bb->block_num].call_handler_target_bb = LLVMAppendBasicBlock (ctx->lmethod, name);
	}

	for (bb_index = 0; bb_index < bblock_list->len; ++bb_index) {
		bb = (MonoBasicBlock*)g_ptr_array_index (bblock_list, bb_index);

		// Prune unreachable mono BBs.
		if (!(bb == cfg->bb_entry || bb->in_count > 0))
			continue;

		process_bb (ctx, bb);
		if (!ctx_ok (ctx))
			return;
	}
	mono_memory_barrier ();

	/* Add incoming phi values */
	for (bb = cfg->bb_entry; bb; bb = bb->next_bb) {
		GSList *l, *ins_list;

		ins_list = bblocks [bb->block_num].phi_nodes;

		for (l = ins_list; l; l = l->next) {
			PhiNode *node = (PhiNode*)l->data;
			MonoInst *phi = node->phi;
			int sreg1 = node->sreg;
			LLVMBasicBlockRef in_bb;

			if (sreg1 == -1)
				continue;

			in_bb = get_end_bb (ctx, node->in_bb);

			if (ctx->unreachable [node->in_bb->block_num])
				continue;

			if (phi->opcode == OP_VPHI) {
				/*
				 * Under opaque pointers comparing LLVMTypeOf () of the two values
				 * is vacuous (ptr == ptr). Compare the tracked element types
				 * instead -- this is the only check that an incoming address's
				 * pointee matches the type the VPHI was created with.
				 */
				g_assert (ctx->addresses [sreg1]);
				g_assert (ctx->addresses [phi->dreg]);
				g_assert (ctx->addresses [sreg1]->type == ctx->addresses [phi->dreg]->type);
				LLVMAddIncoming (values [phi->dreg], &ctx->addresses [sreg1]->value, &in_bb, 1);
			} else {
				if (!values [sreg1]) {
					/* Can happen with values in EH clauses */
					set_failure (ctx, "incoming phi sreg1");
					return;
				}
				if (LLVMTypeOf (values [sreg1]) != LLVMTypeOf (values [phi->dreg])) {
					set_failure (ctx, "incoming phi arg type mismatch");
					return;
				}
				g_assert (LLVMTypeOf (values [sreg1]) == LLVMTypeOf (values [phi->dreg]));
				LLVMAddIncoming (values [phi->dreg], &values [sreg1], &in_bb, 1);
			}
		}
	}

	/* Nullify empty phi instructions */
	for (bb = cfg->bb_entry; bb; bb = bb->next_bb) {
		GSList *l, *ins_list;

		ins_list = bblocks [bb->block_num].phi_nodes;

		for (l = ins_list; l; l = l->next) {
			PhiNode *node = (PhiNode*)l->data;
			MonoInst *phi = node->phi;
			LLVMValueRef phi_ins = values [phi->dreg];

			if (!phi_ins)
				/* Already removed */
				continue;

			if (LLVMCountIncoming (phi_ins) == 0) {
				mono_llvm_replace_uses_of (phi_ins, LLVMConstNull (LLVMTypeOf (phi_ins)));
				LLVMInstructionEraseFromParent (phi_ins);
				values [phi->dreg] = NULL;
			}
		}
	}

	/* Create the SWITCH statements for ENDFINALLY instructions */
	for (bb = cfg->bb_entry; bb; bb = bb->next_bb) {
		BBInfo *info = &bblocks [bb->block_num];
		GSList *l;
		for (l = info->endfinally_switch_ins_list; l; l = l->next) {
			LLVMValueRef switch_ins = (LLVMValueRef)l->data;
			GSList *bb_list = info->call_handler_return_bbs;

			GSList *bb_list_iter;
			i = 0;
			for (bb_list_iter = bb_list; bb_list_iter; bb_list_iter = g_slist_next (bb_list_iter)) {
				LLVMAddCase (switch_ins, LLVMConstInt (LLVMInt32Type (), i + 1, FALSE), (LLVMBasicBlockRef)bb_list_iter->data);
				i ++;
			}
		}
	}

	ctx->module->max_method_idx = MAX (ctx->module->max_method_idx, (int)cfg->method_index);

	if (mini_get_debug_options ()->llvm_disable_inlining)
		mono_llvm_add_func_attr (method, LLVM_ATTR_NO_INLINE);

after_codegen:
	if (cfg->verbose_level > 1) {
		g_print ("\n*** Unoptimized LLVM IR for %s ***\n", mono_method_full_name (cfg->method, TRUE));
		mono_llvm_dump_module (ctx->lmodule);
		g_print ("***\n\n");
	}

	{
		LLVMValueRef md_args [16];
		LLVMValueRef md_node;

		md_args [0] = LLVMMDString (ctx->method_name, strlen (ctx->method_name));
		md_args [1] = LLVMConstInt (LLVMInt32Type (), 1, FALSE);
		md_node = LLVMMDNode (md_args, 2);
		LLVMAddNamedMetadataOperand (lmodule, "mono.function_indexes", md_node);
	}

	//LLVMVerifyFunction (method, 0);
	llvm_jit_finalize_method (ctx);

	if (ctx->module->method_to_lmethod)
		g_hash_table_insert (ctx->module->method_to_lmethod, cfg->method, ctx->lmethod);

	if (ctx->module->idx_to_lmethod)
		g_hash_table_insert (ctx->module->idx_to_lmethod, GINT_TO_POINTER (cfg->method_index), ctx->lmethod);
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
	MonoLLVMModule *module = (MonoLLVMModule*)info->llvm_module;
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
 * against unmodified LLVM 18 (the pipeline requires mono's forked
 * -place-safepoints / -spp-all-backedges passes), so the AOT code paths were
 * dead. These entry points remain only because aot-compiler.c references them
 * unconditionally; they must never be reached.
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

/*
 * decode_llvm_eh_info:
 *
 *   Decode the EH table emitted by llvm in jit mode, and store
 * the result into cfg.
 */
static void
decode_llvm_eh_info (EmitContext *ctx, gpointer eh_frame)
{
	MonoCompile *cfg = ctx->cfg;
	guint8 *cie, *fde;
	int fde_len;
	MonoLLVMFDEInfo info;
	MonoJitExceptionInfo *ei;
	guint8 *p = (guint8*)eh_frame;
	int version, fde_count, fde_offset;
	guint32 ei_len, i, nested_len;
	gpointer *type_info;
	gint32 *table;
	guint8 *unw_info;

	/*
	 * Decode the one element EH table emitted by the MonoException class
	 * in llvm.
	 */

	/* Similar to decode_llvm_mono_eh_frame () in aot-runtime.c */

	version = *p;
	g_assert (version == 3);
	p ++;
	p ++;
	p = (guint8 *)ALIGN_PTR_TO (p, 4);

	fde_count = *(guint32*)p;
	p += 4;
	table = (gint32*)p;

	g_assert (fde_count <= 2);

	/* The first entry is the real method */
	g_assert (table [0] == 1);
	fde_offset = table [1];
	table += fde_count * 2;
	/* Extra entry */
	cfg->code_len = table [0];
	fde_len = table [1] - fde_offset;
	table += 2;

	fde = (guint8*)eh_frame + fde_offset;
	cie = (guint8*)table;

	/* Compute lengths */
	mono_unwind_decode_llvm_mono_fde (fde, fde_len, cie, cfg->native_code, &info, NULL, NULL, NULL);

	ei = (MonoJitExceptionInfo *)g_malloc0 (info.ex_info_len * sizeof (MonoJitExceptionInfo));
	type_info = (gpointer *)g_malloc0 (info.ex_info_len * sizeof (gpointer));
	unw_info = (guint8*)g_malloc0 (info.unw_info_len);

	mono_unwind_decode_llvm_mono_fde (fde, fde_len, cie, cfg->native_code, &info, ei, type_info, unw_info);

	cfg->encoded_unwind_ops = unw_info;
	cfg->encoded_unwind_ops_len = info.unw_info_len;
	if (cfg->verbose_level > 1)
		mono_print_unwind_info (cfg->encoded_unwind_ops, cfg->encoded_unwind_ops_len);
	if (info.this_reg != -1) {
		cfg->llvm_this_reg = info.this_reg;
		cfg->llvm_this_offset = info.this_offset;
	}

	ei_len = info.ex_info_len;

	// Nested clauses are currently disabled
	nested_len = 0;

	cfg->llvm_ex_info = (MonoJitExceptionInfo*)mono_mempool_alloc0 (cfg->mempool, (ei_len + nested_len) * sizeof (MonoJitExceptionInfo));
	cfg->llvm_ex_info_len = ei_len + nested_len;
	memcpy (cfg->llvm_ex_info, ei, ei_len * sizeof (MonoJitExceptionInfo));
	/* Fill the rest of the information from the type info */
	for (i = 0; i < ei_len; ++i) {
		gint32 clause_index = *(gint32*)type_info [i];
		MonoExceptionClause *clause = &cfg->header->clauses [clause_index];

		cfg->llvm_ex_info [i].flags = clause->flags;
		cfg->llvm_ex_info [i].data.catch_class = clause->data.catch_class;
		cfg->llvm_ex_info [i].clause_index = clause_index;
	}
}

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

	module->mono_ee = (MonoEERef*)mono_llvm_create_ee (&module->ee);

	// This contains just the intrinsics
	module->lmodule = LLVMModuleCreateWithName ("jit-global-module");
	add_intrinsics (module->lmodule);
	add_types (module);

	module->llvm_types = g_hash_table_new (NULL, NULL);

	mono_memory_barrier ();

	dinfo->llvm_module = module;

	mono_loader_unlock ();
}

static void
llvm_jit_finalize_method (EmitContext *ctx)
{
	MonoCompile *cfg = ctx->cfg;
	MonoDomain *domain = mono_domain_get ();
	MonoJitDomainInfo *domain_info;
	int nvars = g_hash_table_size (ctx->jit_callees);
	LLVMValueRef *callee_vars = g_new0 (LLVMValueRef, nvars); 
	gpointer *callee_addrs = g_new0 (gpointer, nvars);
	GHashTableIter iter;
	LLVMValueRef var;
	MonoMethod *callee;
	gpointer eh_frame;
	int i;

	/*
	 * Compute the addresses of the LLVM globals pointing to the
	 * methods called by the current method. Pass it to the trampoline
	 * code so it can update them after their corresponding method was
	 * compiled.
	 */
	g_hash_table_iter_init (&iter, ctx->jit_callees);
	i = 0;
	while (g_hash_table_iter_next (&iter, NULL, (void**)&var))
		callee_vars [i ++] = var;

	mono_codeman_enable_write ();
	guint32 llvm_code_size = 0;
	gpointer dwarf_eh_frame = NULL;
	guint32 dwarf_eh_frame_size = 0;
	cfg->native_code = (guint8*)mono_llvm_compile_method (ctx->module->mono_ee, cfg, ctx->lmethod, nvars, callee_vars, callee_addrs, &eh_frame, &llvm_code_size, &dwarf_eh_frame, &dwarf_eh_frame_size);
	mono_llvm_remove_gc_safepoint_poll (ctx->lmodule);
	mono_codeman_disable_write ();
	if (cfg->verbose_level > 1) {
		g_print ("\n*** Optimized LLVM IR for %s ***\n", mono_method_full_name (cfg->method, TRUE));
		mono_llvm_dump_module (ctx->lmodule);
		g_print ("***\n\n");
	}

	/*
	 * eh_frame is the address of the "mono_eh_frame" global, which only the
	 * FORKED LLVM emitted (via its MonoEHFrame support). Unmodified LLVM 18
	 * emits a standard DWARF .eh_frame section instead, and consuming that is
	 * not ported yet - which is exactly why methods carrying EH clauses are
	 * gated out to the classic JIT in mono_llvm_check_method_supported().
	 *
	 * So under stock LLVM this is NULL for every method that reaches here, and
	 * those methods have no EH clauses to describe. Decode only if the forked
	 * global was actually present.
	 */
	if (eh_frame)
		decode_llvm_eh_info (ctx, eh_frame);

	/*
	 * decode_llvm_eh_info() is not only about EH: under the forked LLVM it also
	 * produced three non-EH outputs. With it skipped, they have to come from
	 * somewhere else, or be accounted for:
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
	 * 3. cfg->llvm_this_reg - not needed while generic-shared methods are gated
	 *    out of the LLVM path in mono_llvm_check_method_supported ().
	 */
	cfg->code_len = llvm_code_size;
	/*
	 * Fail loudly rather than silently building a zero-byte jit-info again: an
	 * emitted method always has a non-empty body.
	 */
	g_assert (cfg->code_len > 0);

	{
		GSList *unwind_ops = NULL;

		if (!mono_llvm_eh_frame_to_unwind_ops ((guint8*)dwarf_eh_frame, dwarf_eh_frame_size,
						       cfg->native_code, cfg->code_len, &unwind_ops)) {
			/*
			 * No usable FDE, or CFI we cannot represent. Publishing the method
			 * with no unwind information is the one outcome we must avoid, so
			 * bail out and let the classic JIT compile it instead.
			 */
			set_failure (ctx, "no usable DWARF unwind info");
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

	mono_domain_lock (domain);
	domain_info = domain_jit_info (domain);
	if (!domain_info->llvm_jit_callees)
		domain_info->llvm_jit_callees = g_hash_table_new (NULL, NULL);
	g_hash_table_iter_init (&iter, ctx->jit_callees);
	i = 0;
	while (g_hash_table_iter_next (&iter, (void**)&callee, (void**)&var)) {
		GSList *addrs = (GSList*)g_hash_table_lookup (domain_info->llvm_jit_callees, callee);
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

static void
llvm_jit_finalize_method (EmitContext *ctx)
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
		cpu_features = MONO_CPU_INITED | (MonoCPUFeatures)mono_llvm_check_cpu_features (flags_map, G_N_ELEMENTS (flags_map));

	return cpu_features;
}

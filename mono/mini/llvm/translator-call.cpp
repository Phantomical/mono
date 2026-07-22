/**
 * \file
 * translator-call.cpp: method prologue, call emission and exception emission.
 *
 * Split out of translator.cpp; see translator-internal.hpp for the shape of the
 * split and for everything shared between the pieces.
 *
 * Copyright 2009-2011 Novell Inc (http://www.novell.com)
 * Copyright 2011 Xamarin Inc (http://www.xamarin.com)
 * Licensed under the MIT license. See LICENSE file in the project root for full license information.
 */

#include "translator-internal.hpp"

#ifndef DISABLE_JIT

/*
 * Marshal an array of LLVMValueRef constants into a ConstantVector. Every
 * element must already be an llvm::Constant (callers build them with
 * ConstantInt::get); cast<Constant> asserts that invariant.
 */
static LLVMValueRef
const_vector (const LLVMValueRef *vals, unsigned count)
{
	llvm::SmallVector<llvm::Constant *, 16> elts;
	elts.reserve (count);
	for (unsigned i = 0; i < count; ++i)
		elts.push_back (llvm::cast<llvm::Constant> (llvm::unwrap (vals [i])));
	return llvm::wrap (llvm::ConstantVector::get (elts));
}

void
emit_div_check (EmitContext *ctx, llvm::IRBuilder<> *builder, MonoBasicBlock *bb, MonoInst *ins, LLVMValueRef lhs, LLVMValueRef rhs)
{
	bool need_div_check = ctx->cfg->backend->need_div_check;

	if (bb->region)
		/* LLVM doesn't know that these can throw an exception since they are not called through an intrinsic */
		need_div_check = true;

	if (!need_div_check)
		return;

	switch (ins->opcode) {
	case OP_IDIV:
	case OP_LDIV:
	case OP_IREM:
	case OP_LREM:
	case OP_IDIV_UN:
	case OP_LDIV_UN:
	case OP_IREM_UN:
	case OP_LREM_UN:
	case OP_IDIV_IMM:
	case OP_LDIV_IMM:
	case OP_IREM_IMM:
	case OP_LREM_IMM:
	case OP_IDIV_UN_IMM:
	case OP_LDIV_UN_IMM:
	case OP_IREM_UN_IMM:
	case OP_LREM_UN_IMM: {
		LLVMValueRef cmp;
		bool is_signed = (ins->opcode == OP_IDIV || ins->opcode == OP_LDIV || ins->opcode == OP_IREM || ins->opcode == OP_LREM ||
							  ins->opcode == OP_IDIV_IMM || ins->opcode == OP_LDIV_IMM || ins->opcode == OP_IREM_IMM || ins->opcode == OP_LREM_IMM);

		cmp = LLVMBuildICmp (llvm::wrap (builder), LLVMIntEQ, rhs, llvm::wrap (llvm::ConstantInt::get (llvm::unwrap (LLVMTypeOf (rhs)), 0, false)), "");
		emit_cond_system_exception (ctx, bb, "DivideByZeroException", cmp, FALSE);
		if (!ctx_ok (ctx))
			break;
		builder = ctx->builder;

		/* b == -1 && a == 0x80000000 */
		if (is_signed) {
			LLVMValueRef c = (LLVMTypeOf (lhs) == llvm::wrap (llvm::Type::getInt32Ty (ctx->llvm_ctx ()))) ? llvm::wrap (llvm::ConstantInt::get (llvm::unwrap (LLVMTypeOf (lhs)), 0x80000000, false)) : llvm::wrap (llvm::ConstantInt::get (llvm::unwrap (LLVMTypeOf (lhs)), 0x8000000000000000LL, false));
			LLVMValueRef cond1 = LLVMBuildICmp (llvm::wrap (builder), LLVMIntEQ, rhs, llvm::wrap (llvm::ConstantInt::get (llvm::unwrap (LLVMTypeOf (rhs)), -1, false)), "");
			LLVMValueRef cond2 = LLVMBuildICmp (llvm::wrap (builder), LLVMIntEQ, lhs, c, "");

			cmp = LLVMBuildICmp (llvm::wrap (builder), LLVMIntEQ, llvm::wrap (builder->CreateAnd (llvm::unwrap (cond1), llvm::unwrap (cond2), "")), llvm::wrap (llvm::ConstantInt::get (llvm::Type::getInt1Ty (ctx->llvm_ctx ()), 1, false)), "");
			emit_cond_system_exception (ctx, bb, "OverflowException", cmp, FALSE);
			if (!ctx_ok (ctx))
				break;
			builder = ctx->builder;
		}
		break;
	}
	default:
		break;
	}
}

/*
 * Stackmap patchpoint id for the gshared this/rgctx home slot. There is exactly
 * one llvm.experimental.stackmap per gshared method, so the value is arbitrary;
 * the parser (translator.cpp) reads the first record's first location regardless.
 */
constexpr int MONO_LLVM_THIS_SLOT_STACKMAP_ID = 0;

/*
 * Record the location of SLOT (an alloca holding this/mrgctx) with a
 * llvm.experimental.stackmap intrinsic, so that after code emission the backend
 * can read the slot's home register+offset out of the `.llvm_stackmaps` section
 * and publish it as cfg->llvm_this_reg/llvm_this_offset (mini.c:2573-2577).
 *
 * Stock LLVM has no built-in way to publish that location, so gshared methods
 * would otherwise reach mini.c's g_assert(cfg->llvm_this_reg != -1) with the
 * field unset. A stackmap over the alloca supplies it (design 3.1 / S6): it
 * records the slot's address as a frame reg+offset that is stable for the whole
 * method, exactly what a stack walk needs.
 *
 * The intrinsic is declared on demand in the method's own module (every method
 * gets its own, so declaring by name is race-free), variadic void(i64,i32,...).
 */
static void
emit_this_slot_stackmap (EmitContext *ctx, llvm::IRBuilder<> *builder, LLVMValueRef slot)
{
	LLVMTypeRef params [] = { llvm::wrap (llvm::Type::getInt64Ty (ctx->llvm_ctx ())), llvm::wrap (llvm::Type::getInt32Ty (ctx->llvm_ctx ())) };
	LLVMTypeRef sm_type = LLVMFunctionType (llvm::wrap (llvm::Type::getVoidTy (ctx->llvm_ctx ())), params, 2, TRUE);
	LLVMValueRef sm = LLVMGetNamedFunction (ctx->lmodule, "llvm.experimental.stackmap");

	if (!sm)
		sm = LLVMAddFunction (ctx->lmodule, "llvm.experimental.stackmap", sm_type);

	LLVMValueRef args [] = {
		llvm::wrap (llvm::ConstantInt::get (llvm::Type::getInt64Ty (ctx->llvm_ctx ()), MONO_LLVM_THIS_SLOT_STACKMAP_ID, false)),
		llvm::wrap (llvm::ConstantInt::get (llvm::Type::getInt32Ty (ctx->llvm_ctx ()), 0, false)),
		slot,
	};
	LLVMBuildCall2 (llvm::wrap (builder), sm_type, sm, args, 3, "");
}

/*
 * emit_entry_bb:
 *
 *   Emit code to load/convert arguments.
 */
void
emit_entry_bb (EmitContext *ctx, llvm::IRBuilder<> *builder)
{
	int i, pindex;
	MonoCompile *cfg = ctx->cfg;
	MonoMethodSignature *sig = ctx->sig;
	LLVMCallInfo *linfo = ctx->linfo;
	MonoBasicBlock *bb;
	char **names;

	llvm::IRBuilder<> *old_builder = ctx->builder;
	ctx->builder = builder;

	ctx->alloca_builder = create_builder (ctx);

	/*
	 * Handle indirect/volatile variables by allocating memory for them
	 * using 'alloca', and storing their address in a temporary.
	 */
	for (i = 0; i < cfg->num_varinfo; ++i) {
		MonoInst *var = cfg->varinfo [i];
		LLVMTypeRef vtype;

		if ((var->opcode == OP_GSHAREDVT_LOCAL || var->opcode == OP_GSHAREDVT_ARG_REGOFFSET))
			continue;

#ifdef TARGET_WASM
		// For GC stack scanning to work, have to spill all reference variables to the stack
		// Some ref variables have type intptr
		if (ctx->has_safepoints && (MONO_TYPE_IS_REFERENCE (var->inst_vtype) || var->inst_vtype->type == MONO_TYPE_I) && var != ctx->cfg->rgctx_var)
			var->flags |= MONO_INST_INDIRECT;
#endif

		if (var->flags & (MONO_INST_VOLATILE|MONO_INST_INDIRECT) || (mini_type_is_vtype (var->inst_vtype) && !MONO_CLASS_IS_SIMD (ctx->cfg, var->klass))) {
			vtype = type_to_llvm_type (ctx, var->inst_vtype);
			if (!ctx_ok (ctx))
				return;
			/* Could be already created by an OP_VPHI */
			if (!ctx->addresses [var->dreg]) {
				if (var->flags & MONO_INST_LMF) {
					LLVMTypeRef lmf_type = LLVMArrayType (llvm::wrap (llvm::Type::getInt8Ty (ctx->llvm_ctx ())), MONO_ABI_SIZEOF (MonoLMF));
					ctx->addresses [var->dreg] = create_address (ctx, build_alloca_llvm_type_name (ctx, lmf_type, sizeof (target_mgreg_t), "entry_lmf"), lmf_type);
				} else {
					ctx->addresses [var->dreg] = create_address (ctx, build_named_alloca (ctx, var->inst_vtype, "entry"), vtype);
				}
				//LLVMSetValueName (ctx->addresses [var->dreg], g_strdup_printf ("vreg_loc_%d", var->dreg));
			}
			ctx->vreg_cli_types [var->dreg] = var->inst_vtype;
		}
	}

	names = g_new (char *, sig->param_count);
	mono_method_get_param_names (cfg->method, const_cast<const char **>(names));

	for (i = 0; i < sig->param_count; ++i) {
		LLVMArgInfo *ainfo = &linfo->args [i + sig->hasthis];
		int reg = cfg->args [i + sig->hasthis]->dreg;
		char *name;

		pindex = ainfo->pindex;

		switch (ainfo->storage) {
		case LLVMArgVtypeInReg:
		case LLVMArgAsFpArgs: {
			LLVMValueRef args [8];
			int j;

			pindex += ainfo->ndummy_fpargs;

			/* The argument is received as a set of int/fp arguments, store them into the real argument */
			memset (args, 0, sizeof (args));
			if (ainfo->storage == LLVMArgVtypeInReg) {
				args [0] = LLVMGetParam (ctx->lmethod, pindex);
				if (ainfo->pair_storage [1] != LLVMArgNone)
					args [1] = LLVMGetParam (ctx->lmethod, pindex + 1);
			} else {
				g_assert (ainfo->nslots <= 8);
				for (j = 0; j < ainfo->nslots; ++j)
					args [j] = LLVMGetParam (ctx->lmethod, pindex + j);
			}
			ctx->addresses [reg] = build_alloca_address (ctx, ainfo->type);

			emit_args_to_vtype (ctx, builder, ainfo->type, ctx->addresses [reg]->value, ainfo, args);
			break;
		}
		case LLVMArgVtypeByVal: {
			/* Element type must match the declaration in sig_to_llvm_sig_full () */
			ctx->addresses [reg] = create_address (ctx, LLVMGetParam (ctx->lmethod, pindex), type_to_llvm_arg_type (ctx, ainfo->type));
			break;
		}
		case LLVMArgVtypeAddr:
		case LLVMArgVtypeByRef: {
			/* The argument is passed by ref */
			/* Element type must match the declaration in sig_to_llvm_sig_full () */
			ctx->addresses [reg] = create_address (ctx, LLVMGetParam (ctx->lmethod, pindex), type_to_llvm_arg_type (ctx, ainfo->type));
			break;
		}
		case LLVMArgAsIArgs: {
			LLVMValueRef arg = LLVMGetParam (ctx->lmethod, pindex);
			int size;
			MonoType *t = mini_get_underlying_type (ainfo->type);

			/* The argument is received as an array of ints, store it into the real argument */
			ctx->addresses [reg] = build_alloca_address (ctx, t);

			size = mono_class_value_size (mono_class_from_mono_type_internal (t), NULL);
			if (size == 0) {
			} else if (size < TARGET_SIZEOF_VOID_P) {
				/* The upper bits of the registers might not be valid */
				LLVMValueRef val = LLVMBuildExtractValue (llvm::wrap (builder), arg, 0, "");
				LLVMValueRef dest = convert (ctx, ctx->addresses [reg]->value, llvm::wrap (llvm::PointerType::get (ctx->llvm_ctx (), 0)));
				LLVMBuildStore (llvm::wrap (ctx->builder), LLVMBuildTrunc (llvm::wrap (builder), val, llvm::wrap (llvm::Type::getIntNTy (ctx->llvm_ctx (), size * 8)), ""), dest);
			} else {
				LLVMBuildStore (llvm::wrap (ctx->builder), arg, convert (ctx, ctx->addresses [reg]->value, llvm::wrap (llvm::PointerType::get (ctx->llvm_ctx (), 0))));
			}
			break;
		}
		case LLVMArgVtypeAsScalar:
			g_assert_not_reached ();
			break;
		case LLVMArgGsharedvtFixed: {
			/* These are non-gsharedvt arguments passed by ref, the rest of the IR treats them as scalars */
			LLVMValueRef arg = LLVMGetParam (ctx->lmethod, pindex);

			if (names [i])
				name = g_strdup_printf ("arg_%s", names [i]);
			else
				name = g_strdup_printf ("arg_%d", i);

			ctx->values [reg] = LLVMBuildLoad2 (llvm::wrap (builder), type_to_llvm_type (ctx, ainfo->type), convert (ctx, arg, llvm::wrap (llvm::PointerType::get (ctx->llvm_ctx (), 0))), name);
			break;
		}
		case LLVMArgGsharedvtFixedVtype: {
			LLVMValueRef arg = LLVMGetParam (ctx->lmethod, pindex);

			if (names [i])
				name = g_strdup_printf ("vtype_arg_%s", names [i]);
			else
				name = g_strdup_printf ("vtype_arg_%d", i);

			/* Non-gsharedvt vtype argument passed by ref, the rest of the IR treats it as a vtype */
			g_assert (ctx->addresses [reg]);
			LLVMSetValueName (ctx->addresses [reg]->value, name);
			LLVMBuildStore (llvm::wrap (builder), LLVMBuildLoad2 (llvm::wrap (builder), type_to_llvm_type (ctx, ainfo->type), convert (ctx, arg, llvm::wrap (llvm::PointerType::get (ctx->llvm_ctx (), 0))), ""), ctx->addresses [reg]->value);
			break;
		}
		case LLVMArgGsharedvtVariable:
			/* The IR treats these as variables with addresses */
			/* Element type must match the declaration in sig_to_llvm_sig_full () */
			if (!ctx->addresses [reg])
				ctx->addresses [reg] = create_address (ctx, LLVMGetParam (ctx->lmethod, pindex), IntPtrType ());
			break;
		default: {
			LLVMTypeRef t;
			/* Needed to avoid phi argument mismatch errors since operations on pointers produce i32/i64 */
			if (ainfo->type->byref)
				t = IntPtrType ();
			else
				t = type_to_llvm_type (ctx, ainfo->type);
			ctx->values [reg] = convert_full (ctx, ctx->values [reg], llvm_type_to_stack_type (cfg, t), type_is_unsigned (ctx, ainfo->type));
			break;
		}
		}

		switch (ainfo->storage) {
		case LLVMArgVtypeInReg:
		case LLVMArgVtypeByVal:
		case LLVMArgAsIArgs:
#ifdef ENABLE_NETCORE
			// FIXME: Enabling this fails on windows
		case LLVMArgVtypeAddr:
		case LLVMArgVtypeByRef:
#endif
		{
			if (MONO_CLASS_IS_SIMD (ctx->cfg, mono_class_from_mono_type_internal (ainfo->type)))
				/* Treat these as normal values */
				ctx->values [reg] = LLVMBuildLoad2 (llvm::wrap (builder), ctx->addresses [reg]->type, ctx->addresses [reg]->value, "simd_vtype");
			break;
		}
		default:
			break;
		}
	}
	g_free (names);

	if (sig->hasthis) {
		/* Handle this arguments as inputs to phi nodes */
		int reg = cfg->args [0]->dreg;
		if (ctx->vreg_types [reg])
			ctx->values [reg] = convert (ctx, ctx->values [reg], ctx->vreg_types [reg]);
	}

	if (cfg->vret_addr)
		emit_volatile_store (ctx, cfg->vret_addr->dreg);
	if (sig->hasthis)
		emit_volatile_store (ctx, cfg->args [0]->dreg);
	for (i = 0; i < sig->param_count; ++i)
		if (!mini_type_is_vtype (sig->params [i]))
			emit_volatile_store (ctx, cfg->args [i + sig->hasthis]->dreg);

	if (sig->hasthis && !cfg->rgctx_var && cfg->gshared) {
		LLVMValueRef this_alloc;

		/*
		 * The exception handling code needs the location where the this argument was
		 * stored for gshared methods. We create a separate alloca to hold it, and mark it
		 * with the "mono.this" custom metadata to tell llvm that it needs to save its
		 * location into the LSDA.
		 */
		this_alloc = mono_llvm_build_alloca (llvm::wrap (builder), ThisType (), llvm::wrap (llvm::ConstantInt::get (llvm::Type::getInt32Ty (ctx->llvm_ctx ()), 1, false)), 0, "");
		/* This volatile store will keep the alloca alive */
		mono_llvm_build_store (llvm::wrap (builder), ctx->values [cfg->args [0]->dreg], this_alloc, TRUE, LLVM_BARRIER_NONE);

		set_metadata_flag (this_alloc, "mono.this");

		/*
		 * Stock LLVM 18 ignores "mono.this"; record the slot's home location via a
		 * stackmap so the backend can recover cfg->llvm_this_reg/offset (#15, S6.1).
		 */
		emit_this_slot_stackmap (ctx, builder, this_alloc);
	}

	if (cfg->rgctx_var) {
		if (!(cfg->rgctx_var->flags & MONO_INST_VOLATILE)) {
			/* FIXME: This could be volatile even in llvmonly mode if used inside a clause etc. */
			g_assert (!ctx->addresses [cfg->rgctx_var->dreg]);
			ctx->values [cfg->rgctx_var->dreg] = ctx->rgctx_arg;

			/*
			 * MRGCTX gshared (#15, S6.2): the rgctx normally stays in the nest
			 * register (caller-saved, clobbered by the time a stack walk runs), so
			 * there is no method-wide-stable home for it. Force a dedicated spill
			 * slot holding the rgctx and record it with a stackmap, exactly as the
			 * this-derived case does, so llvm_jit_finalize_method can recover
			 * cfg->llvm_this_reg/offset. The spill is additional to the register
			 * value above; the body keeps using the register.
			 */
			if (cfg->gshared) {
				LLVMValueRef rgctx_slot = mono_llvm_build_alloca (llvm::wrap (builder), IntPtrType (), llvm::wrap (llvm::ConstantInt::get (llvm::Type::getInt32Ty (ctx->llvm_ctx ()), 1, false)), 0, "");
				/* Volatile store keeps the slot alive. */
				mono_llvm_build_store (llvm::wrap (builder), convert (ctx, ctx->rgctx_arg, IntPtrType ()), rgctx_slot, TRUE, LLVM_BARRIER_NONE);
				set_metadata_flag (rgctx_slot, "mono.this");
				emit_this_slot_stackmap (ctx, builder, rgctx_slot);
			}
		} else {
			LLVMValueRef rgctx_alloc, store;

			/*
			 * We handle the rgctx arg similarly to the this pointer.
			 */
			g_assert (ctx->addresses [cfg->rgctx_var->dreg]);
			rgctx_alloc = ctx->addresses [cfg->rgctx_var->dreg]->value;
			/* This volatile store will keep the alloca alive */
			store = mono_llvm_build_store (llvm::wrap (builder), convert (ctx, ctx->rgctx_arg, IntPtrType ()), rgctx_alloc, TRUE, LLVM_BARRIER_NONE);

			set_metadata_flag (rgctx_alloc, "mono.this");

			/*
			 * MRGCTX gshared (#15, S6.2): rgctx_alloc already holds the rgctx via
			 * the volatile store above; record its home slot so the backend can
			 * recover cfg->llvm_this_reg/offset for stack-walk generic-context
			 * reconstruction.
			 */
			if (cfg->gshared)
				emit_this_slot_stackmap (ctx, builder, rgctx_alloc);
		}
	}

	/*
	 * For finally clauses, create an indicator variable telling OP_ENDFINALLY whenever
	 * it needs to continue normally, or return back to the exception handling system.
	 */
	for (bb = cfg->bb_entry; bb; bb = bb->next_bb) {
		char name [128];

		if (!(bb->region != static_cast<guint>(-1) && (bb->flags & BB_EXCEPTION_HANDLER)))
			continue;

		if (bb->in_scount == 0) {
			LLVMValueRef val;

			sprintf (name, "finally_ind_bb%d", bb->block_num);
			val = LLVMBuildAlloca (llvm::wrap (builder), llvm::wrap (llvm::Type::getInt32Ty (ctx->llvm_ctx ())), name);
			LLVMBuildStore (llvm::wrap (builder), llvm::wrap (llvm::ConstantInt::get (llvm::Type::getInt32Ty (ctx->llvm_ctx ()), 0, false)), val);

			ctx->bblocks [bb->block_num].finally_ind = val;
		} else {
			/* Create a variable to hold the exception var */
			if (!ctx->ex_var)
				ctx->ex_var = LLVMBuildAlloca (llvm::wrap (builder), ObjRefType (), "exvar");
		}
	}
	ctx->builder = old_builder;
}

static inline bool
is_supported_callconv (EmitContext *ctx, MonoCallInst *call)
{
#if defined(TARGET_WIN32) && defined(TARGET_AMD64)
	bool result = (call->signature->call_convention == MONO_CALL_DEFAULT) ||
			  (call->signature->call_convention == MONO_CALL_C) ||
			  (call->signature->call_convention == MONO_CALL_STDCALL);
#else
	bool result = (call->signature->call_convention == MONO_CALL_DEFAULT);
#endif
	return result;
}

void
process_call (EmitContext *ctx, MonoBasicBlock *bb, llvm::IRBuilder<> **builder_ref, MonoInst *ins)
{
	MonoCompile *cfg = ctx->cfg;
	LLVMValueRef *values = ctx->values;
	Address **addresses = ctx->addresses;
	MonoCallInst *call = reinterpret_cast<MonoCallInst*>(ins);
	MonoMethodSignature *sig = call->signature;
	LLVMValueRef callee = nullptr, lcall;
	LLVMValueRef *args;
	LLVMCallInfo *cinfo;
	GSList *l;
	int i, len, nargs;
	bool vretaddr;
	LLVMTypeRef llvm_sig;
	gpointer target;
	bool is_virtual, calli;
	llvm::IRBuilder<> *builder = *builder_ref;

	/* If both imt and rgctx arg are required, only pass the imt arg, the rgctx trampoline will pass the rgctx */
	if (call->imt_arg_reg)
		call->rgctx_arg_reg = 0;

	if (!is_supported_callconv (ctx, call)) {
		set_failure (ctx, "non-default callconv");
		return;
	}

	cinfo = call->cinfo;
	g_assert (cinfo);
	if (call->rgctx_arg_reg)
		cinfo->rgctx_arg = TRUE;
	if (call->imt_arg_reg)
		cinfo->imt_arg = TRUE;
	vretaddr = (cinfo->ret.storage == LLVMArgVtypeRetAddr || cinfo->ret.storage == LLVMArgVtypeByRef || cinfo->ret.storage == LLVMArgGsharedvtFixed || cinfo->ret.storage == LLVMArgGsharedvtVariable || cinfo->ret.storage == LLVMArgGsharedvtFixedVtype);

	llvm_sig = sig_to_llvm_sig_full (ctx, sig, cinfo);
	if (!ctx_ok (ctx))
		return;

	int const opcode = ins->opcode;

	is_virtual = opcode == OP_VOIDCALL_MEMBASE || opcode == OP_CALL_MEMBASE
			|| opcode == OP_VCALL_MEMBASE || opcode == OP_LCALL_MEMBASE
			|| opcode == OP_FCALL_MEMBASE || opcode == OP_RCALL_MEMBASE
			|| opcode == OP_TAILCALL_MEMBASE;
	calli = !call->fptr_is_patch && (opcode == OP_VOIDCALL_REG || opcode == OP_CALL_REG
		|| opcode == OP_VCALL_REG || opcode == OP_LCALL_REG || opcode == OP_FCALL_REG
		|| opcode == OP_RCALL_REG || opcode == OP_TAILCALL_REG);

	/* FIXME: Avoid creating duplicate methods */

	if (ins->flags & MONO_INST_HAS_METHOD) {
		if (is_virtual) {
			callee = nullptr;
		} else {
			if (cfg->method == call->method) {
				callee = ctx->lmethod;
			} else {
				ERROR_DECL (error);
				static int tramp_index;
				char *name;

				name = g_strdup_printf ("[tramp_%d] %s", tramp_index, mono_method_full_name (call->method, TRUE));
				tramp_index ++;

				/*
				 * Use our trampoline infrastructure for lazy compilation instead of llvm's.
				 * Make all calls through a global. The address of the global will be saved in
				 * MonoJitDomainInfo.llvm_jit_callees and updated when the method it refers to is
				 * compiled.
				 */
				auto jit_callee_it = ctx->jit_callees.find (call->method);
				LLVMValueRef tramp_var = jit_callee_it != ctx->jit_callees.end () ? jit_callee_it->second : nullptr;
				if (!tramp_var) {
					target =
						mono_create_jit_trampoline (mono_domain_get (),
													call->method, error);
					if (!is_ok (error)) {
						set_failure (ctx, mono_error_get_message (error));
						mono_error_cleanup (error);
						return;
					}

					tramp_var = LLVMAddGlobal (ctx->lmodule, llvm::wrap (llvm::PointerType::get (ctx->llvm_ctx (), 0)), name);
					LLVMSetInitializer (tramp_var, LLVMConstIntToPtr (llvm::wrap (llvm::ConstantInt::get (llvm::Type::getInt64Ty (ctx->llvm_ctx ()), static_cast<guint64>(reinterpret_cast<size_t>(target)), false)), llvm::wrap (llvm::PointerType::get (ctx->llvm_ctx (), 0))));
					LLVMSetLinkage (tramp_var, LLVMExternalLinkage);
					ctx->jit_callees [call->method] = tramp_var;
				}
				callee = LLVMBuildLoad2 (llvm::wrap (builder), llvm::wrap (llvm::PointerType::get (ctx->llvm_ctx (), 0)), tramp_var, "");
			}
		}

		if (call->method && strstr (m_class_get_name (call->method->klass), "AsyncVoidMethodBuilder")) {
			/* LLVM miscompiles async methods */
			set_failure (ctx, "#13734");
			return;
		}
	} else if (calli) {
	} else {
		const MonoJitICallId jit_icall_id = call->jit_icall_id;

		if (jit_icall_id) {
			callee = get_jit_callee (ctx, "", llvm_sig, MONO_PATCH_INFO_JIT_ICALL_ID, GUINT_TO_POINTER (jit_icall_id));
		} else {
			{
				if (cfg->abs_patches) {
					MonoJumpInfo *abs_ji = static_cast<MonoJumpInfo*>(g_hash_table_lookup (cfg->abs_patches, call->fptr));
					if (abs_ji) {
						ERROR_DECL (error);

						target = mono_resolve_patch_target (cfg->method, cfg->domain, NULL, abs_ji, FALSE, error);
						mono_error_assert_ok (error);
						callee = get_jit_callee (ctx, "", llvm_sig, abs_ji->type, abs_ji->data.target);
					} else {
						g_assert_not_reached ();
					}
				} else {
					g_assert_not_reached ();
				}
			}
		}
	}

	if (is_virtual) {
		int size = TARGET_SIZEOF_VOID_P;
		LLVMValueRef index;

		g_assert (ins->inst_offset % size == 0);
		index = llvm::wrap (llvm::ConstantInt::get (llvm::Type::getInt32Ty (ctx->llvm_ctx ()), ins->inst_offset / size, false));

		callee = convert (ctx, LLVMBuildLoad2 (llvm::wrap (builder), llvm::wrap (llvm::PointerType::get (ctx->llvm_ctx (), 0)), LLVMBuildGEP2 (llvm::wrap (builder), llvm::wrap (llvm::PointerType::get (ctx->llvm_ctx (), 0)), convert (ctx, values [ins->inst_basereg], llvm::wrap (llvm::PointerType::get (ctx->llvm_ctx (), 0))), &index, 1, ""), ""), llvm::wrap (llvm::PointerType::get (ctx->llvm_ctx (), 0)));
	} else if (calli) {
		callee = convert (ctx, values [ins->sreg1], llvm::wrap (llvm::PointerType::get (ctx->llvm_ctx (), 0)));
	} else {
		if (ins->flags & MONO_INST_HAS_METHOD) {
		}
	}

	/* 
	 * Collect and convert arguments
	 */
	nargs = (sig->param_count * 16) + sig->hasthis + vretaddr + call->rgctx_reg + call->imt_arg_reg + call->cinfo->dummy_arg + 1;
	len = sizeof (LLVMValueRef) * nargs;
	args = g_newa (LLVMValueRef, nargs);
	memset (args, 0, len);
	l = call->out_ireg_args;

	if (call->rgctx_arg_reg) {
		g_assert (values [call->rgctx_arg_reg]);
		g_assert (cinfo->rgctx_arg_pindex < nargs);
		/*
		 * On ARM, the imt/rgctx argument is passed in a caller save register, but some of our trampolines etc. clobber it, leading to
		 * problems is LLVM moves the arg assignment earlier. To work around this, save the argument into a stack slot and load
		 * it using a volatile load.
		 */
#ifdef TARGET_ARM
		if (!ctx->imt_rgctx_loc)
			ctx->imt_rgctx_loc = build_alloca_llvm_type (ctx, ctx->module->ptr_type, TARGET_SIZEOF_VOID_P);
		LLVMBuildStore (llvm::wrap (builder), convert (ctx, ctx->values [call->rgctx_arg_reg], ctx->module->ptr_type), ctx->imt_rgctx_loc);
		args [cinfo->rgctx_arg_pindex] = mono_llvm_build_load (llvm::wrap (builder), ctx->module->ptr_type, ctx->imt_rgctx_loc, "", TRUE);
#else
		args [cinfo->rgctx_arg_pindex] = convert (ctx, values [call->rgctx_arg_reg], ctx->module->ptr_type);
#endif
	}
	if (call->imt_arg_reg) {
		g_assert (values [call->imt_arg_reg]);
		g_assert (cinfo->imt_arg_pindex < nargs);
#ifdef TARGET_ARM
		if (!ctx->imt_rgctx_loc)
			ctx->imt_rgctx_loc = build_alloca_llvm_type (ctx, ctx->module->ptr_type, TARGET_SIZEOF_VOID_P);
		LLVMBuildStore (llvm::wrap (builder), convert (ctx, ctx->values [call->imt_arg_reg], ctx->module->ptr_type), ctx->imt_rgctx_loc);
		args [cinfo->imt_arg_pindex] = mono_llvm_build_load (llvm::wrap (builder), ctx->module->ptr_type, ctx->imt_rgctx_loc, "", TRUE);
#else
		args [cinfo->imt_arg_pindex] = convert (ctx, values [call->imt_arg_reg], ctx->module->ptr_type);
#endif
	}
	switch (cinfo->ret.storage) {
	case LLVMArgGsharedvtVariable: {
		MonoInst *var = get_vreg_to_inst (cfg, call->inst.dreg);

		if (var && var->opcode == OP_GSHAREDVT_LOCAL) {
			args [cinfo->vret_arg_pindex] = convert (ctx, emit_gsharedvt_ldaddr (ctx, var->dreg), IntPtrType ());
		} else {
			g_assert (addresses [call->inst.dreg]);
			args [cinfo->vret_arg_pindex] = convert (ctx, addresses [call->inst.dreg]->value, IntPtrType ());
		}
		break;
	}
	default:
		if (vretaddr) {
			if (!addresses [call->inst.dreg])
				addresses [call->inst.dreg] = build_alloca_address (ctx, sig->ret);
			g_assert (cinfo->vret_arg_pindex < nargs);
			if (cinfo->ret.storage == LLVMArgVtypeByRef)
				args [cinfo->vret_arg_pindex] = addresses [call->inst.dreg]->value;
			else
				args [cinfo->vret_arg_pindex] = LLVMBuildPtrToInt (llvm::wrap (builder), addresses [call->inst.dreg]->value, IntPtrType (), "");
		}
		break;
	}

	/*
	 * Sometimes the same method is called with two different signatures (i.e. with and without 'this'), so
	 * use the real callee for argument type conversion.
	 */
	/* Opaque pointers: the callee pointer does not carry its function type;
	 * use the signature we already built for this call site (donor pattern). */
	LLVMTypeRef callee_type = llvm_sig;
	LLVMTypeRef *param_types = static_cast<LLVMTypeRef*>(g_alloca (sizeof (LLVMTypeRef) * LLVMCountParamTypes (callee_type)));
	LLVMGetParamTypes (callee_type, param_types);

	for (i = 0; i < sig->param_count + sig->hasthis; ++i) {
		guint32 regpair;
		int reg, pindex;
		LLVMArgInfo *ainfo = &call->cinfo->args [i];

		pindex = ainfo->pindex;

		regpair = static_cast<guint32>(reinterpret_cast<gssize>(l->data));
		reg = regpair & 0xffffff;
		args [pindex] = values [reg];
		switch (ainfo->storage) {
		case LLVMArgVtypeInReg:
		case LLVMArgAsFpArgs: {
			guint32 nargs;
			int j;

			for (j = 0; j < ainfo->ndummy_fpargs; ++j)
				args [pindex + j] = llvm::wrap (llvm::Constant::getNullValue (llvm::Type::getDoubleTy (ctx->llvm_ctx ())));
			pindex += ainfo->ndummy_fpargs;

			g_assert (addresses [reg]);
			emit_vtype_to_args (ctx, builder, ainfo->type, addresses [reg]->value, ainfo, args + pindex, &nargs);
			pindex += nargs;

			// FIXME: alignment
			// FIXME: Get rid of the VMOVE
			break;
		}
		case LLVMArgVtypeByVal:
			g_assert (addresses [reg]);
			args [pindex] = addresses [reg]->value;
			break;
		case LLVMArgVtypeAddr :
		case LLVMArgVtypeByRef: {
			g_assert (addresses [reg]);
			args [pindex] = convert (ctx, addresses [reg]->value, llvm::wrap (llvm::PointerType::get (ctx->llvm_ctx (), 0)));
			break;
		}
		case LLVMArgAsIArgs:
			g_assert (addresses [reg]);
			if (ainfo->esize == 8)
				args [pindex] = LLVMBuildLoad2 (llvm::wrap (ctx->builder), LLVMArrayType (llvm::wrap (llvm::Type::getInt64Ty (ctx->llvm_ctx ())), ainfo->nslots), convert (ctx, addresses [reg]->value, llvm::wrap (llvm::PointerType::get (ctx->llvm_ctx (), 0))), "");
			else
				args [pindex] = LLVMBuildLoad2 (llvm::wrap (ctx->builder), LLVMArrayType (IntPtrType (), ainfo->nslots), convert (ctx, addresses [reg]->value, llvm::wrap (llvm::PointerType::get (ctx->llvm_ctx (), 0))), "");
			break;
		case LLVMArgVtypeAsScalar:
			g_assert_not_reached ();
			break;
		case LLVMArgGsharedvtFixed:
		case LLVMArgGsharedvtFixedVtype:
			g_assert (addresses [reg]);
			args [pindex] = convert (ctx, addresses [reg]->value, llvm::wrap (llvm::PointerType::get (ctx->llvm_ctx (), 0)));
			break;
		case LLVMArgGsharedvtVariable:
			g_assert (addresses [reg]);
			args [pindex] = convert (ctx, addresses [reg]->value, llvm::wrap (llvm::PointerType::get (ctx->llvm_ctx (), 0)));
			break;
		default:
			g_assert (args [pindex]);
			if (i == 0 && sig->hasthis)
				args [pindex] = convert (ctx, args [pindex], param_types [pindex]);
			else
				args [pindex] = convert (ctx, args [pindex], type_to_llvm_arg_type (ctx, ainfo->type));
			break;
		}
		g_assert (pindex <= nargs);

		l = l->next;
	}

	if (call->cinfo->dummy_arg) {
		g_assert (call->cinfo->dummy_arg_pindex < nargs);
		args [call->cinfo->dummy_arg_pindex] = llvm::wrap (llvm::Constant::getNullValue (llvm::unwrap (ctx->module->ptr_type)));
	}

	// FIXME: Align call sites

	/*
	 * Emit the call
	 */
	lcall = emit_call (ctx, bb, &builder, llvm_sig, callee, args, LLVMCountParamTypes (llvm_sig));

	// If we just allocated an object, it's not null.
	if (call->method && call->method->wrapper_type == MONO_WRAPPER_ALLOC) {
		mono_llvm_set_call_nonnull_ret (lcall);
	}

	if (ins->opcode != OP_TAILCALL && ins->opcode != OP_TAILCALL_MEMBASE && LLVMGetInstructionOpcode (lcall) == LLVMCall)
		mono_llvm_set_call_notailcall (lcall);

	// Add original method name we are currently emitting as a custom string metadata (the only way to leave comments in LLVM IR)
	if (mono_debug_enabled () && call && call->method)
		mono_llvm_add_string_metadata (lcall, "managed_name", mono_method_full_name (call->method, TRUE));

	// As per the LLVM docs, a function has a noalias return value if and only if
	// it is an allocation function. This is an allocation function.
	if (call->method && call->method->wrapper_type == MONO_WRAPPER_ALLOC) {
		mono_llvm_set_call_noalias_ret (lcall);
		// All objects are expected to be 8-byte aligned (SGEN_ALLOC_ALIGN)
		mono_llvm_set_alignment_ret (lcall, 8);
	}

	/*
	 * Modify cconv and parameter attributes to pass rgctx/imt correctly.
	 */
#if defined(MONO_ARCH_IMT_REG) && defined(MONO_ARCH_RGCTX_REG)
	g_assert (MONO_ARCH_IMT_REG == MONO_ARCH_RGCTX_REG);
#endif
	/*
	 * The two can't be used together, so only one of them is passed.
	 * They travel in the 'nest' parameter (tagged LLVM_ATTR_NEST below) under
	 * the default C calling convention: stock LLVM pins 'nest' to R10 on SysV,
	 * which is mono's MONO_ARCH_RGCTX_REG / MONO_ARCH_IMT_REG on amd64.
	 */
	g_assert (!(call->rgctx_arg_reg && call->imt_arg_reg));

	if (cinfo->ret.storage == LLVMArgVtypeByRef)
		mono_llvm_add_instr_attr_with_type (lcall, 1 + cinfo->vret_arg_pindex, LLVM_ATTR_STRUCT_RET, type_to_llvm_type (ctx, sig->ret));
	if (call->rgctx_arg_reg)
		mono_llvm_add_instr_attr (lcall, 1 + cinfo->rgctx_arg_pindex, LLVM_ATTR_NEST);
	if (call->imt_arg_reg)
		mono_llvm_add_instr_attr (lcall, 1 + cinfo->imt_arg_pindex, LLVM_ATTR_NEST);

	/* Add byval attributes if needed */
	for (i = 0; i < sig->param_count; ++i) {
		LLVMArgInfo *ainfo = &call->cinfo->args [i + sig->hasthis];

		if (ainfo && ainfo->storage == LLVMArgVtypeByVal)
			mono_llvm_add_instr_attr_with_type (lcall, 1 + ainfo->pindex, LLVM_ATTR_BY_VAL, type_to_llvm_arg_type (ctx, ainfo->type));
	}

	/*
	 * Convert the result
	 */
	switch (cinfo->ret.storage) {
	case LLVMArgVtypeInReg: {
		LLVMValueRef regs [2];

		if (LLVMTypeOf (lcall) == llvm::wrap (llvm::Type::getVoidTy (ctx->llvm_ctx ())))
			/* Empty struct */
			break;

		if (!addresses [ins->dreg])
			addresses [ins->dreg] = build_alloca_address (ctx, sig->ret);

		regs [0] = LLVMBuildExtractValue (llvm::wrap (builder), lcall, 0, "");
		if (cinfo->ret.pair_storage [1] != LLVMArgNone)
			regs [1] = LLVMBuildExtractValue (llvm::wrap (builder), lcall, 1, "");
		emit_args_to_vtype (ctx, builder, sig->ret, addresses [ins->dreg]->value, &cinfo->ret, regs);
		break;
	}
	case LLVMArgVtypeByVal:
		if (!addresses [call->inst.dreg])
			addresses [call->inst.dreg] = build_alloca_address (ctx, sig->ret);
		LLVMBuildStore (llvm::wrap (builder), lcall, addresses [call->inst.dreg]->value);
		break;
	case LLVMArgAsIArgs:
	case LLVMArgFpStruct:
		if (!addresses [call->inst.dreg])
			addresses [call->inst.dreg] = build_alloca_address (ctx, sig->ret);
		LLVMBuildStore (llvm::wrap (builder), lcall, convert_full (ctx, addresses [call->inst.dreg]->value, llvm::wrap (llvm::PointerType::get (ctx->llvm_ctx (), 0)), FALSE));
		break;
	case LLVMArgVtypeAsScalar:
		if (!addresses [call->inst.dreg])
			addresses [call->inst.dreg] = build_alloca_address (ctx, sig->ret);
		LLVMBuildStore (llvm::wrap (builder), lcall, convert_full (ctx, addresses [call->inst.dreg]->value, llvm::wrap (llvm::PointerType::get (ctx->llvm_ctx (), 0)), FALSE));
		if (MONO_CLASS_IS_SIMD (ctx->cfg, mono_class_from_mono_type_internal (sig->ret)))
			values [ins->dreg] = LLVMBuildBitCast(llvm::wrap (builder), lcall, type_to_llvm_type (ctx, sig->ret), "callret_cvt_simd");
		break;
	case LLVMArgVtypeRetAddr:
	case LLVMArgVtypeByRef:
		if (MONO_CLASS_IS_SIMD (ctx->cfg, mono_class_from_mono_type_internal (sig->ret))) {
			/* Some opcodes like STOREX_MEMBASE access these by value */
			g_assert (addresses [call->inst.dreg]);
			values [ins->dreg] = LLVMBuildLoad2 (llvm::wrap (builder), type_to_llvm_type (ctx, sig->ret), convert_full (ctx, addresses [call->inst.dreg]->value, llvm::wrap (llvm::PointerType::get (ctx->llvm_ctx (), 0)), FALSE), "");
		}
		break;
	case LLVMArgGsharedvtVariable:
		break;
	case LLVMArgGsharedvtFixed:
	case LLVMArgGsharedvtFixedVtype:
		values [ins->dreg] = LLVMBuildLoad2 (llvm::wrap (builder), type_to_llvm_type (ctx, sig->ret), convert_full (ctx, addresses [call->inst.dreg]->value, llvm::wrap (llvm::PointerType::get (ctx->llvm_ctx (), 0)), FALSE), "");
		break;
	default:
		if (sig->ret->type != MONO_TYPE_VOID)
			/* If the method returns an unsigned value, need to zext it */
			values [ins->dreg] = convert_full (ctx, lcall, llvm_type_to_stack_type (cfg, type_to_llvm_type (ctx, sig->ret)), type_is_unsigned (ctx, sig->ret));
		break;
	}

	*builder_ref = ctx->builder;
}

void
emit_throw (EmitContext *ctx, MonoBasicBlock *bb, gboolean rethrow, LLVMValueRef exc)
{
	MonoMethodSignature *throw_sig;

	LLVMValueRef * const pcallee = rethrow ? &ctx->module->rethrow : &ctx->module->throw_icall;
	LLVMValueRef callee = *pcallee;
	char const * const icall_name = rethrow ? "mono_arch_rethrow_exception" : "mono_arch_throw_exception";
#ifndef TARGET_X86
	const
#endif
	MonoJitICallId icall_id = rethrow ? MONO_JIT_ICALL_mono_arch_rethrow_exception  : MONO_JIT_ICALL_mono_arch_throw_exception;

	/*
	 * emit_call () needs the signature the callee was declared with, and the
	 * callee is cached on the module -- never derive it from the callee value
	 * itself (a cached Function's type need not match this call site). The type
	 * is cached too; see MonoLLVMModule::throw_sig_type.
	 */
	if (!ctx->module->throw_sig_type) {
		throw_sig = mono_metadata_signature_alloc (mono_get_corlib (), 1);
		throw_sig->ret = m_class_get_byval_arg (mono_get_void_class ());
		throw_sig->params [0] = m_class_get_byval_arg (mono_get_object_class ());
		ctx->module->throw_sig_type = sig_to_llvm_sig (ctx, throw_sig);
	}
	LLVMTypeRef sig = ctx->module->throw_sig_type;

	if (!callee) {
		{
#ifdef TARGET_X86
			/*
			 * LLVM doesn't push the exception argument, so we need a different
			 * trampoline.
			 */
			icall_id =  rethrow ? MONO_JIT_ICALL_mono_llvm_rethrow_exception_trampoline : MONO_JIT_ICALL_mono_llvm_throw_exception_trampoline;
#endif
			callee = get_jit_callee (ctx, icall_name, sig, MONO_PATCH_INFO_JIT_ICALL_ID, GUINT_TO_POINTER (icall_id));
		}

		mono_memory_barrier ();
	}
	LLVMValueRef arg;
	arg = convert (ctx, exc, type_to_llvm_type (ctx, m_class_get_byval_arg (mono_get_object_class ())));
	emit_call (ctx, bb, &ctx->builder, sig, callee, &arg, 1);
}

LLVMValueRef
create_const_vector (LLVMTypeRef t, const int *vals, int count)
{
	g_assert (count <= 16);
	LLVMValueRef llvm_vals [16];
	for (int i = 0; i < count; i++)
		llvm_vals [i] = llvm::wrap (llvm::ConstantInt::get (llvm::unwrap (t), vals [i], false));
	return const_vector (llvm_vals, count);
}

LLVMValueRef
create_const_vector_i32 (const int *mask, int count)
{
	return create_const_vector (llvm::wrap (llvm::Type::getInt32Ty (llvm_global_ctx ())), mask, count);
}

LLVMValueRef
create_const_vector_4_i32 (int v0, int v1, int v2, int v3)
{
	LLVMValueRef mask [4];
	mask [0] = llvm::wrap (llvm::ConstantInt::get (llvm::Type::getInt32Ty (llvm_global_ctx ()), v0, false));
	mask [1] = llvm::wrap (llvm::ConstantInt::get (llvm::Type::getInt32Ty (llvm_global_ctx ()), v1, false));
	mask [2] = llvm::wrap (llvm::ConstantInt::get (llvm::Type::getInt32Ty (llvm_global_ctx ()), v2, false));
	mask [3] = llvm::wrap (llvm::ConstantInt::get (llvm::Type::getInt32Ty (llvm_global_ctx ()), v3, false));
	return const_vector (mask, 4);
}

LLVMValueRef
create_const_vector_2_i32 (int v0, int v1)
{
	LLVMValueRef mask [2];
	mask [0] = llvm::wrap (llvm::ConstantInt::get (llvm::Type::getInt32Ty (llvm_global_ctx ()), v0, false));
	mask [1] = llvm::wrap (llvm::ConstantInt::get (llvm::Type::getInt32Ty (llvm_global_ctx ()), v1, false));
	return const_vector (mask, 2);
}

/*
 * get_mono_personality:
 *
 *   Return the current function's landingpad personality, creating it the first
 * time and pinning it onto ctx->lmethod via LLVMSetPersonalityFn.
 *
 * The LLVM verifier rejects a `landingpad` whose enclosing function carries no
 * personality, and the no-asserts backend segfaults on it (doc 09 R1). The C
 * API's LLVMBuildLandingPad PersFn operand is ignored on LLVM 18, so the
 * personality has to be set on the FUNCTION with LLVMSetPersonalityFn. A method
 * with several catch clauses calls emit_handler_start once per handler, so the
 * `mono_personality` stub is defined - and the personality fn set - exactly once
 * per method (cached on the context) instead of being duplicated per handler.
 */
static LLVMValueRef
get_mono_personality (EmitContext *ctx)
{
	if (ctx->personality)
		return ctx->personality;

	/* Can't cache across methods as each method is in its own llvm module */
	LLVMTypeRef personality_type = LLVMFunctionType (llvm::wrap (llvm::Type::getInt32Ty (ctx->llvm_ctx ())), NULL, 0, TRUE);
	LLVMValueRef personality = LLVMAddFunction (ctx->lmodule, "mono_personality", personality_type);
	mono_llvm_add_func_attr (personality, LLVM_ATTR_NO_UNWIND);
	LLVMBasicBlockRef entry_bb = LLVMAppendBasicBlock (personality, "ENTRY");
	llvm::IRBuilder<> builder2_storage (llvm_global_ctx ());
	llvm::IRBuilder<> *builder2 = &builder2_storage;
	builder2->SetInsertPoint (llvm::unwrap (entry_bb));
	LLVMBuildRet (llvm::wrap (builder2), llvm::wrap (llvm::ConstantInt::get (llvm::Type::getInt32Ty (ctx->llvm_ctx ()), 0, false)));

	LLVMSetPersonalityFn (ctx->lmethod, personality);

	ctx->personality = personality;
	return personality;
}

void
emit_handler_start (EmitContext *ctx, MonoBasicBlock *bb, llvm::IRBuilder<> *builder)
{
	MonoCompile *cfg = ctx->cfg;
	LLVMValueRef *values = ctx->values;
	LLVMModuleRef lmodule = ctx->lmodule;
	BBInfo *bblocks = ctx->bblocks;
	LLVMTypeRef i8ptr;
	LLVMValueRef personality;
	LLVMBasicBlockRef target_bb;
	MonoInst *exvar;
	static int ti_generator;
	char ti_name [128];
	int clause_index;

	// <resultval> = landingpad <somety> personality <type> <pers_fn> <clause>+

	clause_index = (mono_get_block_region_notry (cfg, bb->region) >> 8) - 1;

	target_bb = bblocks [bb->block_num].call_handler_target_bb;
	g_assert (target_bb);

	/*
	 * A handler is entered through the ONE landing pad of its sibling group -
	 * the invoke target - which the runtime jumps to with the matched clause's
	 * index in the selector register (RDX, doc 11 6.4). Sibling catches over the
	 * IDENTICAL try region - try { } catch(A) catch(B) - share that one landing
	 * pad; only its clause is the invoke target, the others are reached through
	 * its selector switch and carry no landing pad of their own.
	 */
	if (bblocks [bb->block_num].invoke_target) {
		MonoExceptionClause *this_clause = &cfg->header->clauses [clause_index];
		LLVMValueRef landing_pad;
		LLVMValueRef switch_ins;
		int i;

		personality = get_mono_personality (ctx);
		i8ptr = llvm::wrap (llvm::PointerType::get (ctx->llvm_ctx (), 0));

		{
			LLVMTypeRef members [2], ret_type;

			members [0] = i8ptr;
			members [1] = llvm::wrap (llvm::Type::getInt32Ty (ctx->llvm_ctx ()));
			ret_type = LLVMStructType (members, 2, FALSE);

			landing_pad = LLVMBuildLandingPad (llvm::wrap (builder), ret_type, personality, cfg->header->num_clauses, "");
		}

		/*
		 * Carry one landingpad clause per catch that shares this try region, in
		 * ascending IL clause_index order. Each is a type_info_N global whose i32
		 * initializer smuggles the IL clause_index; the gather pass reads them
		 * (one .mono_lsda entry per catch over the shared invoke range) and the
		 * runtime picks the handler by isinst in that order, so declaration order
		 * (inner/more-derived catch first) is preserved. A single catch adds just
		 * its own clause; sibling catches add the whole group.
		 */
		for (i = 0; i < cfg->header->num_clauses; ++i) {
			MonoExceptionClause *c = &cfg->header->clauses [i];
			LLVMValueRef type_info;

			if (c->flags != MONO_EXCEPTION_CLAUSE_NONE)
				continue;
			if (c->try_offset != this_clause->try_offset || c->try_len != this_clause->try_len)
				continue;

			sprintf (ti_name, "type_info_%d", ti_generator);
			ti_generator ++;

			type_info = LLVMAddGlobal (lmodule, llvm::wrap (llvm::Type::getInt32Ty (ctx->llvm_ctx ())), ti_name);
			LLVMSetInitializer (type_info, llvm::wrap (llvm::ConstantInt::get (llvm::Type::getInt32Ty (ctx->llvm_ctx ()), i, false)));
			LLVMAddClause (landing_pad, type_info);
		}

		/* Store the exception into the exvar */
		if (ctx->ex_var)
			LLVMBuildStore (llvm::wrap (builder), convert (ctx, LLVMBuildExtractValue (llvm::wrap (builder), landing_pad, 0, "ex_obj"), ObjRefType ()), ctx->ex_var);

		/*
		 * The selector register holds the matched clause's index; branch to the
		 * sibling handler that owns it (this clause's own body is the default).
		 */
		LLVMValueRef ex_selector = LLVMBuildExtractValue (llvm::wrap (builder), landing_pad, 1, "ex_selector");
		switch_ins = LLVMBuildSwitch (llvm::wrap (builder), ex_selector, target_bb, 0);

		for (i = 0; i < cfg->header->num_clauses; ++i) {
			MonoExceptionClause *c = &cfg->header->clauses [i];
			MonoBasicBlock *handler_bb;

			if (i == clause_index)
				continue;
			if (c->flags != MONO_EXCEPTION_CLAUSE_NONE)
				continue;
			if (c->try_offset != this_clause->try_offset || c->try_len != this_clause->try_len)
				continue;

			auto clause_it = ctx->clause_to_handler.find (i);
			handler_bb = clause_it != ctx->clause_to_handler.end () ? clause_it->second : nullptr;
			g_assert (handler_bb);
			g_assert (ctx->bblocks [handler_bb->block_num].call_handler_target_bb);
			LLVMAddCase (switch_ins, llvm::wrap (llvm::ConstantInt::get (llvm::Type::getInt32Ty (ctx->llvm_ctx ()), i, false)), ctx->bblocks [handler_bb->block_num].call_handler_target_bb);
		}
	} else {
		/*
		 * A secondary sibling has no landing pad of its own - it is reached only
		 * through the group's invoke-target landing pad selector switch, which
		 * branches straight to this clause's call_handler_target_bb. Its own EH
		 * entry block is never an unwind destination, so terminate it as
		 * unreachable to keep the IR well-formed.
		 */
		LLVMBuildUnreachable (llvm::wrap (builder));
	}

	/* Start a new bblock which CALL_HANDLER can branch to */
	ctx->builder = builder = create_builder (ctx);
	ctx->builder->SetInsertPoint (llvm::unwrap (target_bb));

	ctx->bblocks [bb->block_num].end_bblock = target_bb;

	/* Store the exception into the IL level exvar */
	if (bb->in_scount == 1) {
		g_assert (bb->in_scount == 1);
		exvar = bb->in_stack [0];

		// FIXME: This is shared with filter clauses ?
		g_assert (!values [exvar->dreg]);

		g_assert (ctx->ex_var);
		values [exvar->dreg] = LLVMBuildLoad2 (llvm::wrap (builder), ObjRefType (), ctx->ex_var, "");
		emit_volatile_store (ctx, exvar->dreg);
	}

	/* Make normal branches to the start of the clause branch to the new bblock */
	bblocks [bb->block_num].bblock = target_bb;
}

//Wasm requires us to canonicalize NaNs.
LLVMValueRef
get_double_const (MonoCompile *cfg, double val)
{
#ifdef TARGET_WASM
	if (mono_isnan (val))
		*reinterpret_cast<gint64 *>(&val) = 0x7FF8000000000000ll;
#endif
	return llvm::wrap (llvm::ConstantFP::get (llvm::Type::getDoubleTy (llvm_global_ctx ()), val));
}

LLVMValueRef
get_float_const (MonoCompile *cfg, float val)
{
#ifdef TARGET_WASM
	if (mono_isnan (val))
		*reinterpret_cast<int *>(&val) = 0x7FC00000;
#endif
	if (cfg->r4fp)
		return llvm::wrap (llvm::ConstantFP::get (llvm::Type::getFloatTy (llvm_global_ctx ()), val));
	else
		/* LLVM 18 removed the FPExt const-expr; val is already a float, so this
		 * double constant is exactly the extension of the float constant. */
		return llvm::wrap (llvm::ConstantFP::get (llvm::Type::getDoubleTy (llvm_global_ctx ()), val));
}

LLVMValueRef
call_intrins (EmitContext *ctx, int id, LLVMValueRef *args, const char *name)
{
	LLVMValueRef intrins = get_intrins (ctx, id);
	int nargs = LLVMCountParamTypes (LLVMGlobalGetValueType (intrins));

	for (int i = 0; i < nargs; ++i) {
		LLVMTypeRef t1 = LLVMTypeOf (args [i]);
		LLVMTypeRef t2 = LLVMTypeOf (LLVMGetParam (intrins, i));
		if (t1 != t2)
			args [i] = convert (ctx, args [i], t2);
	}

	return LLVMBuildCall2 (llvm::wrap (ctx->builder), LLVMGlobalGetValueType (intrins), intrins, args, nargs, name);
}


#endif /* DISABLE_JIT */

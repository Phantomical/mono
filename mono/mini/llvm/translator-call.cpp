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
emit_div_check (EmitContext *ctx, llvm::IRBuilder<> *builder, MonoBasicBlock *bb, MonoInst *ins, llvm::Value *lhs, llvm::Value *rhs)
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

		cmp = llvm::wrap (builder->CreateICmp (to_llvm_pred (LLVMIntEQ), rhs, llvm::ConstantInt::get (rhs->getType (), 0, false), ""));
		emit_cond_system_exception (ctx, bb, "DivideByZeroException", cmp, FALSE);
		if (!ctx->ok ())
			break;
		builder = ctx->builder;

		/* b == -1 && a == 0x80000000 */
		if (is_signed) {
			llvm::Value *c = (lhs->getType () == llvm::Type::getInt32Ty (ctx->llvm_ctx ())) ? llvm::ConstantInt::get (lhs->getType (), 0x80000000, false) : llvm::ConstantInt::get (lhs->getType (), 0x8000000000000000LL, false);
			llvm::Value *cond1 = builder->CreateICmp (to_llvm_pred (LLVMIntEQ), rhs, llvm::ConstantInt::get (rhs->getType (), -1, false), "");
			llvm::Value *cond2 = builder->CreateICmp (to_llvm_pred (LLVMIntEQ), lhs, c, "");

			cmp = llvm::wrap (builder->CreateICmp (to_llvm_pred (LLVMIntEQ), builder->CreateAnd (cond1, cond2, ""), llvm::ConstantInt::get (llvm::Type::getInt1Ty (ctx->llvm_ctx ()), 1, false), ""));
			emit_cond_system_exception (ctx, bb, "OverflowException", cmp, FALSE);
			if (!ctx->ok ())
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
	llvm::wrap (builder->CreateCall (llvm::cast<llvm::FunctionType> (llvm::unwrap (sm_type)), llvm::unwrap (sm), gep_index_list (args, 3), ""));
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
			vtype = ctx->type_to_llvm_type (var->inst_vtype);
			if (!ctx->ok ())
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

			emit_args_to_vtype (ctx, builder, ainfo->type, llvm::wrap (ctx->addresses [reg]->value), ainfo, args);
			break;
		}
		case LLVMArgVtypeByVal: {
			/* Element type must match the declaration in sig_to_llvm_sig_full () */
			ctx->addresses [reg] = create_address (ctx, LLVMGetParam (ctx->lmethod, pindex), ctx->type_to_llvm_arg_type (ainfo->type));
			break;
		}
		case LLVMArgVtypeAddr:
		case LLVMArgVtypeByRef: {
			/* The argument is passed by ref */
			/* Element type must match the declaration in sig_to_llvm_sig_full () */
			ctx->addresses [reg] = create_address (ctx, LLVMGetParam (ctx->lmethod, pindex), ctx->type_to_llvm_arg_type (ainfo->type));
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
				llvm::Value *val = builder->CreateExtractValue (llvm::unwrap (arg), {0}, "");
				llvm::Value *dest = ctx->convert (ctx->addresses [reg]->value, llvm::PointerType::get (ctx->llvm_ctx (), 0));
				llvm::wrap (ctx->builder->CreateStore (builder->CreateTrunc (val, llvm::Type::getIntNTy (ctx->llvm_ctx (), size * 8), ""), dest));
			} else {
				llvm::wrap (ctx->builder->CreateStore (llvm::unwrap (arg), ctx->convert (ctx->addresses [reg]->value, llvm::PointerType::get (ctx->llvm_ctx (), 0))));
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

			ctx->values [reg] = builder->CreateLoad (llvm::unwrap (ctx->type_to_llvm_type (ainfo->type)), ctx->convert (llvm::unwrap (arg), llvm::PointerType::get (ctx->llvm_ctx (), 0)), name);
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
			LLVMSetValueName (llvm::wrap (ctx->addresses [reg]->value), name);
			llvm::wrap (builder->CreateStore (builder->CreateLoad (llvm::unwrap (ctx->type_to_llvm_type (ainfo->type)), ctx->convert (llvm::unwrap (arg), llvm::PointerType::get (ctx->llvm_ctx (), 0)), ""), ctx->addresses [reg]->value));
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
				t = ctx->type_to_llvm_type (ainfo->type);
			ctx->values [reg] = ctx->convert_full (ctx->values [reg], llvm::unwrap (llvm_type_to_stack_type (cfg, t)), ctx->type_is_unsigned (ainfo->type));
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
				ctx->values [reg] = builder->CreateLoad (ctx->addresses [reg]->type, ctx->addresses [reg]->value, "simd_vtype");
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
			ctx->values [reg] = ctx->convert (ctx->values [reg], ctx->vreg_types [reg]);
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
		mono_llvm_build_store (llvm::wrap (builder), llvm::wrap (ctx->values [cfg->args [0]->dreg]), this_alloc, TRUE, LLVM_BARRIER_NONE);

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
				mono_llvm_build_store (llvm::wrap (builder), llvm::wrap (ctx->convert (ctx->rgctx_arg, llvm::unwrap (IntPtrType ()))), rgctx_slot, TRUE, LLVM_BARRIER_NONE);
				set_metadata_flag (rgctx_slot, "mono.this");
				emit_this_slot_stackmap (ctx, builder, rgctx_slot);
			}
		} else {
			LLVMValueRef rgctx_alloc, store;

			/*
			 * We handle the rgctx arg similarly to the this pointer.
			 */
			g_assert (ctx->addresses [cfg->rgctx_var->dreg]);
			rgctx_alloc = llvm::wrap (ctx->addresses [cfg->rgctx_var->dreg]->value);
			/* This volatile store will keep the alloca alive */
			store = mono_llvm_build_store (llvm::wrap (builder), llvm::wrap (ctx->convert (ctx->rgctx_arg, llvm::unwrap (IntPtrType ()))), rgctx_alloc, TRUE, LLVM_BARRIER_NONE);

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
			llvm::Value *val;

			sprintf (name, "finally_ind_bb%d", bb->block_num);
			val = builder->CreateAlloca (llvm::Type::getInt32Ty (ctx->llvm_ctx ()), nullptr, name);
			builder->CreateStore (llvm::ConstantInt::get (llvm::Type::getInt32Ty (ctx->llvm_ctx ()), 0, false), val);

			ctx->bblocks [bb->block_num].finally_ind = val;
		} else {
			/* Create a variable to hold the exception var */
			if (!ctx->ex_var)
				ctx->ex_var = builder->CreateAlloca (llvm::unwrap (ObjRefType ()), nullptr, "exvar");
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
	llvm::Value **values = ctx->values;
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
		ctx->set_failure ("non-default callconv");
		return;
	}

	cinfo = call->cinfo;
	g_assert (cinfo);
	if (call->rgctx_arg_reg)
		cinfo->rgctx_arg = TRUE;
	if (call->imt_arg_reg)
		cinfo->imt_arg = TRUE;
	vretaddr = (cinfo->ret.storage == LLVMArgVtypeRetAddr || cinfo->ret.storage == LLVMArgVtypeByRef || cinfo->ret.storage == LLVMArgGsharedvtFixed || cinfo->ret.storage == LLVMArgGsharedvtVariable || cinfo->ret.storage == LLVMArgGsharedvtFixedVtype);

	llvm_sig = ctx->sig_to_llvm_sig_full (sig, cinfo);
	if (!ctx->ok ())
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
				llvm::Value *tramp_var = jit_callee_it != ctx->jit_callees.end () ? jit_callee_it->second : nullptr;
				if (!tramp_var) {
					target =
						mono_create_jit_trampoline (mono_domain_get (),
													call->method, error);
					if (!is_ok (error)) {
						ctx->set_failure (mono_error_get_message (error));
						mono_error_cleanup (error);
						return;
					}

					tramp_var = llvm::unwrap (LLVMAddGlobal (ctx->lmodule, llvm::wrap (llvm::PointerType::get (ctx->llvm_ctx (), 0)), name));
					LLVMSetInitializer (llvm::wrap (tramp_var), llvm::wrap (llvm::ConstantExpr::getIntToPtr (llvm::cast<llvm::Constant> (llvm::ConstantInt::get (llvm::Type::getInt64Ty (ctx->llvm_ctx ()), static_cast<guint64>(reinterpret_cast<size_t>(target)), false)), llvm::PointerType::get (ctx->llvm_ctx (), 0))));
					LLVMSetLinkage (llvm::wrap (tramp_var), LLVMExternalLinkage);
					ctx->jit_callees [call->method] = tramp_var;
				}
				callee = llvm::wrap (builder->CreateLoad (llvm::PointerType::get (ctx->llvm_ctx (), 0), tramp_var, ""));
			}
		}

		if (call->method && strstr (m_class_get_name (call->method->klass), "AsyncVoidMethodBuilder")) {
			/* LLVM miscompiles async methods */
			ctx->set_failure ("#13734");
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

		callee = llvm::wrap (ctx->convert (builder->CreateLoad (llvm::PointerType::get (ctx->llvm_ctx (), 0), builder->CreateGEP (llvm::PointerType::get (ctx->llvm_ctx (), 0), ctx->convert (values [ins->inst_basereg], llvm::PointerType::get (ctx->llvm_ctx (), 0)), gep_index_list (&index, 1), ""), ""), llvm::PointerType::get (ctx->llvm_ctx (), 0)));
	} else if (calli) {
		callee = llvm::wrap (ctx->convert (values [ins->sreg1], llvm::PointerType::get (ctx->llvm_ctx (), 0)));
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
			ctx->imt_rgctx_loc = llvm::unwrap (build_alloca_llvm_type (ctx, ctx->module->ptr_type, TARGET_SIZEOF_VOID_P));
		builder->CreateStore (ctx->convert (ctx->values [call->rgctx_arg_reg], llvm::unwrap (ctx->module->ptr_type)), ctx->imt_rgctx_loc);
		args [cinfo->rgctx_arg_pindex] = mono_llvm_build_load (llvm::wrap (builder), ctx->module->ptr_type, llvm::wrap (ctx->imt_rgctx_loc), "", TRUE);
#else
		args [cinfo->rgctx_arg_pindex] = llvm::wrap (ctx->convert (values [call->rgctx_arg_reg], llvm::unwrap (ctx->module->ptr_type)));
#endif
	}
	if (call->imt_arg_reg) {
		g_assert (values [call->imt_arg_reg]);
		g_assert (cinfo->imt_arg_pindex < nargs);
#ifdef TARGET_ARM
		if (!ctx->imt_rgctx_loc)
			ctx->imt_rgctx_loc = llvm::unwrap (build_alloca_llvm_type (ctx, ctx->module->ptr_type, TARGET_SIZEOF_VOID_P));
		builder->CreateStore (ctx->convert (ctx->values [call->imt_arg_reg], llvm::unwrap (ctx->module->ptr_type)), ctx->imt_rgctx_loc);
		args [cinfo->imt_arg_pindex] = mono_llvm_build_load (llvm::wrap (builder), ctx->module->ptr_type, llvm::wrap (ctx->imt_rgctx_loc), "", TRUE);
#else
		args [cinfo->imt_arg_pindex] = llvm::wrap (ctx->convert (values [call->imt_arg_reg], llvm::unwrap (ctx->module->ptr_type)));
#endif
	}
	switch (cinfo->ret.storage) {
	case LLVMArgGsharedvtVariable: {
		MonoInst *var = get_vreg_to_inst (cfg, call->inst.dreg);

		if (var && var->opcode == OP_GSHAREDVT_LOCAL) {
			args [cinfo->vret_arg_pindex] = llvm::wrap (ctx->convert (llvm::unwrap (emit_gsharedvt_ldaddr (ctx, var->dreg)), llvm::unwrap (IntPtrType ())));
		} else {
			g_assert (addresses [call->inst.dreg]);
			args [cinfo->vret_arg_pindex] = llvm::wrap (ctx->convert (addresses [call->inst.dreg]->value, llvm::unwrap (IntPtrType ())));
		}
		break;
	}
	default:
		if (vretaddr) {
			if (!addresses [call->inst.dreg])
				addresses [call->inst.dreg] = build_alloca_address (ctx, sig->ret);
			g_assert (cinfo->vret_arg_pindex < nargs);
			if (cinfo->ret.storage == LLVMArgVtypeByRef)
				args [cinfo->vret_arg_pindex] = llvm::wrap (addresses [call->inst.dreg]->value);
			else
				args [cinfo->vret_arg_pindex] = llvm::wrap (builder->CreatePtrToInt (addresses [call->inst.dreg]->value, llvm::unwrap (IntPtrType ()), ""));
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
		args [pindex] = llvm::wrap (values [reg]);
		switch (ainfo->storage) {
		case LLVMArgVtypeInReg:
		case LLVMArgAsFpArgs: {
			guint32 nargs;
			int j;

			for (j = 0; j < ainfo->ndummy_fpargs; ++j)
				args [pindex + j] = llvm::wrap (llvm::Constant::getNullValue (llvm::Type::getDoubleTy (ctx->llvm_ctx ())));
			pindex += ainfo->ndummy_fpargs;

			g_assert (addresses [reg]);
			emit_vtype_to_args (ctx, builder, ainfo->type, llvm::wrap (addresses [reg]->value), ainfo, args + pindex, &nargs);
			pindex += nargs;

			// FIXME: alignment
			// FIXME: Get rid of the VMOVE
			break;
		}
		case LLVMArgVtypeByVal:
			g_assert (addresses [reg]);
			args [pindex] = llvm::wrap (addresses [reg]->value);
			break;
		case LLVMArgVtypeAddr :
		case LLVMArgVtypeByRef: {
			g_assert (addresses [reg]);
			args [pindex] = llvm::wrap (ctx->convert (addresses [reg]->value, llvm::PointerType::get (ctx->llvm_ctx (), 0)));
			break;
		}
		case LLVMArgAsIArgs:
			g_assert (addresses [reg]);
			if (ainfo->esize == 8)
				args [pindex] = llvm::wrap (ctx->builder->CreateLoad (llvm::unwrap (LLVMArrayType (llvm::wrap (llvm::Type::getInt64Ty (ctx->llvm_ctx ())), ainfo->nslots)), ctx->convert (addresses [reg]->value, llvm::PointerType::get (ctx->llvm_ctx (), 0)), ""));
			else
				args [pindex] = llvm::wrap (ctx->builder->CreateLoad (llvm::unwrap (LLVMArrayType (IntPtrType (), ainfo->nslots)), ctx->convert (addresses [reg]->value, llvm::PointerType::get (ctx->llvm_ctx (), 0)), ""));
			break;
		case LLVMArgVtypeAsScalar:
			g_assert_not_reached ();
			break;
		case LLVMArgGsharedvtFixed:
		case LLVMArgGsharedvtFixedVtype:
			g_assert (addresses [reg]);
			args [pindex] = llvm::wrap (ctx->convert (addresses [reg]->value, llvm::PointerType::get (ctx->llvm_ctx (), 0)));
			break;
		case LLVMArgGsharedvtVariable:
			g_assert (addresses [reg]);
			args [pindex] = llvm::wrap (ctx->convert (addresses [reg]->value, llvm::PointerType::get (ctx->llvm_ctx (), 0)));
			break;
		default:
			g_assert (args [pindex]);
			if (i == 0 && sig->hasthis)
				args [pindex] = llvm::wrap (ctx->convert (llvm::unwrap (args [pindex]), llvm::unwrap (param_types [pindex])));
			else
				args [pindex] = llvm::wrap (ctx->convert (llvm::unwrap (args [pindex]), llvm::unwrap (ctx->type_to_llvm_arg_type (ainfo->type))));
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
		mono_llvm_add_instr_attr_with_type (lcall, 1 + cinfo->vret_arg_pindex, LLVM_ATTR_STRUCT_RET, ctx->type_to_llvm_type (sig->ret));
	if (call->rgctx_arg_reg)
		mono_llvm_add_instr_attr (lcall, 1 + cinfo->rgctx_arg_pindex, LLVM_ATTR_NEST);
	if (call->imt_arg_reg)
		mono_llvm_add_instr_attr (lcall, 1 + cinfo->imt_arg_pindex, LLVM_ATTR_NEST);

	/* Add byval attributes if needed */
	for (i = 0; i < sig->param_count; ++i) {
		LLVMArgInfo *ainfo = &call->cinfo->args [i + sig->hasthis];

		if (ainfo && ainfo->storage == LLVMArgVtypeByVal)
			mono_llvm_add_instr_attr_with_type (lcall, 1 + ainfo->pindex, LLVM_ATTR_BY_VAL, ctx->type_to_llvm_arg_type (ainfo->type));
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

		regs [0] = llvm::wrap (builder->CreateExtractValue (llvm::unwrap (lcall), {0}, ""));
		if (cinfo->ret.pair_storage [1] != LLVMArgNone)
			regs [1] = llvm::wrap (builder->CreateExtractValue (llvm::unwrap (lcall), {1}, ""));
		emit_args_to_vtype (ctx, builder, sig->ret, llvm::wrap (addresses [ins->dreg]->value), &cinfo->ret, regs);
		break;
	}
	case LLVMArgVtypeByVal:
		if (!addresses [call->inst.dreg])
			addresses [call->inst.dreg] = build_alloca_address (ctx, sig->ret);
		llvm::wrap (builder->CreateStore (llvm::unwrap (lcall), addresses [call->inst.dreg]->value));
		break;
	case LLVMArgAsIArgs:
	case LLVMArgFpStruct:
		if (!addresses [call->inst.dreg])
			addresses [call->inst.dreg] = build_alloca_address (ctx, sig->ret);
		llvm::wrap (builder->CreateStore (llvm::unwrap (lcall), ctx->convert_full (addresses [call->inst.dreg]->value, llvm::PointerType::get (ctx->llvm_ctx (), 0), FALSE)));
		break;
	case LLVMArgVtypeAsScalar:
		if (!addresses [call->inst.dreg])
			addresses [call->inst.dreg] = build_alloca_address (ctx, sig->ret);
		llvm::wrap (builder->CreateStore (llvm::unwrap (lcall), ctx->convert_full (addresses [call->inst.dreg]->value, llvm::PointerType::get (ctx->llvm_ctx (), 0), FALSE)));
		if (MONO_CLASS_IS_SIMD (ctx->cfg, mono_class_from_mono_type_internal (sig->ret)))
			values [ins->dreg] = builder->CreateBitCast (llvm::unwrap (lcall), llvm::unwrap (ctx->type_to_llvm_type (sig->ret)), "callret_cvt_simd");
		break;
	case LLVMArgVtypeRetAddr:
	case LLVMArgVtypeByRef:
		if (MONO_CLASS_IS_SIMD (ctx->cfg, mono_class_from_mono_type_internal (sig->ret))) {
			/* Some opcodes like STOREX_MEMBASE access these by value */
			g_assert (addresses [call->inst.dreg]);
			values [ins->dreg] = builder->CreateLoad (llvm::unwrap (ctx->type_to_llvm_type (sig->ret)), ctx->convert_full (addresses [call->inst.dreg]->value, llvm::PointerType::get (ctx->llvm_ctx (), 0), FALSE), "");
		}
		break;
	case LLVMArgGsharedvtVariable:
		break;
	case LLVMArgGsharedvtFixed:
	case LLVMArgGsharedvtFixedVtype:
		values [ins->dreg] = builder->CreateLoad (llvm::unwrap (ctx->type_to_llvm_type (sig->ret)), ctx->convert_full (addresses [call->inst.dreg]->value, llvm::PointerType::get (ctx->llvm_ctx (), 0), FALSE), "");
		break;
	default:
		if (sig->ret->type != MONO_TYPE_VOID)
			/* If the method returns an unsigned value, need to zext it */
			values [ins->dreg] = ctx->convert_full (llvm::unwrap (lcall), llvm::unwrap (llvm_type_to_stack_type (cfg, ctx->type_to_llvm_type (sig->ret))), ctx->type_is_unsigned (sig->ret));
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
		ctx->module->throw_sig_type = ctx->sig_to_llvm_sig (throw_sig);
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
	arg = llvm::wrap (ctx->convert (llvm::unwrap (exc), llvm::unwrap (ctx->type_to_llvm_type (m_class_get_byval_arg (mono_get_object_class ())))));
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
 * personality, and the no-asserts backend segfaults on it (doc 09 R1). A
 * `landingpad` does not carry its own personality, so it has to be set on the
 * FUNCTION with LLVMSetPersonalityFn. A method
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
	llvm::wrap (builder2->CreateRet (llvm::ConstantInt::get (llvm::Type::getInt32Ty (ctx->llvm_ctx ()), 0, false)));

	LLVMSetPersonalityFn (ctx->lmethod, personality);

	ctx->personality = personality;
	return personality;
}

/*
 * clause_encloses:
 *
 *   Does IL clause J strictly ENCLOSE clause C - i.e. is C's try region nested in
 * J's? This is the emission-side mirror of the identically-named predicate in
 * mono_lsda.cpp (EH N1) and of the translator nesting gate's containment test:
 *
 *   c.try_offset >= j.try_offset && c.handler_offset <= j.handler_offset
 *
 * with SIBLINGS (identical try_offset AND try_len - try { } catch(A) catch(B))
 * excluded, since siblings share one landing pad and are routed by the same-range
 * loops, not by nesting. Keeping this byte-for-byte in step with the build side is
 * load-bearing: the switch cases emitted here (case per enclosing clause) must line
 * up 1:1 with the enclosing .mono_lsda entries build_ex_info synthesises for the
 * same clause, or a live nested method (once N3 lifts the gate) would dispatch the
 * wrong handler.
 */
static inline bool
clause_encloses (const MonoExceptionClause *c, const MonoExceptionClause *j)
{
	bool siblings = c->try_offset == j->try_offset && c->try_len == j->try_len;
	return !siblings &&
	       c->try_offset >= j->try_offset &&
	       c->handler_offset <= j->handler_offset;
}

void
emit_handler_start (EmitContext *ctx, MonoBasicBlock *bb, llvm::IRBuilder<> *builder)
{
	MonoCompile *cfg = ctx->cfg;
	llvm::Value **values = ctx->values;
	LLVMModuleRef lmodule = ctx->lmodule;
	BBInfo *bblocks = ctx->bblocks;
	LLVMTypeRef i8ptr;
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
		llvm::SwitchInst *switch_ins;
		int i;

		get_mono_personality (ctx);
		i8ptr = llvm::wrap (llvm::PointerType::get (ctx->llvm_ctx (), 0));

		{
			LLVMTypeRef members [2], ret_type;

			members [0] = i8ptr;
			members [1] = llvm::wrap (llvm::Type::getInt32Ty (ctx->llvm_ctx ()));
			ret_type = LLVMStructType (members, 2, FALSE);

			landing_pad = llvm::wrap (builder->CreateLandingPad (llvm::unwrap (ret_type), cfg->header->num_clauses, ""));
		}

		if (this_clause->flags == MONO_EXCEPTION_CLAUSE_FINALLY ||
		    this_clause->flags == MONO_EXCEPTION_CLAUSE_FAULT) {
			/*
			 * A finally/fault handler IS the invoke target of its (standalone,
			 * non-nested) try region, but - unlike a catch - it receives no
			 * exception object and has no siblings: a try/catch nested inside a
			 * try/finally is declined by the nesting gate, so this handler owns its
			 * landing pad alone. Give the pad just its OWN smuggled type_info clause
			 * - the same 2-word {i32 clause_index, i32 kind} global catch uses, with
			 * kind = this clause's flags (FINALLY == 2 / FAULT == 4) - so the gather
			 * pass records one self-describing .mono_lsda entry for it. Then branch
			 * straight to call_handler_target_bb, which the OP_START_HANDLER /
			 * OP_ENDFINALLY machinery drives on both the exceptional-unwind and the
			 * leave normal-exit paths. No catch-style exception-object store, and no
			 * sibling selector switch.
			 */
			LLVMTypeRef i32_ty = llvm::wrap (llvm::Type::getInt32Ty (ctx->llvm_ctx ()));
			LLVMTypeRef ti_members [2] = { i32_ty, i32_ty };
			LLVMTypeRef ti_type = LLVMStructType (ti_members, 2, FALSE);
			LLVMValueRef ti_init [2];
			LLVMValueRef type_info;

			sprintf (ti_name, "type_info_%d", ti_generator);
			ti_generator ++;

			ti_init [0] = llvm::wrap (llvm::ConstantInt::get (llvm::Type::getInt32Ty (ctx->llvm_ctx ()), clause_index, false));
			ti_init [1] = llvm::wrap (llvm::ConstantInt::get (llvm::Type::getInt32Ty (ctx->llvm_ctx ()), this_clause->flags, false));

			type_info = LLVMAddGlobal (lmodule, ti_type, ti_name);
			LLVMSetInitializer (type_info, LLVMConstNamedStruct (ti_type, ti_init, 2));
			LLVMAddClause (landing_pad, type_info);

			/*
			 * doc 21 5.2 (EH N2) - the finally/fault pad grows a selector switch when
			 * it participates in nesting. DORMANT: the nesting gate declines every
			 * strictly-nested method, so for every admitted shape this clause neither
			 * nests in nor encloses another - is_nested and is_enclosing stay false -
			 * and the straight CreateBr below is emitted BYTE-IDENTICAL to the landed
			 * standalone finally/fault (F1-F6). The switch is added ONLY for the gated-
			 * out nested-or-enclosing case, which is what keeps N2 runtime-inert.
			 */
			bool is_nested = false;
			bool is_enclosing = false;
			for (i = 0; i < cfg->header->num_clauses; ++i) {
				if (clause_encloses (this_clause, &cfg->header->clauses [i]))
					is_nested = true;
				if (clause_encloses (&cfg->header->clauses [i], this_clause))
					is_enclosing = true;
			}

			if (!is_nested && !is_enclosing) {
				llvm::wrap (builder->CreateBr (llvm::unwrap (target_bb)));
			} else {
				/*
				 * A nested finally/fault can be the INNERMOST landing pad a call
				 * unwinds to, so the runtime may deliver here with RDX == an ENCLOSING
				 * clause's index (not this finally's). Route with a selector switch:
				 * default -> this finally's own body; case RDX==j -> encloser j's body.
				 *
				 * Exception-object store (Probe A, safe form). When the encloser j is a
				 * CATCH its body reads ex_var, but - unlike the catch pad - the finally
				 * pad never stored it. On the finally's OWN (default) path the runtime
				 * did NOT set RAX (mini-exceptions.c sets the exc reg only for
				 * NONE/FILTER), so an extractvalue 0 there would read a garbage RAX. We
				 * therefore emit the store ONLY on the catch-routing edge, in a per-case
				 * trampoline block, and NEVER at pad entry - so no garbage RAX can ever
				 * be written into the (possibly GC-scanned) exvar slot on the finally's
				 * own path. Enclosing finally/fault edges take no store and branch
				 * straight to the encloser's body.
				 */
				llvm::Value *ex_selector = builder->CreateExtractValue (llvm::unwrap (landing_pad), {1}, "ex_selector");
				switch_ins = builder->CreateSwitch (ex_selector, llvm::unwrap (target_bb), 0);

				for (i = 0; i < cfg->header->num_clauses; ++i) {
					MonoExceptionClause *enc = &cfg->header->clauses [i];
					MonoBasicBlock *handler_bb;
					LLVMBasicBlockRef case_target;

					if (!clause_encloses (this_clause, enc))
						continue;

					auto clause_it = ctx->clause_to_handler.find (i);
					handler_bb = clause_it != ctx->clause_to_handler.end () ? clause_it->second : nullptr;
					g_assert (handler_bb);
					g_assert (ctx->bblocks [handler_bb->block_num].call_handler_target_bb);
					case_target = ctx->bblocks [handler_bb->block_num].call_handler_target_bb;

					if (enc->flags == MONO_EXCEPTION_CLAUSE_NONE && ctx->ex_var) {
						/*
						 * Enclosing catch: store the exception object on THIS edge only.
						 * builder == ctx->builder (both at the pad block), and convert ()
						 * inserts via ctx->builder, so reposition it into the trampoline,
						 * emit the store, then restore it to the pad block for the rest
						 * of the switch build-out.
						 */
						LLVMBasicBlockRef store_bb = LLVMAppendBasicBlock (ctx->lmethod, "finally_to_catch");
						llvm::BasicBlock *saved_ip = builder->GetInsertBlock ();
						builder->SetInsertPoint (llvm::unwrap (store_bb));
						llvm::Value *ex_obj = builder->CreateExtractValue (llvm::unwrap (landing_pad), {0}, "ex_obj");
						builder->CreateStore (ctx->convert (ex_obj, llvm::unwrap (ObjRefType ())), ctx->ex_var);
						llvm::wrap (builder->CreateBr (llvm::unwrap (case_target)));
						builder->SetInsertPoint (saved_ip);
						case_target = store_bb;
					}

					switch_ins->addCase (llvm::cast<llvm::ConstantInt> (llvm::ConstantInt::get (llvm::Type::getInt32Ty (ctx->llvm_ctx ()), i, false)), llvm::unwrap (case_target));
				}
			}
		} else {
			/*
			 * Carry one landingpad clause per catch that shares this try region, in
			 * ascending IL clause_index order. Each is a type_info_N global whose
			 * 2-word {i32 clause_index, i32 kind} initializer smuggles the IL
			 * clause_index AND the clause's flags (kind); the gather pass reads both
			 * (one .mono_lsda entry per catch over the shared invoke range, carrying a
			 * self-describing kind column) and the runtime picks the handler by isinst
			 * in that order, so declaration order (inner/more-derived catch first) is
			 * preserved. A single catch adds just its own clause; sibling catches add
			 * the whole group. Every clause admitted here is catch (flags == NONE == 0).
			 */
			for (i = 0; i < cfg->header->num_clauses; ++i) {
				MonoExceptionClause *c = &cfg->header->clauses [i];
				LLVMValueRef type_info;
				LLVMTypeRef i32_ty = llvm::wrap (llvm::Type::getInt32Ty (ctx->llvm_ctx ()));
				LLVMTypeRef ti_members [2] = { i32_ty, i32_ty };
				LLVMTypeRef ti_type = LLVMStructType (ti_members, 2, FALSE);
				LLVMValueRef ti_init [2];

				if (c->flags != MONO_EXCEPTION_CLAUSE_NONE)
					continue;
				if (c->try_offset != this_clause->try_offset || c->try_len != this_clause->try_len)
					continue;

				sprintf (ti_name, "type_info_%d", ti_generator);
				ti_generator ++;

				ti_init [0] = llvm::wrap (llvm::ConstantInt::get (llvm::Type::getInt32Ty (ctx->llvm_ctx ()), i, false));
				ti_init [1] = llvm::wrap (llvm::ConstantInt::get (llvm::Type::getInt32Ty (ctx->llvm_ctx ()), c->flags, false));

				type_info = LLVMAddGlobal (lmodule, ti_type, ti_name);
				LLVMSetInitializer (type_info, LLVMConstNamedStruct (ti_type, ti_init, 2));
				LLVMAddClause (landing_pad, type_info);
			}

			/* Store the exception into the exvar */
			if (ctx->ex_var)
				builder->CreateStore (ctx->convert (builder->CreateExtractValue (llvm::unwrap (landing_pad), {0}, "ex_obj"), llvm::unwrap (ObjRefType ())), ctx->ex_var);

			/*
			 * The selector register holds the matched clause's index; branch to the
			 * sibling handler that owns it (this clause's own body is the default).
			 */
			llvm::Value *ex_selector = builder->CreateExtractValue (llvm::unwrap (landing_pad), {1}, "ex_selector");
			switch_ins = builder->CreateSwitch (ex_selector, llvm::unwrap (target_bb), 0);

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
				switch_ins->addCase (llvm::cast<llvm::ConstantInt> (llvm::ConstantInt::get (llvm::Type::getInt32Ty (ctx->llvm_ctx ()), i, false)), llvm::unwrap (ctx->bblocks [handler_bb->block_num].call_handler_target_bb));
			}

			/*
			 * doc 21 5.1 (EH N2) - enclosing-clause selector cases. DORMANT: the
			 * nesting gate declines every method with a strictly-nested clause, so
			 * clause_encloses () is false for every clause pair in an admitted method
			 * and this loop adds NO case - the catch pad stays exactly as landed (C7).
			 *
			 * When N3 lifts the gate, the runtime always delivers a fault in a
			 * doubly-protected call to the INNERMOST landing pad (this one), carrying
			 * the MATCHED clause's index in the selector (RDX). For each clause j that
			 * ENCLOSES this catch, add a case routing RDX==j to j's handler body, so
			 * control is re-dispatched to the right enclosing handler. Enclosing
			 * handlers may be catch/finally/fault alike - clause_to_handler and
			 * call_handler_target_bb are populated for every clause (translator.cpp) -
			 * so any encloser is a valid target. The exception-object store above
			 * already ran at pad entry and is correct for an enclosing catch too (it
			 * reads the same ex_var). Enclosers have a different try range than this
			 * clause, so they are never same-range siblings and never collide with the
			 * sibling cases above. This is mini-llvm.c:4832-4840 re-homed.
			 */
			for (i = 0; i < cfg->header->num_clauses; ++i) {
				MonoBasicBlock *handler_bb;

				if (!clause_encloses (this_clause, &cfg->header->clauses [i]))
					continue;

				auto clause_it = ctx->clause_to_handler.find (i);
				handler_bb = clause_it != ctx->clause_to_handler.end () ? clause_it->second : nullptr;
				g_assert (handler_bb);
				g_assert (ctx->bblocks [handler_bb->block_num].call_handler_target_bb);
				switch_ins->addCase (llvm::cast<llvm::ConstantInt> (llvm::ConstantInt::get (llvm::Type::getInt32Ty (ctx->llvm_ctx ()), i, false)), llvm::unwrap (ctx->bblocks [handler_bb->block_num].call_handler_target_bb));
			}
		}
	} else {
		/*
		 * A secondary sibling has no landing pad of its own - it is reached only
		 * through the group's invoke-target landing pad selector switch, which
		 * branches straight to this clause's call_handler_target_bb. Its own EH
		 * entry block is never an unwind destination, so terminate it as
		 * unreachable to keep the IR well-formed.
		 */
		llvm::wrap (builder->CreateUnreachable ());
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
		values [exvar->dreg] = builder->CreateLoad (llvm::unwrap (ObjRefType ()), ctx->ex_var, "");
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
			args [i] = llvm::wrap (ctx->convert (llvm::unwrap (args [i]), llvm::unwrap (t2)));
	}

	return llvm::wrap (ctx->builder->CreateCall (llvm::cast<llvm::FunctionType> (llvm::unwrap (LLVMGlobalGetValueType (intrins))), llvm::unwrap (intrins), gep_index_list (args, nargs), name));
}


#endif /* DISABLE_JIT */

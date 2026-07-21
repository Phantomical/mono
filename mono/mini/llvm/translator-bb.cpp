/**
 * \file
 * translator-bb.cpp: process_bb (): the per-instruction IL -> LLVM IR translator.
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

void
process_bb (EmitContext *ctx, MonoBasicBlock *bb)
{
	MonoCompile *cfg = ctx->cfg;
	MonoMethodSignature *sig = ctx->sig;
	LLVMValueRef method = ctx->lmethod;
	LLVMValueRef *values = ctx->values;
	Address **addresses = ctx->addresses;
	LLVMCallInfo *linfo = ctx->linfo;
	BBInfo *bblocks = ctx->bblocks;
	MonoInst *ins;
	LLVMBasicBlockRef cbb;
	LLVMBuilderRef builder, starting_builder;
	gboolean has_terminator;
	LLVMValueRef v;
	LLVMValueRef lhs, rhs, arg3;
	int nins = 0;

	cbb = get_end_bb (ctx, bb);

	builder = create_builder (ctx);
	ctx->builder = builder;
	LLVMPositionBuilderAtEnd (builder, cbb);

	if (!ctx_ok (ctx))
		return;

	if (cfg->interp_entry_only && bb != cfg->bb_init && bb != cfg->bb_entry && bb != cfg->bb_exit) {
		/* The interp entry code is in bb_entry, skip the rest as we might not be able to compile it */
		LLVMBuildUnreachable (builder);
		return;
	}

	if (bb->flags & BB_EXCEPTION_HANDLER) {
		if (!bblocks [bb->block_num].invoke_target) {
			set_failure (ctx, "handler without invokes");
			return;
		}

		emit_handler_start (ctx, bb, builder);
		if (!ctx_ok (ctx))
			return;
		builder = ctx->builder;
	}

	/* Handle PHI nodes first */
	/* They should be grouped at the start of the bb */
	for (ins = bb->code; ins; ins = ins->next) {
		if (ins->opcode == OP_NOP)
			continue;
		if (!MONO_IS_PHI (ins))
			break;

		int i;
		gboolean empty = TRUE;

		/* Check that all input bblocks really branch to us */
		for (i = 0; i < bb->in_count; ++i) {
			if (bb->in_bb [i]->last_ins && bb->in_bb [i]->last_ins->opcode == OP_NOT_REACHED)
				ins->inst_phi_args [i + 1] = -1;
			else
				empty = FALSE;
		}

		if (empty) {
			/* LLVM doesn't like phi instructions with zero operands */
			ctx->is_dead [ins->dreg] = TRUE;
			continue;
		}

		/* Created earlier, insert it now */
		LLVMInsertIntoBuilder (builder, values [ins->dreg]);

		for (i = 0; i < ins->inst_phi_args [0]; i++) {
			int sreg1 = ins->inst_phi_args [i + 1];
			int count, j;

			/*
			 * Count the number of times the incoming bblock branches to us,
			 * since llvm requires a separate entry for each.
			 */
			if (bb->in_bb [i]->last_ins && bb->in_bb [i]->last_ins->opcode == OP_SWITCH) {
				MonoInst *switch_ins = bb->in_bb [i]->last_ins;

				count = 0;
				for (j = 0; j < GPOINTER_TO_UINT (switch_ins->klass); ++j) {
					if (switch_ins->inst_many_bb [j] == bb)
						count ++;
				}
			} else {
				count = 1;
			}

			/* Remember for later */
			for (j = 0; j < count; ++j) {
				PhiNode *node = (PhiNode*)mono_mempool_alloc0 (ctx->mempool, sizeof (PhiNode));
				node->bb = bb;
				node->phi = ins;
				node->in_bb = bb->in_bb [i];
				node->sreg = sreg1;
				bblocks [bb->in_bb [i]->block_num].phi_nodes = g_slist_prepend_mempool (ctx->mempool, bblocks [bb->in_bb [i]->block_num].phi_nodes, node);
			}
		}
	}
	// Add volatile stores for PHI nodes
	// These need to be emitted after the PHI nodes
	for (ins = bb->code; ins; ins = ins->next) {
		const char *spec = LLVM_INS_INFO (ins->opcode);

		if (ins->opcode == OP_NOP)
			continue;
		if (!MONO_IS_PHI (ins))
			break;

		if (spec [MONO_INST_DEST] != 'v')
			emit_volatile_store (ctx, ins->dreg);
	}

	has_terminator = FALSE;
	starting_builder = builder;
	for (ins = bb->code; ins; ins = ins->next) {
		const char *spec = LLVM_INS_INFO (ins->opcode);
		char *dname = NULL;
		char dname_buf [128];

		nins ++;
		if (nins > 1000) {
			/*
			 * Some steps in llc are non-linear in the size of basic blocks, see #5714.
			 * Start a new bblock.
			 * Prevent the bblocks to be merged by doing a volatile load + cond branch
			 * from localloc-ed memory.
			 */
			if (!ctx->long_bb_break_var) {
				ctx->long_bb_break_var = build_alloca_llvm_type_name (ctx, LLVMInt32Type (), 0, "long_bb_break");
				mono_llvm_build_store (ctx->alloca_builder, LLVMConstInt (LLVMInt32Type (), 0, FALSE), ctx->long_bb_break_var, TRUE, LLVM_BARRIER_NONE);
			}

			cbb = gen_bb (ctx, "CONT_LONG_BB");
			LLVMBasicBlockRef dummy_bb = gen_bb (ctx, "CONT_LONG_BB_DUMMY");

			LLVMValueRef load = mono_llvm_build_load (builder, LLVMInt32Type (), ctx->long_bb_break_var, "", TRUE);
			/*
			 * The long_bb_break_var is initialized to 0 in the prolog, so this branch will always go to 'cbb'
			 * but llvm doesn't know that, so the branch is not going to be eliminated.
			 */
			LLVMValueRef cmp = LLVMBuildICmp (builder, LLVMIntEQ, load, LLVMConstInt (LLVMInt32Type (), 0, FALSE), "");

			LLVMBuildCondBr (builder, cmp, cbb, dummy_bb);

			/* Emit a dummy false bblock which does nothing but contains a volatile store so it cannot be eliminated */
			ctx->builder = builder = create_builder (ctx);
			LLVMPositionBuilderAtEnd (builder, dummy_bb);
			mono_llvm_build_store (builder, LLVMConstInt (LLVMInt32Type (), 1, FALSE), ctx->long_bb_break_var, TRUE, LLVM_BARRIER_NONE);
			LLVMBuildBr (builder, cbb);

			ctx->builder = builder = create_builder (ctx);
			LLVMPositionBuilderAtEnd (builder, cbb);
			ctx->bblocks [bb->block_num].end_bblock = cbb;
			nins = 0;
		}

		if (has_terminator)
			/* There could be instructions after a terminator, skip them */
			break;

		if (spec [MONO_INST_DEST] != ' ' && !MONO_IS_STORE_MEMBASE (ins)) {
			sprintf (dname_buf, "t%d", ins->dreg);
			dname = dname_buf;
		}

		if (spec [MONO_INST_SRC1] != ' ' && spec [MONO_INST_SRC1] != 'v') {
			MonoInst *var = get_vreg_to_inst (cfg, ins->sreg1);

			if (var && var->flags & (MONO_INST_VOLATILE|MONO_INST_INDIRECT) && var->opcode != OP_GSHAREDVT_ARG_REGOFFSET) {
				lhs = emit_volatile_load (ctx, ins->sreg1);
			} else {
				/* It is ok for SETRET to have an uninitialized argument */
				if (!values [ins->sreg1] && ins->opcode != OP_SETRET) {
					set_failure (ctx, "sreg1");
					return;
				}
				lhs = values [ins->sreg1];
			}
		} else {
			lhs = NULL;
		}

		if (spec [MONO_INST_SRC2] != ' ' && spec [MONO_INST_SRC2] != 'v') {
			MonoInst *var = get_vreg_to_inst (cfg, ins->sreg2);
			if (var && var->flags & (MONO_INST_VOLATILE|MONO_INST_INDIRECT)) {
				rhs = emit_volatile_load (ctx, ins->sreg2);
			} else {
				if (!values [ins->sreg2]) {
					set_failure (ctx, "sreg2");
					return;
				}
				rhs = values [ins->sreg2];
			}
		} else {
			rhs = NULL;
		}

		if (spec [MONO_INST_SRC3] != ' ' && spec [MONO_INST_SRC3] != 'v') {
			MonoInst *var = get_vreg_to_inst (cfg, ins->sreg3);
			if (var && var->flags & (MONO_INST_VOLATILE|MONO_INST_INDIRECT)) {
				arg3 = emit_volatile_load (ctx, ins->sreg3);
			} else {
				if (!values [ins->sreg3]) {
					set_failure (ctx, "sreg3");
					return;
				}
				arg3 = values [ins->sreg3];
			}
		} else {
			arg3 = NULL;
		}

		//mono_print_ins (ins);
		gboolean skip_volatile_store = FALSE;
		switch (ins->opcode) {
		case OP_NOP:
		case OP_NOT_NULL:
		case OP_LIVERANGE_START:
		case OP_LIVERANGE_END:
			break;
		case OP_ICONST:
			values [ins->dreg] = LLVMConstInt (LLVMInt32Type (), ins->inst_c0, FALSE);
			break;
		case OP_I8CONST:
#if TARGET_SIZEOF_VOID_P == 4
			values [ins->dreg] = LLVMConstInt (LLVMInt64Type (), GET_LONG_IMM (ins), FALSE);
#else
			values [ins->dreg] = LLVMConstInt (LLVMInt64Type (), (gint64)ins->inst_c0, FALSE);
#endif
			break;
		case OP_R8CONST:
			values [ins->dreg] = get_double_const (cfg, *(double*)ins->inst_p0);
			break;
		case OP_R4CONST:
			values [ins->dreg] = get_float_const (cfg, *(float*)ins->inst_p0);
			break;
		case OP_DUMMY_ICONST:
			values [ins->dreg] = LLVMConstInt (LLVMInt32Type (), 0, FALSE);
			break;
		case OP_DUMMY_I8CONST:
			values [ins->dreg] = LLVMConstInt (LLVMInt64Type (), 0, FALSE);
			break;
		case OP_DUMMY_R8CONST:
			values [ins->dreg] = LLVMConstReal (LLVMDoubleType (), 0.0f);
			break;
		case OP_BR: {
			LLVMBasicBlockRef target_bb = get_bb (ctx, ins->inst_target_bb);
			LLVMBuildBr (builder, target_bb);
			has_terminator = TRUE;
			break;
		}
		case OP_SWITCH: {
			int i;
			LLVMValueRef v;
			char bb_name [128];
			LLVMBasicBlockRef new_bb;
			LLVMBuilderRef new_builder;

			// The default branch is already handled
			// FIXME: Handle it here

			/* Start new bblock */
			sprintf (bb_name, "SWITCH_DEFAULT_BB%d", ctx->default_index ++);
			new_bb = LLVMAppendBasicBlock (ctx->lmethod, bb_name);

			lhs = convert (ctx, lhs, LLVMInt32Type ());
			v = LLVMBuildSwitch (builder, lhs, new_bb, GPOINTER_TO_UINT (ins->klass));
			for (i = 0; i < GPOINTER_TO_UINT (ins->klass); ++i) {
				MonoBasicBlock *target_bb = ins->inst_many_bb [i];

				LLVMAddCase (v, LLVMConstInt (LLVMInt32Type (), i, FALSE), get_bb (ctx, target_bb));
			}

			new_builder = create_builder (ctx);
			LLVMPositionBuilderAtEnd (new_builder, new_bb);
			LLVMBuildUnreachable (new_builder);

			has_terminator = TRUE;
			g_assert (!ins->next);
				
			break;
		}

		case OP_SETRET:
			switch (linfo->ret.storage) {
			case LLVMArgVtypeInReg: {
				LLVMTypeRef ret_type = LLVMGetReturnType (LLVMGlobalGetValueType (method));
				LLVMValueRef val, addr, retval;
				int i;

				retval = LLVMGetUndef (ret_type);

				if (!addresses [ins->sreg1]) {
					/*
					 * The return type is an LLVM vector type, have to convert between it and the
					 * real return type which is a struct type.
					 */
					g_assert (MONO_CLASS_IS_SIMD (ctx->cfg, mono_class_from_mono_type_internal (sig->ret)));
					/* Convert to 2xi64 first */
					val = LLVMBuildBitCast (builder, values [ins->sreg1], LLVMVectorType (IntPtrType (), 2), "");

					for (i = 0; i < 2; ++i) {
						if (linfo->ret.pair_storage [i] == LLVMArgInIReg) {
							retval = LLVMBuildInsertValue (builder, retval, LLVMBuildExtractElement (builder, val, LLVMConstInt (LLVMInt32Type (), i, FALSE), ""), i, "");
						} else {
							g_assert (linfo->ret.pair_storage [i] == LLVMArgNone);
						}
					}
				} else {
					addr = LLVMBuildBitCast (builder, addresses [ins->sreg1]->value, LLVMPointerType (ret_type, 0), "");
					for (i = 0; i < 2; ++i) {
						if (linfo->ret.pair_storage [i] == LLVMArgInIReg) {
							LLVMValueRef indexes [2], part_addr;

							indexes [0] = LLVMConstInt (LLVMInt32Type (), 0, FALSE);
							indexes [1] = LLVMConstInt (LLVMInt32Type (), i, FALSE);
							part_addr = LLVMBuildGEP2 (builder, ret_type, addr, indexes, 2, "");

							retval = LLVMBuildInsertValue (builder, retval, LLVMBuildLoad2 (builder, LLVMStructGetTypeAtIndex (ret_type, i), part_addr, ""), i, "");
						} else {
							g_assert (linfo->ret.pair_storage [i] == LLVMArgNone);
						}
					}
				}
				LLVMBuildRet (builder, retval);
				break;
			}
			case LLVMArgVtypeAsScalar: {
				LLVMTypeRef ret_type = LLVMGetReturnType (LLVMGlobalGetValueType (method));
				LLVMValueRef retval;
				if (MONO_CLASS_IS_SIMD (ctx->cfg, mono_class_from_mono_type_internal (sig->ret)))
					retval = LLVMBuildBitCast (builder, values [ins->sreg1], ret_type, "setret_cvt_simd");
				else {
					g_assert (addresses [ins->sreg1]);
					retval = LLVMBuildLoad2 (builder, ret_type, LLVMBuildBitCast (builder, addresses [ins->sreg1]->value, LLVMPointerType (ret_type, 0), ""), "");
				}
				LLVMBuildRet (builder, retval);
				break;
			}
			case LLVMArgVtypeByVal: {
				LLVMValueRef retval;

				g_assert (addresses [ins->sreg1]);
				retval = LLVMBuildLoad2 (builder, addresses [ins->sreg1]->type, addresses [ins->sreg1]->value, "");
				LLVMBuildRet (builder, retval);
				break;
			}
			case LLVMArgVtypeByRef: {
				LLVMBuildRetVoid (builder);
				break;
			}
			case LLVMArgGsharedvtFixed: {
				LLVMTypeRef ret_type = type_to_llvm_type (ctx, sig->ret);
				/* The return value is in lhs, need to store to the vret argument */
				/* sreg1 might not be set */
				if (lhs) {
					g_assert (cfg->vret_addr);
					g_assert (values [cfg->vret_addr->dreg]);
					LLVMBuildStore (builder, convert (ctx, lhs, ret_type), convert (ctx, values [cfg->vret_addr->dreg], LLVMPointerType (ret_type, 0)));
				}
				LLVMBuildRetVoid (builder);
				break;
			}
			case LLVMArgGsharedvtFixedVtype: {
				/* Already set */
				LLVMBuildRetVoid (builder);
				break;
			}
			case LLVMArgGsharedvtVariable: {
				/* Already set */
				LLVMBuildRetVoid (builder);
				break;
			}
			case LLVMArgVtypeRetAddr: {
				LLVMBuildRetVoid (builder);
				break;
			}
			case LLVMArgAsIArgs:
			case LLVMArgFpStruct: {
				LLVMTypeRef ret_type = LLVMGetReturnType (LLVMGlobalGetValueType (method));
				LLVMValueRef retval;

				g_assert (addresses [ins->sreg1]);
				retval = LLVMBuildLoad2 (builder, ret_type, convert (ctx, addresses [ins->sreg1]->value, LLVMPointerType (ret_type, 0)), "");
				LLVMBuildRet (builder, retval);
				break;
			}
			case LLVMArgNone:
			case LLVMArgNormal: {
				if (!lhs || ctx->is_dead [ins->sreg1]) {
					/*
					 * The method did not set its return value, probably because it
					 * ends with a throw.
					 */
					if (cfg->vret_addr)
						LLVMBuildRetVoid (builder);
					else
						LLVMBuildRet (builder, LLVMConstNull (type_to_llvm_type (ctx, sig->ret)));
				} else {
					LLVMBuildRet (builder, convert (ctx, lhs, type_to_llvm_type (ctx, sig->ret)));
				}
				has_terminator = TRUE;
				break;
			}
			default:
				g_assert_not_reached ();
				break;
			}
			break;
		case OP_ICOMPARE:
		case OP_FCOMPARE:
		case OP_RCOMPARE:
		case OP_LCOMPARE:
		case OP_COMPARE:
		case OP_ICOMPARE_IMM:
		case OP_LCOMPARE_IMM:
		case OP_COMPARE_IMM: {
			CompRelation rel;
			LLVMValueRef cmp, args [16];
			gboolean likely = (ins->flags & MONO_INST_LIKELY) != 0;
			gboolean unlikely = FALSE;

			if (MONO_IS_COND_BRANCH_OP (ins->next)) {
				if (ins->next->inst_false_bb->out_of_line)
					likely = TRUE;
				else if (ins->next->inst_true_bb->out_of_line)
					unlikely = TRUE;
			}

			if (ins->next->opcode == OP_NOP)
				break;

			if (ins->next->opcode == OP_BR)
				/* The comparison result is not needed */
				continue;

			rel = mono_opcode_to_cond (ins->next->opcode);

			if (ins->opcode == OP_ICOMPARE_IMM) {
				lhs = convert (ctx, lhs, LLVMInt32Type ());
				rhs = LLVMConstInt (LLVMInt32Type (), ins->inst_imm, FALSE);
			}
			if (ins->opcode == OP_LCOMPARE_IMM) {
				lhs = convert (ctx, lhs, LLVMInt64Type ());
				rhs = LLVMConstInt (LLVMInt64Type (), GET_LONG_IMM (ins), FALSE);
			}
			if (ins->opcode == OP_LCOMPARE) {
				lhs = convert (ctx, lhs, LLVMInt64Type ());
				rhs = convert (ctx, rhs, LLVMInt64Type ());
			}
			if (ins->opcode == OP_ICOMPARE) {
				lhs = convert (ctx, lhs, LLVMInt32Type ());
				rhs = convert (ctx, rhs, LLVMInt32Type ());
			}

			if (lhs && rhs) {
				if (LLVMGetTypeKind (LLVMTypeOf (lhs)) == LLVMPointerTypeKind)
					rhs = convert (ctx, rhs, LLVMTypeOf (lhs));
				else if (LLVMGetTypeKind (LLVMTypeOf (rhs)) == LLVMPointerTypeKind)
					lhs = convert (ctx, lhs, LLVMTypeOf (rhs));
			}

			/* We use COMPARE+SETcc/Bcc, llvm uses SETcc+br cond */
			if (ins->opcode == OP_FCOMPARE) {
				cmp = LLVMBuildFCmp (builder, fpcond_to_llvm_cond [rel], convert (ctx, lhs, LLVMDoubleType ()), convert (ctx, rhs, LLVMDoubleType ()), "");
			} else if (ins->opcode == OP_RCOMPARE) {
				cmp = LLVMBuildFCmp (builder, fpcond_to_llvm_cond [rel], convert (ctx, lhs, LLVMFloatType ()), convert (ctx, rhs, LLVMFloatType ()), "");
			} else if (ins->opcode == OP_COMPARE_IMM) {
				LLVMIntPredicate llvm_pred = cond_to_llvm_cond [rel];
				if (LLVMGetTypeKind (LLVMTypeOf (lhs)) == LLVMPointerTypeKind && ins->inst_imm == 0) {
					// We are emitting a NULL check for a pointer
					gboolean nonnull = mono_llvm_is_nonnull (lhs);

					if (nonnull && llvm_pred == LLVMIntEQ)
						cmp = LLVMConstInt (LLVMInt1Type (), FALSE, FALSE);
					else if (nonnull && llvm_pred == LLVMIntNE)
						cmp = LLVMConstInt (LLVMInt1Type (), TRUE, FALSE);
					else
						cmp = LLVMBuildICmp (builder, llvm_pred, lhs, LLVMConstNull (LLVMTypeOf (lhs)), "");

				} else {
					cmp = LLVMBuildICmp (builder, llvm_pred, convert (ctx, lhs, IntPtrType ()), LLVMConstInt (IntPtrType (), ins->inst_imm, FALSE), "");
				}
			} else if (ins->opcode == OP_LCOMPARE_IMM) {
				cmp = LLVMBuildICmp (builder, cond_to_llvm_cond [rel], lhs, rhs, "");
			}
			else if (ins->opcode == OP_COMPARE) {
				if (LLVMGetTypeKind (LLVMTypeOf (lhs)) == LLVMPointerTypeKind && LLVMTypeOf (lhs) == LLVMTypeOf (rhs))
					cmp = LLVMBuildICmp (builder, cond_to_llvm_cond [rel], lhs, rhs, "");
				else
					cmp = LLVMBuildICmp (builder, cond_to_llvm_cond [rel], convert (ctx, lhs, IntPtrType ()), convert (ctx, rhs, IntPtrType ()), "");
			} else
				cmp = LLVMBuildICmp (builder, cond_to_llvm_cond [rel], lhs, rhs, "");

			if (likely || unlikely) {
				args [0] = cmp;
				args [1] = LLVMConstInt (LLVMInt1Type (), likely ? 1 : 0, FALSE);
				cmp = call_intrins (ctx, INTRINS_EXPECT_I1, args, "");
			}

			if (MONO_IS_COND_BRANCH_OP (ins->next)) {
				if (ins->next->inst_true_bb == ins->next->inst_false_bb) {
					/*
					 * If the target bb contains PHI instructions, LLVM requires
					 * two PHI entries for this bblock, while we only generate one.
					 * So convert this to an unconditional bblock. (bxc #171).
					 */
					LLVMBuildBr (builder, get_bb (ctx, ins->next->inst_true_bb));
				} else {
					LLVMBuildCondBr (builder, cmp, get_bb (ctx, ins->next->inst_true_bb), get_bb (ctx, ins->next->inst_false_bb));
				}
				has_terminator = TRUE;
			} else if (MONO_IS_SETCC (ins->next)) {
				sprintf (dname_buf, "t%d", ins->next->dreg);
				dname = dname_buf;
				values [ins->next->dreg] = LLVMBuildZExt (builder, cmp, LLVMInt32Type (), dname);

				/* Add stores for volatile variables */
				emit_volatile_store (ctx, ins->next->dreg);
			} else if (MONO_IS_COND_EXC (ins->next)) {
				gboolean force_explicit_branch = FALSE;
				if (bb->region != -1) {
					/* Don't tag null check branches in exception-handling
					 * regions with `make.implicit`.
					 */
					force_explicit_branch = TRUE;
				}
				emit_cond_system_exception (ctx, bb, (const char*)ins->next->inst_p1, cmp, force_explicit_branch);
				if (!ctx_ok (ctx))
					break;
				builder = ctx->builder;
			} else {
				set_failure (ctx, "next");
				break;
			}

			ins = ins->next;
			break;
		}
		case OP_FCEQ:
		case OP_FCNEQ:
		case OP_FCLT:
		case OP_FCLT_UN:
		case OP_FCGT:
		case OP_FCGT_UN:
		case OP_FCGE:
		case OP_FCLE: {
			CompRelation rel;
			LLVMValueRef cmp;

			rel = mono_opcode_to_cond (ins->opcode);

			cmp = LLVMBuildFCmp (builder, fpcond_to_llvm_cond [rel], convert (ctx, lhs, LLVMDoubleType ()), convert (ctx, rhs, LLVMDoubleType ()), "");
			values [ins->dreg] = LLVMBuildZExt (builder, cmp, LLVMInt32Type (), dname);
			break;
		}
		case OP_RCEQ:
		case OP_RCNEQ:
		case OP_RCLT:
		case OP_RCLT_UN:
		case OP_RCGT:
		case OP_RCGT_UN: {
			CompRelation rel;
			LLVMValueRef cmp;

			rel = mono_opcode_to_cond (ins->opcode);

			cmp = LLVMBuildFCmp (builder, fpcond_to_llvm_cond [rel], convert (ctx, lhs, LLVMFloatType ()), convert (ctx, rhs, LLVMFloatType ()), "");
			values [ins->dreg] = LLVMBuildZExt (builder, cmp, LLVMInt32Type (), dname);
			break;
		}
		case OP_PHI:
		case OP_FPHI:
		case OP_VPHI:
		case OP_XPHI: {
			// Handled above
			skip_volatile_store = TRUE;
			break;
		}
		case OP_MOVE:
		case OP_LMOVE:
		case OP_XMOVE:
		case OP_SETFRET:
			g_assert (lhs);
			values [ins->dreg] = lhs;
			break;
		case OP_FMOVE:
		case OP_RMOVE: {
			MonoInst *var = get_vreg_to_inst (cfg, ins->dreg);
				
			g_assert (lhs);
			values [ins->dreg] = lhs;

			if (var && m_class_get_byval_arg (var->klass)->type == MONO_TYPE_R4) {
				/* 
				 * This is added by the spilling pass in case of the JIT,
				 * but we have to do it ourselves.
				 */
				values [ins->dreg] = convert (ctx, values [ins->dreg], LLVMFloatType ());
			}
			break;
		}
		case OP_MOVE_F_TO_I4: {
			values [ins->dreg] = LLVMBuildBitCast (builder, LLVMBuildFPTrunc (builder, lhs, LLVMFloatType (), ""), LLVMInt32Type (), "");
			break;
		}
		case OP_MOVE_I4_TO_F: {
			values [ins->dreg] = LLVMBuildFPExt (builder, LLVMBuildBitCast (builder, lhs, LLVMFloatType (), ""), LLVMDoubleType (), "");
			break;
		}
		case OP_MOVE_F_TO_I8: {
			values [ins->dreg] = LLVMBuildBitCast (builder, lhs, LLVMInt64Type (), "");
			break;
		}
		case OP_MOVE_I8_TO_F: {
			values [ins->dreg] = LLVMBuildBitCast (builder, lhs, LLVMDoubleType (), "");
			break;
		}
		case OP_IADD:
		case OP_ISUB:
		case OP_IAND:
		case OP_IMUL:
		case OP_IDIV:
		case OP_IDIV_UN:
		case OP_IREM:
		case OP_IREM_UN:
		case OP_IOR:
		case OP_IXOR:
		case OP_ISHL:
		case OP_ISHR:
		case OP_ISHR_UN:
		case OP_FADD:
		case OP_FSUB:
		case OP_FMUL:
		case OP_FDIV:
		case OP_LADD:
		case OP_LSUB:
		case OP_LMUL:
		case OP_LDIV:
		case OP_LDIV_UN:
		case OP_LREM:
		case OP_LREM_UN:
		case OP_LAND:
		case OP_LOR:
		case OP_LXOR:
		case OP_LSHL:
		case OP_LSHR:
		case OP_LSHR_UN:
			lhs = convert (ctx, lhs, regtype_to_llvm_type (spec [MONO_INST_DEST]));
			rhs = convert (ctx, rhs, regtype_to_llvm_type (spec [MONO_INST_DEST]));

			emit_div_check (ctx, builder, bb, ins, lhs, rhs);
			if (!ctx_ok (ctx))
				break;
			builder = ctx->builder;

			switch (ins->opcode) {
			case OP_IADD:
			case OP_LADD:
				values [ins->dreg] = LLVMBuildAdd (builder, lhs, rhs, dname);
				break;
			case OP_ISUB:
			case OP_LSUB:
				values [ins->dreg] = LLVMBuildSub (builder, lhs, rhs, dname);
				break;
			case OP_IMUL:
			case OP_LMUL:
				values [ins->dreg] = LLVMBuildMul (builder, lhs, rhs, dname);
				break;
			case OP_IREM:
			case OP_LREM:
				values [ins->dreg] = LLVMBuildSRem (builder, lhs, rhs, dname);
				break;
			case OP_IREM_UN:
			case OP_LREM_UN:
				values [ins->dreg] = LLVMBuildURem (builder, lhs, rhs, dname);
				break;
			case OP_IDIV:
			case OP_LDIV:
				values [ins->dreg] = LLVMBuildSDiv (builder, lhs, rhs, dname);
				break;
			case OP_IDIV_UN:
			case OP_LDIV_UN:
				values [ins->dreg] = LLVMBuildUDiv (builder, lhs, rhs, dname);
				break;
			case OP_FDIV:
			case OP_RDIV:
				values [ins->dreg] = LLVMBuildFDiv (builder, lhs, rhs, dname);
				break;
			case OP_IAND:
			case OP_LAND:
				values [ins->dreg] = LLVMBuildAnd (builder, lhs, rhs, dname);
				break;
			case OP_IOR:
			case OP_LOR:
				values [ins->dreg] = LLVMBuildOr (builder, lhs, rhs, dname);
				break;
			case OP_IXOR:
			case OP_LXOR:
				values [ins->dreg] = LLVMBuildXor (builder, lhs, rhs, dname);
				break;
			case OP_ISHL:
			case OP_LSHL:
				values [ins->dreg] = LLVMBuildShl (builder, lhs, rhs, dname);
				break;
			case OP_ISHR:
			case OP_LSHR:
				values [ins->dreg] = LLVMBuildAShr (builder, lhs, rhs, dname);
				break;
			case OP_ISHR_UN:
			case OP_LSHR_UN:
				values [ins->dreg] = LLVMBuildLShr (builder, lhs, rhs, dname);
				break;

			case OP_FADD:
				values [ins->dreg] = LLVMBuildFAdd (builder, lhs, rhs, dname);
				break;
			case OP_FSUB:
				values [ins->dreg] = LLVMBuildFSub (builder, lhs, rhs, dname);
				break;
			case OP_FMUL:
				values [ins->dreg] = LLVMBuildFMul (builder, lhs, rhs, dname);
				break;

			default:
				g_assert_not_reached ();
			}
			break;
		case OP_RADD:
		case OP_RSUB:
		case OP_RMUL:
		case OP_RDIV: {
			lhs = convert (ctx, lhs, LLVMFloatType ());
			rhs = convert (ctx, rhs, LLVMFloatType ());
			switch (ins->opcode) {
			case OP_RADD:
				values [ins->dreg] = LLVMBuildFAdd (builder, lhs, rhs, dname);
				break;
			case OP_RSUB:
				values [ins->dreg] = LLVMBuildFSub (builder, lhs, rhs, dname);
				break;
			case OP_RMUL:
				values [ins->dreg] = LLVMBuildFMul (builder, lhs, rhs, dname);
				break;
			case OP_RDIV:
				values [ins->dreg] = LLVMBuildFDiv (builder, lhs, rhs, dname);
				break;
			default:
				g_assert_not_reached ();
				break;
			}
			break;
		}
		case OP_IADD_IMM:
		case OP_ISUB_IMM:
		case OP_IMUL_IMM:
		case OP_IREM_IMM:
		case OP_IREM_UN_IMM:
		case OP_IDIV_IMM:
		case OP_IDIV_UN_IMM:
		case OP_IAND_IMM:
		case OP_IOR_IMM:
		case OP_IXOR_IMM:
		case OP_ISHL_IMM:
		case OP_ISHR_IMM:
		case OP_ISHR_UN_IMM:
		case OP_LADD_IMM:
		case OP_LSUB_IMM:
		case OP_LMUL_IMM:
		case OP_LREM_IMM:
		case OP_LAND_IMM:
		case OP_LOR_IMM:
		case OP_LXOR_IMM:
		case OP_LSHL_IMM:
		case OP_LSHR_IMM:
		case OP_LSHR_UN_IMM:
		case OP_ADD_IMM:
		case OP_AND_IMM:
		case OP_MUL_IMM:
		case OP_SHL_IMM:
		case OP_SHR_IMM:
		case OP_SHR_UN_IMM: {
			LLVMValueRef imm;

			if (spec [MONO_INST_SRC1] == 'l') {
				imm = LLVMConstInt (LLVMInt64Type (), GET_LONG_IMM (ins), FALSE);
			} else {
				imm = LLVMConstInt (LLVMInt32Type (), ins->inst_imm, FALSE);
			}

			emit_div_check (ctx, builder, bb, ins, lhs, imm);
			if (!ctx_ok (ctx))
				break;
			builder = ctx->builder;

#if TARGET_SIZEOF_VOID_P == 4
			if (ins->opcode == OP_LSHL_IMM || ins->opcode == OP_LSHR_IMM || ins->opcode == OP_LSHR_UN_IMM)
				imm = LLVMConstInt (LLVMInt32Type (), ins->inst_imm, FALSE);
#endif

			if (LLVMGetTypeKind (LLVMTypeOf (lhs)) == LLVMPointerTypeKind)
				lhs = convert (ctx, lhs, IntPtrType ());
			imm = convert (ctx, imm, LLVMTypeOf (lhs));
			switch (ins->opcode) {
			case OP_IADD_IMM:
			case OP_LADD_IMM:
			case OP_ADD_IMM:
				values [ins->dreg] = LLVMBuildAdd (builder, lhs, imm, dname);
				break;
			case OP_ISUB_IMM:
			case OP_LSUB_IMM:
				values [ins->dreg] = LLVMBuildSub (builder, lhs, imm, dname);
				break;
			case OP_IMUL_IMM:
			case OP_MUL_IMM:
			case OP_LMUL_IMM:
				values [ins->dreg] = LLVMBuildMul (builder, lhs, imm, dname);
				break;
			case OP_IDIV_IMM:
			case OP_LDIV_IMM:
				values [ins->dreg] = LLVMBuildSDiv (builder, lhs, imm, dname);
				break;
			case OP_IDIV_UN_IMM:
			case OP_LDIV_UN_IMM:
				values [ins->dreg] = LLVMBuildUDiv (builder, lhs, imm, dname);
				break;
			case OP_IREM_IMM:
			case OP_LREM_IMM:
				values [ins->dreg] = LLVMBuildSRem (builder, lhs, imm, dname);
				break;
			case OP_IREM_UN_IMM:
				values [ins->dreg] = LLVMBuildURem (builder, lhs, imm, dname);
				break;
			case OP_IAND_IMM:
			case OP_LAND_IMM:
			case OP_AND_IMM:
				values [ins->dreg] = LLVMBuildAnd (builder, lhs, imm, dname);
				break;
			case OP_IOR_IMM:
			case OP_LOR_IMM:
				values [ins->dreg] = LLVMBuildOr (builder, lhs, imm, dname);
				break;
			case OP_IXOR_IMM:
			case OP_LXOR_IMM:
				values [ins->dreg] = LLVMBuildXor (builder, lhs, imm, dname);
				break;
			case OP_ISHL_IMM:
			case OP_LSHL_IMM:
				values [ins->dreg] = LLVMBuildShl (builder, lhs, imm, dname);
				break;
			case OP_SHL_IMM:
				if (TARGET_SIZEOF_VOID_P == 8) {
					/* The IL is not regular */
					lhs = convert (ctx, lhs, LLVMInt64Type ());
					imm = convert (ctx, imm, LLVMInt64Type ());
				}
				values [ins->dreg] = LLVMBuildShl (builder, lhs, imm, dname);
				break;
			case OP_ISHR_IMM:
			case OP_LSHR_IMM:
			case OP_SHR_IMM:
				values [ins->dreg] = LLVMBuildAShr (builder, lhs, imm, dname);
				break;
			case OP_ISHR_UN_IMM:
				/* This is used to implement conv.u4, so the lhs could be an i8 */
				lhs = convert (ctx, lhs, LLVMInt32Type ());
				imm = convert (ctx, imm, LLVMInt32Type ());
				values [ins->dreg] = LLVMBuildLShr (builder, lhs, imm, dname);
				break;
			case OP_LSHR_UN_IMM:
			case OP_SHR_UN_IMM:
				values [ins->dreg] = LLVMBuildLShr (builder, lhs, imm, dname);
				break;
			default:
				g_assert_not_reached ();
			}
			break;
		}
		case OP_INEG:
			values [ins->dreg] = LLVMBuildSub (builder, LLVMConstInt (LLVMInt32Type (), 0, FALSE), convert (ctx, lhs, LLVMInt32Type ()), dname);
			break;
		case OP_LNEG:
			if (LLVMTypeOf (lhs) != LLVMInt64Type ())
				lhs = convert (ctx, lhs, LLVMInt64Type ());
			values [ins->dreg] = LLVMBuildSub (builder, LLVMConstInt (LLVMInt64Type (), 0, FALSE), lhs, dname);
			break;
		case OP_FNEG:
			lhs = convert (ctx, lhs, LLVMDoubleType ());
			values [ins->dreg] = LLVMBuildFNeg (builder, lhs, dname);
			break;
		case OP_RNEG:
			lhs = convert (ctx, lhs, LLVMFloatType ());
			values [ins->dreg] = LLVMBuildFNeg (builder, lhs, dname);
			break;
		case OP_INOT: {
			guint32 v = 0xffffffff;
			values [ins->dreg] = LLVMBuildXor (builder, LLVMConstInt (LLVMInt32Type (), v, FALSE), convert (ctx, lhs, LLVMInt32Type ()), dname);
			break;
		}
		case OP_LNOT: {
			if (LLVMTypeOf (lhs) != LLVMInt64Type ())
				lhs = convert (ctx, lhs, LLVMInt64Type ());
			guint64 v = 0xffffffffffffffffLL;
			values [ins->dreg] = LLVMBuildXor (builder, LLVMConstInt (LLVMInt64Type (), v, FALSE), lhs, dname);
			break;
		}
#if defined(TARGET_X86) || defined(TARGET_AMD64)
		case OP_X86_LEA: {
			LLVMValueRef v1, v2;

			rhs = LLVMBuildSExt (builder, convert (ctx, rhs, LLVMInt32Type ()), LLVMInt64Type (), "");

			v1 = LLVMBuildMul (builder, convert (ctx, rhs, IntPtrType ()), LLVMConstInt (IntPtrType (), ((unsigned long long)1 << ins->backend.shift_amount), FALSE), "");
			v2 = LLVMBuildAdd (builder, convert (ctx, lhs, IntPtrType ()), v1, "");
			values [ins->dreg] = LLVMBuildAdd (builder, v2, LLVMConstInt (IntPtrType (), ins->inst_imm, FALSE), dname);
			break;
		}
		case OP_X86_BSF32:
		case OP_X86_BSF64: {
			LLVMValueRef args [] = {
				lhs,
				LLVMConstInt (LLVMInt1Type (), 1, TRUE),
			};
			int op = ins->opcode == OP_X86_BSF32 ? INTRINS_CTTZ_I32 : INTRINS_CTTZ_I64;
			values [ins->dreg] = call_intrins (ctx, op, args, dname);
			break;
		}
		case OP_X86_BSR32:
		case OP_X86_BSR64: {
			LLVMValueRef args [] = {
				lhs,
				LLVMConstInt (LLVMInt1Type (), 1, TRUE),
			};
			int op = ins->opcode == OP_X86_BSR32 ? INTRINS_CTLZ_I32 : INTRINS_CTLZ_I64;
			LLVMValueRef width = ins->opcode == OP_X86_BSR32 ? const_int32 (31) : const_int64 (63);
			LLVMValueRef tz = call_intrins (ctx, op, args, "");
			values [ins->dreg] = LLVMBuildXor (builder, tz, width, dname);
			break;
		}
#endif

		case OP_ICONV_TO_I1:
		case OP_ICONV_TO_I2:
		case OP_ICONV_TO_I4:
		case OP_ICONV_TO_U1:
		case OP_ICONV_TO_U2:
		case OP_ICONV_TO_U4:
		case OP_LCONV_TO_I1:
		case OP_LCONV_TO_I2:
		case OP_LCONV_TO_U1:
		case OP_LCONV_TO_U2:
		case OP_LCONV_TO_U4: {
			gboolean sign;

			sign = (ins->opcode == OP_ICONV_TO_I1) || (ins->opcode == OP_ICONV_TO_I2) || (ins->opcode == OP_ICONV_TO_I4) || (ins->opcode == OP_LCONV_TO_I1) || (ins->opcode == OP_LCONV_TO_I2);

			/* Have to do two casts since our vregs have type int */
			v = LLVMBuildTrunc (builder, lhs, op_to_llvm_type (ins->opcode), "");
			if (sign)
				values [ins->dreg] = LLVMBuildSExt (builder, v, LLVMInt32Type (), dname);
			else
				values [ins->dreg] = LLVMBuildZExt (builder, v, LLVMInt32Type (), dname);
			break;
		}
		case OP_ICONV_TO_I8:
			values [ins->dreg] = LLVMBuildSExt (builder, lhs, LLVMInt64Type (), dname);
			break;
		case OP_ICONV_TO_U8:
			values [ins->dreg] = LLVMBuildZExt (builder, lhs, LLVMInt64Type (), dname);
			break;
		case OP_FCONV_TO_I4:
		case OP_RCONV_TO_I4:
			values [ins->dreg] = LLVMBuildFPToSI (builder, lhs, LLVMInt32Type (), dname);
			break;
		case OP_FCONV_TO_I1:
		case OP_RCONV_TO_I1:
			values [ins->dreg] = LLVMBuildSExt (builder, LLVMBuildFPToSI (builder, lhs, LLVMInt8Type (), dname), LLVMInt32Type (), "");
			break;
		case OP_FCONV_TO_U1:
		case OP_RCONV_TO_U1:
			values [ins->dreg] = LLVMBuildZExt (builder, LLVMBuildTrunc (builder, LLVMBuildFPToUI (builder, lhs, IntPtrType (), dname), LLVMInt8Type (), ""), LLVMInt32Type (), "");
			break;
		case OP_FCONV_TO_I2:
		case OP_RCONV_TO_I2:
			values [ins->dreg] = LLVMBuildSExt (builder, LLVMBuildFPToSI (builder, lhs, LLVMInt16Type (), dname), LLVMInt32Type (), "");
			break;
		case OP_FCONV_TO_U2:
		case OP_RCONV_TO_U2:
			values [ins->dreg] = LLVMBuildZExt (builder, LLVMBuildFPToUI (builder, lhs, LLVMInt16Type (), dname), LLVMInt32Type (), "");
			break;
		case OP_FCONV_TO_U4:
		case OP_RCONV_TO_U4:
			values [ins->dreg] = LLVMBuildFPToUI (builder, lhs, LLVMInt32Type (), dname);
			break;
		case OP_FCONV_TO_U8:
		case OP_RCONV_TO_U8:
			values [ins->dreg] = LLVMBuildFPToUI (builder, lhs, LLVMInt64Type (), dname);
			break;
		case OP_FCONV_TO_I8:
		case OP_RCONV_TO_I8:
			values [ins->dreg] = LLVMBuildFPToSI (builder, lhs, LLVMInt64Type (), dname);
			break;
		case OP_FCONV_TO_I:
			values [ins->dreg] = LLVMBuildFPToSI (builder, lhs, IntPtrType (), dname);
			break;
		case OP_ICONV_TO_R8:
		case OP_LCONV_TO_R8:
			values [ins->dreg] = LLVMBuildSIToFP (builder, lhs, LLVMDoubleType (), dname);
			break;
		case OP_ICONV_TO_R_UN:
		case OP_LCONV_TO_R_UN:
			values [ins->dreg] = LLVMBuildUIToFP (builder, lhs, LLVMDoubleType (), dname);
			break;
#if TARGET_SIZEOF_VOID_P == 4
		case OP_LCONV_TO_U:
#endif
		case OP_LCONV_TO_I4:
			values [ins->dreg] = LLVMBuildTrunc (builder, lhs, LLVMInt32Type (), dname);
			break;
		case OP_ICONV_TO_R4:
		case OP_LCONV_TO_R4:
			v = LLVMBuildSIToFP (builder, lhs, LLVMFloatType (), "");
			if (cfg->r4fp)
				values [ins->dreg] = v;
			else
				values [ins->dreg] = LLVMBuildFPExt (builder, v, LLVMDoubleType (), dname);
			break;
		case OP_FCONV_TO_R4:
			v = LLVMBuildFPTrunc (builder, lhs, LLVMFloatType (), "");
			if (cfg->r4fp)
				values [ins->dreg] = v;
			else
				values [ins->dreg] = LLVMBuildFPExt (builder, v, LLVMDoubleType (), dname);
			break;
		case OP_RCONV_TO_R8:
			values [ins->dreg] = LLVMBuildFPExt (builder, lhs, LLVMDoubleType (), dname);
			break;
		case OP_RCONV_TO_R4:
			values [ins->dreg] = lhs;
			break;
		case OP_SEXT_I4:
			values [ins->dreg] = LLVMBuildSExt (builder, convert (ctx, lhs, LLVMInt32Type ()), LLVMInt64Type (), dname);
			break;
		case OP_ZEXT_I4:
			values [ins->dreg] = LLVMBuildZExt (builder, convert (ctx, lhs, LLVMInt32Type ()), LLVMInt64Type (), dname);
			break;
		case OP_TRUNC_I4:
			values [ins->dreg] = LLVMBuildTrunc (builder, lhs, LLVMInt32Type (), dname);
			break;
		case OP_LOCALLOC_IMM: {
			LLVMValueRef v;

			guint32 size = ins->inst_imm;
			size = (size + (MONO_ARCH_FRAME_ALIGNMENT - 1)) & ~ (MONO_ARCH_FRAME_ALIGNMENT - 1);

			v = mono_llvm_build_alloca (builder, LLVMInt8Type (), LLVMConstInt (LLVMInt32Type (), size, FALSE), MONO_ARCH_FRAME_ALIGNMENT, "");

			if (ins->flags & MONO_INST_INIT)
				emit_memset (ctx, builder, v, const_int32 (size), MONO_ARCH_FRAME_ALIGNMENT);

			values [ins->dreg] = v;
			break;
		}
		case OP_LOCALLOC: {
			LLVMValueRef v, size;
				
			size = LLVMBuildAnd (builder, LLVMBuildAdd (builder, convert (ctx, lhs, LLVMInt32Type ()), LLVMConstInt (LLVMInt32Type (), MONO_ARCH_FRAME_ALIGNMENT - 1, FALSE), ""), LLVMConstInt (LLVMInt32Type (), ~ (MONO_ARCH_FRAME_ALIGNMENT - 1), FALSE), "");

			v = mono_llvm_build_alloca (builder, LLVMInt8Type (), size, MONO_ARCH_FRAME_ALIGNMENT, "");

			if (ins->flags & MONO_INST_INIT)
				emit_memset (ctx, builder, v, size, MONO_ARCH_FRAME_ALIGNMENT);
			values [ins->dreg] = v;
			break;
		}

		case OP_LOADI1_MEMBASE:
		case OP_LOADU1_MEMBASE:
		case OP_LOADI2_MEMBASE:
		case OP_LOADU2_MEMBASE:
		case OP_LOADI4_MEMBASE:
		case OP_LOADU4_MEMBASE:
		case OP_LOADI8_MEMBASE:
		case OP_LOADR4_MEMBASE:
		case OP_LOADR8_MEMBASE:
		case OP_LOAD_MEMBASE:
		case OP_LOADI8_MEM:
		case OP_LOADU1_MEM:
		case OP_LOADU2_MEM:
		case OP_LOADI4_MEM:
		case OP_LOADU4_MEM:
		case OP_LOAD_MEM: {
			int size = 8;
			LLVMValueRef base, index, addr;
			LLVMTypeRef t;
			gboolean sext = FALSE, zext = FALSE;
			gboolean is_faulting = (ins->flags & MONO_INST_FAULT) != 0;
			gboolean is_volatile = (ins->flags & MONO_INST_VOLATILE) != 0;
			gboolean is_unaligned = (ins->flags & MONO_INST_UNALIGNED) != 0;

			t = load_store_to_llvm_type (ins->opcode, &size, &sext, &zext);

			if (sext || zext)
				dname = (char*)"";

			if ((ins->opcode == OP_LOADI8_MEM) || (ins->opcode == OP_LOAD_MEM) || (ins->opcode == OP_LOADI4_MEM) || (ins->opcode == OP_LOADU4_MEM) || (ins->opcode == OP_LOADU1_MEM) || (ins->opcode == OP_LOADU2_MEM)) {
				addr = LLVMConstInt (IntPtrType (), ins->inst_imm, FALSE);
				base = addr;
			} else {
				/* _MEMBASE */
				base = lhs;

				if (ins->inst_offset == 0) {
					LLVMValueRef gep_base, gep_offset;
					if (mono_llvm_can_be_gep (base, &gep_base, &gep_offset)) {
						addr = LLVMBuildGEP2 (builder, LLVMInt8Type (), convert (ctx, gep_base, LLVMPointerType (LLVMInt8Type (), 0)), &gep_offset, 1, "");
					} else {
						addr = base;
					}
				} else if (ins->inst_offset % size != 0) {
					/* Unaligned load */
					index = LLVMConstInt (LLVMInt32Type (), ins->inst_offset, FALSE);
					addr = LLVMBuildGEP2 (builder, LLVMInt8Type (), convert (ctx, base, LLVMPointerType (LLVMInt8Type (), 0)), &index, 1, "");
				} else {
					index = LLVMConstInt (LLVMInt32Type (), ins->inst_offset / size, FALSE);
					addr = LLVMBuildGEP2 (builder, t, convert (ctx, base, LLVMPointerType (t, 0)), &index, 1, "");
				}
			}

			addr = convert (ctx, addr, LLVMPointerType (t, 0));

			if (is_unaligned)
				values [ins->dreg] = mono_llvm_build_aligned_load (builder, t, addr, dname, is_volatile, 1);
			else
				values [ins->dreg] = emit_load (ctx, bb, &builder, size, t, addr, base, dname, is_faulting, is_volatile, LLVM_BARRIER_NONE);

			if (!(is_faulting || is_volatile) && (ins->flags & MONO_INST_INVARIANT_LOAD)) {
				/*
				 * These will signal LLVM that these loads do not alias any stores, and
				 * they can't fail, allowing them to be hoisted out of loops.
				 */
				set_invariant_load_flag (values [ins->dreg]);
			}

			if (sext)
				values [ins->dreg] = LLVMBuildSExt (builder, values [ins->dreg], LLVMInt32Type (), dname);
			else if (zext)
				values [ins->dreg] = LLVMBuildZExt (builder, values [ins->dreg], LLVMInt32Type (), dname);
			else if (!cfg->r4fp && ins->opcode == OP_LOADR4_MEMBASE)
				values [ins->dreg] = LLVMBuildFPExt (builder, values [ins->dreg], LLVMDoubleType (), dname);
			break;
		}

		case OP_STOREI1_MEMBASE_REG:
		case OP_STOREI2_MEMBASE_REG:
		case OP_STOREI4_MEMBASE_REG:
		case OP_STOREI8_MEMBASE_REG:
		case OP_STORER4_MEMBASE_REG:
		case OP_STORER8_MEMBASE_REG:
		case OP_STORE_MEMBASE_REG: {
			int size = 8;
			LLVMValueRef index, addr, base;
			LLVMTypeRef t;
			gboolean sext = FALSE, zext = FALSE;
			gboolean is_faulting = (ins->flags & MONO_INST_FAULT) != 0;
			gboolean is_volatile = (ins->flags & MONO_INST_VOLATILE) != 0;
			gboolean is_unaligned = (ins->flags & MONO_INST_UNALIGNED) != 0;

			if (!values [ins->inst_destbasereg]) {
				set_failure (ctx, "inst_destbasereg");
				break;
			}

			t = load_store_to_llvm_type (ins->opcode, &size, &sext, &zext);

			base = values [ins->inst_destbasereg];
			LLVMValueRef gep_base, gep_offset;
			if (ins->inst_offset == 0 && mono_llvm_can_be_gep (base, &gep_base, &gep_offset)) {
				addr = LLVMBuildGEP2 (builder, LLVMInt8Type (), convert (ctx, gep_base, LLVMPointerType (LLVMInt8Type (), 0)), &gep_offset, 1, "");
			} else if (ins->inst_offset % size != 0) {
				/* Unaligned store */
				index = LLVMConstInt (LLVMInt32Type (), ins->inst_offset, FALSE);
				addr = LLVMBuildGEP2 (builder, LLVMInt8Type (), convert (ctx, base, LLVMPointerType (LLVMInt8Type (), 0)), &index, 1, "");
			} else {
				index = LLVMConstInt (LLVMInt32Type (), ins->inst_offset / size, FALSE);
				addr = LLVMBuildGEP2 (builder, t, convert (ctx, base, LLVMPointerType (t, 0)), &index, 1, "");
			}
			if (is_volatile && LLVMGetInstructionOpcode (base) == LLVMAlloca && !(ins->flags & MONO_INST_VOLATILE))
				/* Storing to an alloca cannot fail */
				is_volatile = FALSE;
			LLVMValueRef srcval = convert (ctx, values [ins->sreg1], t);
			LLVMValueRef ptrdst = convert (ctx, addr, LLVMPointerType (t, 0));

			if (is_unaligned)
				mono_llvm_build_aligned_store (builder, srcval, ptrdst, is_volatile, 1);
			else
				emit_store (ctx, bb, &builder, size, srcval, ptrdst, base, is_faulting, is_volatile);
			break;
		}

		case OP_STOREI1_MEMBASE_IMM:
		case OP_STOREI2_MEMBASE_IMM:
		case OP_STOREI4_MEMBASE_IMM:
		case OP_STOREI8_MEMBASE_IMM:
		case OP_STORE_MEMBASE_IMM: {
			int size = 8;
			LLVMValueRef index, addr, base;
			LLVMTypeRef t;
			gboolean sext = FALSE, zext = FALSE;
			gboolean is_faulting = (ins->flags & MONO_INST_FAULT) != 0;
			gboolean is_volatile = (ins->flags & MONO_INST_VOLATILE) != 0;
			gboolean is_unaligned = (ins->flags & MONO_INST_UNALIGNED) != 0;

			t = load_store_to_llvm_type (ins->opcode, &size, &sext, &zext);

			base = values [ins->inst_destbasereg];
			LLVMValueRef gep_base, gep_offset;
			if (ins->inst_offset == 0 && mono_llvm_can_be_gep (base, &gep_base, &gep_offset)) {
				addr = LLVMBuildGEP2 (builder, LLVMInt8Type (), convert (ctx, gep_base, LLVMPointerType (LLVMInt8Type (), 0)), &gep_offset, 1, "");
			} else if (ins->inst_offset % size != 0) {
				/* Unaligned store */
				index = LLVMConstInt (LLVMInt32Type (), ins->inst_offset, FALSE);
				addr = LLVMBuildGEP2 (builder, LLVMInt8Type (), convert (ctx, base, LLVMPointerType (LLVMInt8Type (), 0)), &index, 1, "");
			} else {
				index = LLVMConstInt (LLVMInt32Type (), ins->inst_offset / size, FALSE);
				addr = LLVMBuildGEP2 (builder, t, convert (ctx, base, LLVMPointerType (t, 0)), &index, 1, "");
			}
			LLVMValueRef srcval = convert (ctx, LLVMConstInt (IntPtrType (), ins->inst_imm, FALSE), t);
			LLVMValueRef ptrdst = convert (ctx, addr, LLVMPointerType (t, 0));
			if (is_unaligned)
				mono_llvm_build_aligned_store (builder, srcval, ptrdst, is_volatile, 1);
			else
				emit_store (ctx, bb, &builder, size, srcval, ptrdst, base, is_faulting, is_volatile);
			break;
		}

		case OP_CHECK_THIS:
			emit_load (ctx, bb, &builder, TARGET_SIZEOF_VOID_P, IntPtrType (), convert (ctx, lhs, LLVMPointerType (IntPtrType (), 0)), lhs, "", TRUE, FALSE, LLVM_BARRIER_NONE);
			break;
		case OP_OUTARG_VTRETADDR:
			break;
		case OP_VOIDCALL:
		case OP_CALL:
		case OP_LCALL:
		case OP_FCALL:
		case OP_RCALL:
		case OP_VCALL:
		case OP_VOIDCALL_MEMBASE:
		case OP_CALL_MEMBASE:
		case OP_LCALL_MEMBASE:
		case OP_FCALL_MEMBASE:
		case OP_RCALL_MEMBASE:
		case OP_VCALL_MEMBASE:
		case OP_VOIDCALL_REG:
		case OP_CALL_REG:
		case OP_LCALL_REG:
		case OP_FCALL_REG:
		case OP_RCALL_REG:
		case OP_VCALL_REG: {
			process_call (ctx, bb, &builder, ins);
			break;
		}
		case OP_AOTCONST: {
			MonoJumpInfoType ji_type = (MonoJumpInfoType)ins->inst_c1;
			gpointer ji_data = ins->inst_p0;

			if (ji_type == MONO_PATCH_INFO_ICALL_ADDR) {
				char *symbol = mono_aot_get_direct_call_symbol (MONO_PATCH_INFO_ICALL_ADDR_CALL, ji_data);
				if (symbol) {
					/*
					 * Avoid emitting a got entry for these since the method is directly called, and it might not be
					 * resolvable at runtime using dlsym ().
					 */
					g_free (symbol);
					values [ins->dreg] = LLVMConstInt (IntPtrType (), 0, FALSE);
					break;
				}
			}

			values [ins->dreg] = get_aotconst (ctx, ji_type, ji_data, LLVMPointerType (IntPtrType (), 0));
			break;
		}
		case OP_MEMMOVE: {
			int argn = 0;
			LLVMValueRef args [5];
			args [argn++] = convert (ctx, values [ins->sreg1], LLVMPointerType (LLVMInt8Type (), 0));
			args [argn++] = convert (ctx, values [ins->sreg2], LLVMPointerType (LLVMInt8Type (), 0));
			args [argn++] = convert (ctx, values [ins->sreg3], LLVMInt64Type ());
#if LLVM_API_VERSION < 900
			args [argn++] = LLVMConstInt (LLVMInt32Type (), 1, FALSE); // alignment
#endif
			args [argn++] = LLVMConstInt (LLVMInt1Type (), 0, FALSE);  // is_volatile

			call_intrins (ctx, INTRINS_MEMMOVE, args, "");
			break;
		}
		case OP_NOT_REACHED:
			LLVMBuildUnreachable (builder);
			has_terminator = TRUE;
			g_assert (bb->block_num < cfg->max_block_num);
			ctx->unreachable [bb->block_num] = TRUE;
			/* Might have instructions after this */
			while (ins->next) {
				MonoInst *next = ins->next;
				/* 
				 * FIXME: If later code uses the regs defined by these instructions,
				 * compilation will fail.
				 */
				const char *spec = INS_INFO (next->opcode);
				if (spec [MONO_INST_DEST] == 'i' && !MONO_IS_STORE_MEMBASE (next))
					ctx->values [next->dreg] = LLVMConstNull (LLVMInt32Type ());
				MONO_DELETE_INS (bb, next);
			}				
			break;
		case OP_LDADDR: {
			MonoInst *var = ins->inst_i0;
			MonoClass *klass = var->klass;

			if (var->opcode == OP_VTARG_ADDR && !MONO_CLASS_IS_SIMD(cfg, klass)) {
				/* The variable contains the vtype address */
				values [ins->dreg] = values [var->dreg];
			} else if (var->opcode == OP_GSHAREDVT_LOCAL) {
				values [ins->dreg] = emit_gsharedvt_ldaddr (ctx, var->dreg);
			} else {
				values [ins->dreg] = addresses [var->dreg]->value;
			}
			break;
		}
		case OP_SIN: {
			LLVMValueRef args [1];

			args [0] = convert (ctx, lhs, LLVMDoubleType ());
			values [ins->dreg] = call_intrins (ctx, INTRINS_SIN, args, dname);
			break;
		}
		case OP_SINF: {
			LLVMValueRef args [1];

			args [0] = convert (ctx, lhs, LLVMFloatType ());
			values [ins->dreg] = call_intrins (ctx, INTRINS_SINF, args, dname);
			break;
		}
		case OP_EXP: {
			LLVMValueRef args [1];

			args [0] = convert (ctx, lhs, LLVMDoubleType ());
			values [ins->dreg] = call_intrins (ctx, INTRINS_EXP, args, dname);
			break;
		}
		case OP_EXPF: {
			LLVMValueRef args [1];

			args [0] = convert (ctx, lhs, LLVMFloatType ());
			values [ins->dreg] = call_intrins (ctx, INTRINS_EXPF, args, dname);
			break;
		}
		case OP_LOG2: {
			LLVMValueRef args [1];

			args [0] = convert (ctx, lhs, LLVMDoubleType ());
			values [ins->dreg] = call_intrins (ctx, INTRINS_LOG2, args, dname);
			break;
		}
		case OP_LOG2F: {
			LLVMValueRef args [1];

			args [0] = convert (ctx, lhs, LLVMFloatType ());
			values [ins->dreg] = call_intrins (ctx, INTRINS_LOG2F, args, dname);
			break;
		}
		case OP_LOG10: {
			LLVMValueRef args [1];

			args [0] = convert (ctx, lhs, LLVMDoubleType ());
			values [ins->dreg] = call_intrins (ctx, INTRINS_LOG10, args, dname);
			break;
		}
		case OP_LOG10F: {
			LLVMValueRef args [1];

			args [0] = convert (ctx, lhs, LLVMFloatType ());
			values [ins->dreg] = call_intrins (ctx, INTRINS_LOG10F, args, dname);
			break;
		}
		case OP_LOG: {
			LLVMValueRef args [1];

			args [0] = convert (ctx, lhs, LLVMDoubleType ());
			values [ins->dreg] = call_intrins (ctx, INTRINS_LOG, args, dname);
			break;
		}
		case OP_TRUNC: {
			LLVMValueRef args [1];

			args [0] = convert (ctx, lhs, LLVMDoubleType ());
			values [ins->dreg] = call_intrins (ctx, INTRINS_TRUNC, args, dname);
			break;
		}
		case OP_TRUNCF: {
			LLVMValueRef args [1];

			args [0] = convert (ctx, lhs, LLVMFloatType ());
			values [ins->dreg] = call_intrins (ctx, INTRINS_TRUNCF, args, dname);
			break;
		}
		case OP_COS: {
			LLVMValueRef args [1];

			args [0] = convert (ctx, lhs, LLVMDoubleType ());
			values [ins->dreg] = call_intrins (ctx, INTRINS_COS, args, dname);
			break;
		}
		case OP_COSF: {
			LLVMValueRef args [1];

			args [0] = convert (ctx, lhs, LLVMFloatType ());
			values [ins->dreg] = call_intrins (ctx, INTRINS_COSF, args, dname);
			break;
		}
		case OP_SQRT: {
			LLVMValueRef args [1];

			args [0] = convert (ctx, lhs, LLVMDoubleType ());
			values [ins->dreg] = call_intrins (ctx, INTRINS_SQRT, args, dname);
			break;
		}
		case OP_SQRTF: {
			LLVMValueRef args [1];

			args [0] = convert (ctx, lhs, LLVMFloatType ());
			values [ins->dreg] = call_intrins (ctx, INTRINS_SQRTF, args, dname);
			break;
		}
		case OP_FLOOR: {
			LLVMValueRef args [1];

			args [0] = convert (ctx, lhs, LLVMDoubleType ());
			values [ins->dreg] = call_intrins (ctx, INTRINS_FLOOR, args, dname);
			break;
		}
		case OP_FLOORF: {
			LLVMValueRef args [1];

			args [0] = convert (ctx, lhs, LLVMFloatType ());
			values [ins->dreg] = call_intrins (ctx, INTRINS_FLOORF, args, dname);
			break;
		}
		case OP_CEIL: {
			LLVMValueRef args [1];

			args [0] = convert (ctx, lhs, LLVMDoubleType ());
			values [ins->dreg] = call_intrins (ctx, INTRINS_CEIL, args, dname);
			break;
		}
		case OP_CEILF: {
			LLVMValueRef args [1];

			args [0] = convert (ctx, lhs, LLVMFloatType ());
			values [ins->dreg] = call_intrins (ctx, INTRINS_CEILF, args, dname);
			break;
		}
		case OP_FMA: {
			LLVMValueRef args [3];

			args [0] = convert (ctx, values [ins->sreg1], LLVMDoubleType ());
			args [1] = convert (ctx, values [ins->sreg2], LLVMDoubleType ());
			args [2] = convert (ctx, values [ins->sreg3], LLVMDoubleType ());
			
			values [ins->dreg] = call_intrins (ctx, INTRINS_FMA, args, dname);
			break;
		}
		case OP_FMAF: {
			LLVMValueRef args [3];

			args [0] = convert (ctx, values [ins->sreg1], LLVMFloatType ());
			args [1] = convert (ctx, values [ins->sreg2], LLVMFloatType ());
			args [2] = convert (ctx, values [ins->sreg3], LLVMFloatType ());
			
			values [ins->dreg] = call_intrins (ctx, INTRINS_FMAF, args, dname);
			break;
		}
		case OP_ABS: {
			LLVMValueRef args [1];

			args [0] = convert (ctx, lhs, LLVMDoubleType ());
			values [ins->dreg] = call_intrins (ctx, INTRINS_FABS, args, dname);
			break;
		}
		case OP_ABSF: {
			LLVMValueRef args [1];

#ifdef TARGET_AMD64
			args [0] = convert (ctx, lhs, LLVMFloatType ());
			values [ins->dreg] = call_intrins (ctx, INTRINS_ABSF, args, dname);
#else
			/* llvm.fabs not supported on all platforms */
			args [0] = convert (ctx, lhs, LLVMDoubleType ());
			values [ins->dreg] = call_intrins (ctx, INTRINS_FABS, args, dname);
			values [ins->dreg] = convert (ctx, values [ins->dreg], LLVMFloatType ());
#endif
			break;
		}
		case OP_RPOW: {
			LLVMValueRef args [2];

			args [0] = convert (ctx, lhs, LLVMFloatType ());
			args [1] = convert (ctx, rhs, LLVMFloatType ());
			values [ins->dreg] = call_intrins (ctx, INTRINS_POWF, args, dname);
			break;
		}
		case OP_FPOW: {
			LLVMValueRef args [2];

			args [0] = convert (ctx, lhs, LLVMDoubleType ());
			args [1] = convert (ctx, rhs, LLVMDoubleType ());
			values [ins->dreg] = call_intrins (ctx, INTRINS_POW, args, dname);
			break;
		}
		case OP_FCOPYSIGN: {
			LLVMValueRef args [2];

			args [0] = convert (ctx, lhs, LLVMDoubleType ());
			args [1] = convert (ctx, rhs, LLVMDoubleType ());
			values [ins->dreg] = call_intrins (ctx, INTRINS_COPYSIGN, args, dname);
			break;
		}
		case OP_RCOPYSIGN: {
			LLVMValueRef args [2];

			args [0] = convert (ctx, lhs, LLVMFloatType ());
			args [1] = convert (ctx, rhs, LLVMFloatType ());
			values [ins->dreg] = call_intrins (ctx, INTRINS_COPYSIGNF, args, dname);
			break;
		}

		case OP_IMIN:
		case OP_LMIN:
		case OP_IMAX:
		case OP_LMAX:
		case OP_IMIN_UN:
		case OP_LMIN_UN:
		case OP_IMAX_UN:
		case OP_LMAX_UN:
		case OP_FMIN:
		case OP_FMAX:
		case OP_RMIN:
		case OP_RMAX: {
			LLVMValueRef v;

			lhs = convert (ctx, lhs, regtype_to_llvm_type (spec [MONO_INST_DEST]));
			rhs = convert (ctx, rhs, regtype_to_llvm_type (spec [MONO_INST_DEST]));

			switch (ins->opcode) {
			case OP_IMIN:
			case OP_LMIN:
				v = LLVMBuildICmp (builder, LLVMIntSLE, lhs, rhs, "");
				break;
			case OP_IMAX:
			case OP_LMAX:
				v = LLVMBuildICmp (builder, LLVMIntSGE, lhs, rhs, "");
				break;
			case OP_IMIN_UN:
			case OP_LMIN_UN:
				v = LLVMBuildICmp (builder, LLVMIntULE, lhs, rhs, "");
				break;
			case OP_IMAX_UN:
			case OP_LMAX_UN:
				v = LLVMBuildICmp (builder, LLVMIntUGE, lhs, rhs, "");
				break;
			case OP_FMAX:
			case OP_RMAX:
				v = LLVMBuildFCmp (builder, LLVMRealUGE, lhs, rhs, "");
				break;
			case OP_FMIN:
			case OP_RMIN:
				v = LLVMBuildFCmp (builder, LLVMRealULE, lhs, rhs, "");
				break;
			default:
				g_assert_not_reached ();
				break;
			}
			values [ins->dreg] = LLVMBuildSelect (builder, v, lhs, rhs, dname);
			break;
		}

/*
 * See the ARM64 comment in mono/utils/atomic.h for an explanation of why this
 * hack is necessary (for now).
 */
#ifdef TARGET_ARM64
#define ARM64_ATOMIC_FENCE_FIX mono_llvm_build_fence (builder, LLVM_BARRIER_SEQ)
#else
#define ARM64_ATOMIC_FENCE_FIX
#endif

		case OP_ATOMIC_EXCHANGE_I4:
		case OP_ATOMIC_EXCHANGE_I8: {
			LLVMValueRef args [2];
			LLVMTypeRef t;
				
			if (ins->opcode == OP_ATOMIC_EXCHANGE_I4)
				t = LLVMInt32Type ();
			else
				t = LLVMInt64Type ();

			g_assert (ins->inst_offset == 0);

			args [0] = convert (ctx, lhs, LLVMPointerType (t, 0));
			args [1] = convert (ctx, rhs, t);

			ARM64_ATOMIC_FENCE_FIX;
			values [ins->dreg] = mono_llvm_build_atomic_rmw (builder, LLVM_ATOMICRMW_OP_XCHG, args [0], args [1]);
			ARM64_ATOMIC_FENCE_FIX;
			break;
		}
		case OP_ATOMIC_ADD_I4:
		case OP_ATOMIC_ADD_I8:
		case OP_ATOMIC_AND_I4:
		case OP_ATOMIC_AND_I8:
		case OP_ATOMIC_OR_I4:
		case OP_ATOMIC_OR_I8: {
			LLVMValueRef args [2];
			LLVMTypeRef t;

			if (ins->type == STACK_I4)
				t = LLVMInt32Type ();
			else
				t = LLVMInt64Type ();

			g_assert (ins->inst_offset == 0);

			args [0] = convert (ctx, lhs, LLVMPointerType (t, 0));
			args [1] = convert (ctx, rhs, t);
			ARM64_ATOMIC_FENCE_FIX;
			if (ins->opcode == OP_ATOMIC_ADD_I4 || ins->opcode == OP_ATOMIC_ADD_I8)
				// Interlocked.Add returns new value (that's why we emit additional Add here)
				// see https://github.com/dotnet/runtime/pull/33102
				values [ins->dreg] = LLVMBuildAdd (builder, mono_llvm_build_atomic_rmw (builder, LLVM_ATOMICRMW_OP_ADD, args [0], args [1]), args [1], dname);
			else if (ins->opcode == OP_ATOMIC_AND_I4 || ins->opcode == OP_ATOMIC_AND_I8)
				values [ins->dreg] = mono_llvm_build_atomic_rmw (builder, LLVM_ATOMICRMW_OP_AND, args [0], args [1]);
			else if (ins->opcode == OP_ATOMIC_OR_I4 || ins->opcode == OP_ATOMIC_OR_I8)
				values [ins->dreg] = mono_llvm_build_atomic_rmw (builder, LLVM_ATOMICRMW_OP_OR, args [0], args [1]);
			else
				g_assert_not_reached ();
			ARM64_ATOMIC_FENCE_FIX;
			break;
		}
		case OP_ATOMIC_CAS_I4:
		case OP_ATOMIC_CAS_I8: {
			LLVMValueRef args [3], val;
			LLVMTypeRef t;
				
			if (ins->opcode == OP_ATOMIC_CAS_I4)
				t = LLVMInt32Type ();
			else
				t = LLVMInt64Type ();

			args [0] = convert (ctx, lhs, LLVMPointerType (t, 0));
			/* comparand */
			args [1] = convert (ctx, values [ins->sreg3], t);
			/* new value */
			args [2] = convert (ctx, values [ins->sreg2], t);
			ARM64_ATOMIC_FENCE_FIX;
			val = mono_llvm_build_cmpxchg (builder, args [0], args [1], args [2]);
			ARM64_ATOMIC_FENCE_FIX;
			/* cmpxchg returns a pair */
			values [ins->dreg] = LLVMBuildExtractValue (builder, val, 0, "");
			break;
		}
		case OP_MEMORY_BARRIER: {
			mono_llvm_build_fence (builder, (BarrierKind) ins->backend.memory_barrier_kind);
			break;
		}
		case OP_ATOMIC_LOAD_I1:
		case OP_ATOMIC_LOAD_I2:
		case OP_ATOMIC_LOAD_I4:
		case OP_ATOMIC_LOAD_I8:
		case OP_ATOMIC_LOAD_U1:
		case OP_ATOMIC_LOAD_U2:
		case OP_ATOMIC_LOAD_U4:
		case OP_ATOMIC_LOAD_U8:
		case OP_ATOMIC_LOAD_R4:
		case OP_ATOMIC_LOAD_R8: {
			int size;
			gboolean sext, zext;
			LLVMTypeRef t;
			gboolean is_faulting = (ins->flags & MONO_INST_FAULT) != 0;
			gboolean is_volatile = (ins->flags & MONO_INST_VOLATILE) != 0;
			BarrierKind barrier = (BarrierKind) ins->backend.memory_barrier_kind;
			LLVMValueRef index, addr;

			t = load_store_to_llvm_type (ins->opcode, &size, &sext, &zext);

			if (sext || zext)
				dname = (char *)"";

			if (ins->inst_offset != 0) {
				index = LLVMConstInt (LLVMInt32Type (), ins->inst_offset / size, FALSE);
				addr = LLVMBuildGEP2 (builder, t, convert (ctx, lhs, LLVMPointerType (t, 0)), &index, 1, "");
			} else {
				addr = lhs;
			}

			addr = convert (ctx, addr, LLVMPointerType (t, 0));

			ARM64_ATOMIC_FENCE_FIX;
			values [ins->dreg] = emit_load (ctx, bb, &builder, size, t, addr, lhs, dname, is_faulting, is_volatile, barrier);
			ARM64_ATOMIC_FENCE_FIX;

			if (sext)
				values [ins->dreg] = LLVMBuildSExt (builder, values [ins->dreg], LLVMInt32Type (), dname);
			else if (zext)
				values [ins->dreg] = LLVMBuildZExt (builder, values [ins->dreg], LLVMInt32Type (), dname);
			break;
		}
		case OP_ATOMIC_STORE_I1:
		case OP_ATOMIC_STORE_I2:
		case OP_ATOMIC_STORE_I4:
		case OP_ATOMIC_STORE_I8:
		case OP_ATOMIC_STORE_U1:
		case OP_ATOMIC_STORE_U2:
		case OP_ATOMIC_STORE_U4:
		case OP_ATOMIC_STORE_U8:
		case OP_ATOMIC_STORE_R4:
		case OP_ATOMIC_STORE_R8: {
			int size;
			gboolean sext, zext;
			LLVMTypeRef t;
			gboolean is_faulting = (ins->flags & MONO_INST_FAULT) != 0;
			gboolean is_volatile = (ins->flags & MONO_INST_VOLATILE) != 0;
			BarrierKind barrier = (BarrierKind) ins->backend.memory_barrier_kind;
			LLVMValueRef index, addr, value, base;

			if (!values [ins->inst_destbasereg]) {
			    set_failure (ctx, "inst_destbasereg");
				break;
			}

			t = load_store_to_llvm_type (ins->opcode, &size, &sext, &zext);

			base = values [ins->inst_destbasereg];
			index = LLVMConstInt (LLVMInt32Type (), ins->inst_offset / size, FALSE);
			addr = LLVMBuildGEP2 (builder, t, convert (ctx, base, LLVMPointerType (t, 0)), &index, 1, "");
			value = convert (ctx, values [ins->sreg1], t);

			ARM64_ATOMIC_FENCE_FIX;
			emit_store_general (ctx, bb, &builder, size, value, addr, base, is_faulting, is_volatile, barrier);
			ARM64_ATOMIC_FENCE_FIX;
			break;
		}
		case OP_RELAXED_NOP: {
#if defined(TARGET_AMD64) || defined(TARGET_X86)
			call_intrins (ctx, INTRINS_SSE_PAUSE, NULL, "");
			break;
#else
			break;
#endif
		}
		case OP_TLS_GET: {
#if (defined(TARGET_AMD64) || defined(TARGET_X86)) && defined(__linux__)
#ifdef TARGET_AMD64
			// 257 == FS segment register
			LLVMTypeRef ptrtype = LLVMPointerType (IntPtrType (), 257);
#else
			// 256 == GS segment register
			LLVMTypeRef ptrtype = LLVMPointerType (IntPtrType (), 256);
#endif
			// FIXME: XEN
			values [ins->dreg] = LLVMBuildLoad2 (builder, IntPtrType (), LLVMBuildIntToPtr (builder, LLVMConstInt (IntPtrType (), ins->inst_offset, TRUE), ptrtype, ""), "");
#elif defined(TARGET_AMD64) && defined(TARGET_OSX)
			/* See mono_amd64_emit_tls_get () */
			int offset = mono_amd64_get_tls_gs_offset () + (ins->inst_offset * 8);

			// 256 == GS segment register
			LLVMTypeRef ptrtype = LLVMPointerType (IntPtrType (), 256);
			values [ins->dreg] = LLVMBuildLoad2 (builder, IntPtrType (), LLVMBuildIntToPtr (builder, LLVMConstInt (IntPtrType (), offset, TRUE), ptrtype, ""), "");
#else
			set_failure (ctx, "opcode tls-get");
			break;
#endif

			break;
		}
		case OP_GC_SAFE_POINT: {
			LLVMValueRef val, cmp, callee, call;
			LLVMBasicBlockRef poll_bb, cont_bb;
			LLVMValueRef args [2];
			static LLVMTypeRef sig;
			const char *icall_name = "mono_threads_state_poll";

			/*
			 * Create the cold wrapper around the icall, along with a managed method for it so
			 * unwinding works.
			 */
			if (!ctx->module->gc_poll_cold_wrapper_compiled) {
				ERROR_DECL (error);
				/* Compiling a method here is a bit ugly, but it works */
				MonoMethod *wrapper = mono_marshal_get_llvm_func_wrapper (LLVM_FUNC_WRAPPER_GC_POLL);
				ctx->module->gc_poll_cold_wrapper_compiled = mono_jit_compile_method (wrapper, error);
				mono_error_assert_ok (error);
			}

			if (!sig)
				sig = LLVMFunctionType0 (LLVMVoidType (), FALSE);

			/*
			 * if (!*sreg1)
			 *   mono_threads_state_poll ();
			 */
			val = mono_llvm_build_load (builder, IntPtrType (), convert (ctx, lhs, LLVMPointerType (IntPtrType (), 0)), "", TRUE);
			cmp = LLVMBuildICmp (builder, LLVMIntEQ, val, LLVMConstNull (LLVMTypeOf (val)), "");
			poll_bb = gen_bb (ctx, "POLL_BB");
			cont_bb = gen_bb (ctx, "CONT_BB");

			args [0] = cmp;
			args [1] = LLVMConstInt (LLVMInt1Type (), 1, FALSE);
			cmp = call_intrins (ctx, INTRINS_EXPECT_I1, args, "");

			mono_llvm_build_weighted_branch (builder, cmp, cont_bb, poll_bb, 1000, 1);

			ctx->builder = builder = create_builder (ctx);
			LLVMPositionBuilderAtEnd (builder, poll_bb);

			callee = get_jit_callee (ctx, icall_name, sig, MONO_PATCH_INFO_ABS, ctx->module->gc_poll_cold_wrapper_compiled);
			call = LLVMBuildCall2 (builder, sig, callee, NULL, 0, "");
			set_call_cold_cconv (call);
			LLVMBuildBr (builder, cont_bb);

			ctx->builder = builder = create_builder (ctx);
			LLVMPositionBuilderAtEnd (builder, cont_bb);
			ctx->bblocks [bb->block_num].end_bblock = cont_bb;
			break;
		}

			/*
			 * Overflow opcodes.
			 */
		case OP_IADD_OVF:
		case OP_IADD_OVF_UN:
		case OP_ISUB_OVF:
		case OP_ISUB_OVF_UN:
		case OP_IMUL_OVF:
		case OP_IMUL_OVF_UN:
		case OP_LADD_OVF:
		case OP_LADD_OVF_UN:
		case OP_LSUB_OVF:
		case OP_LSUB_OVF_UN:
		case OP_LMUL_OVF:
		case OP_LMUL_OVF_UN: {
			LLVMValueRef args [2], val, ovf;
			IntrinsicId intrins;

			args [0] = convert (ctx, lhs, op_to_llvm_type (ins->opcode));
			args [1] = convert (ctx, rhs, op_to_llvm_type (ins->opcode));
			intrins = ovf_op_to_intrins (ins->opcode);
			val = call_intrins (ctx, intrins, args, "");
			values [ins->dreg] = LLVMBuildExtractValue (builder, val, 0, dname);
			ovf = LLVMBuildExtractValue (builder, val, 1, "");
			emit_cond_system_exception (ctx, bb, "OverflowException", ovf, FALSE);
			if (!ctx_ok (ctx))
				break;
			builder = ctx->builder;
			break;
		}

		/* 
		 * Valuetypes.
		 *   We currently model them using arrays. Promotion to local vregs is 
		 * disabled for them in mono_handle_global_vregs () in the LLVM case, 
		 * so we always have an entry in cfg->varinfo for them.
		 * FIXME: Is this needed ?
		 */
		case OP_VZERO: {
			MonoClass *klass = ins->klass;

			if (!klass) {
				// FIXME:
				set_failure (ctx, "!klass");
				break;
			}

			if (!addresses [ins->dreg])
				addresses [ins->dreg] = build_named_alloca_address (ctx, m_class_get_byval_arg (klass), "vzero");
			LLVMValueRef ptr = LLVMBuildBitCast (builder, addresses [ins->dreg]->value, LLVMPointerType (LLVMInt8Type (), 0), "");
			emit_memset (ctx, builder, ptr, const_int32 (mono_class_value_size (klass, NULL)), 0);
			break;
		}
		case OP_DUMMY_VZERO:
			break;

		case OP_STOREV_MEMBASE:
		case OP_LOADV_MEMBASE:
		case OP_VMOVE: {
			MonoClass *klass = ins->klass;
			LLVMValueRef src = NULL, dst, args [5];
			gboolean done = FALSE;

			if (!klass) {
				// FIXME:
				set_failure (ctx, "!klass");
				break;
			}

			if (mini_is_gsharedvt_klass (klass)) {
				// FIXME:
				set_failure (ctx, "gsharedvt");
				break;
			}

			switch (ins->opcode) {
			case OP_STOREV_MEMBASE:
				if (cfg->gen_write_barriers && m_class_has_references (klass) && ins->inst_destbasereg != cfg->frame_reg &&
					LLVMGetInstructionOpcode (values [ins->inst_destbasereg]) != LLVMAlloca) {
					/* Decomposed earlier */
					g_assert_not_reached ();
					break;
				}
				if (!addresses [ins->sreg1]) {
					/* SIMD */
					g_assert (values [ins->sreg1]);
					dst = convert (ctx, LLVMBuildAdd (builder, convert (ctx, values [ins->inst_destbasereg], IntPtrType ()), LLVMConstInt (IntPtrType (), ins->inst_offset, FALSE), ""), LLVMPointerType (type_to_llvm_type (ctx, m_class_get_byval_arg (klass)), 0));
					LLVMBuildStore (builder, values [ins->sreg1], dst);
					done = TRUE;
				} else {
					src = LLVMBuildBitCast (builder, addresses [ins->sreg1]->value, LLVMPointerType (LLVMInt8Type (), 0), "");
					dst = convert (ctx, LLVMBuildAdd (builder, convert (ctx, values [ins->inst_destbasereg], IntPtrType ()), LLVMConstInt (IntPtrType (), ins->inst_offset, FALSE), ""), LLVMPointerType (LLVMInt8Type (), 0));
				}
				break;
			case OP_LOADV_MEMBASE:
				if (!addresses [ins->dreg])
					addresses [ins->dreg] = build_alloca_address (ctx, m_class_get_byval_arg (klass));
				src = convert (ctx, LLVMBuildAdd (builder, convert (ctx, values [ins->inst_basereg], IntPtrType ()), LLVMConstInt (IntPtrType (), ins->inst_offset, FALSE), ""), LLVMPointerType (LLVMInt8Type (), 0));
				dst = LLVMBuildBitCast (builder, addresses [ins->dreg]->value, LLVMPointerType (LLVMInt8Type (), 0), "");
				break;
			case OP_VMOVE:
				if (!addresses [ins->sreg1])
					addresses [ins->sreg1] = build_alloca_address (ctx, m_class_get_byval_arg (klass));
				if (!addresses [ins->dreg])
					addresses [ins->dreg] = build_alloca_address (ctx, m_class_get_byval_arg (klass));
				src = LLVMBuildBitCast (builder, addresses [ins->sreg1]->value, LLVMPointerType (LLVMInt8Type (), 0), "");
				dst = LLVMBuildBitCast (builder, addresses [ins->dreg]->value, LLVMPointerType (LLVMInt8Type (), 0), "");
				break;
			default:
				g_assert_not_reached ();
			}
			if (!ctx_ok (ctx))
				break;

			if (done)
				break;

			int aindex = 0;
			args [aindex ++] = dst;
			args [aindex ++] = src;
			args [aindex ++] = LLVMConstInt (LLVMInt32Type (), mono_class_value_size (klass, NULL), FALSE);
#if LLVM_API_VERSION < 900
			// FIXME: Alignment
			args [aindex ++] = LLVMConstInt (LLVMInt32Type (), 0, FALSE);
#endif
			args [aindex ++] = LLVMConstInt (LLVMInt1Type (), 0, FALSE);
			call_intrins (ctx, INTRINS_MEMCPY, args, "");
			break;
		}
		case OP_LLVM_OUTARG_VT: {
			LLVMArgInfo *ainfo = (LLVMArgInfo*)ins->inst_p0;
			MonoType *t = mini_get_underlying_type (ins->inst_vtype);

			if (ainfo->storage == LLVMArgGsharedvtVariable) {
					MonoInst *var = get_vreg_to_inst (cfg, ins->sreg1);

					if (var && var->opcode == OP_GSHAREDVT_LOCAL) {
						addresses [ins->dreg] = create_address (ctx, convert (ctx, emit_gsharedvt_ldaddr (ctx, var->dreg), LLVMPointerType (IntPtrType (), 0)), IntPtrType ());
					} else {
						g_assert (addresses [ins->sreg1]);
						addresses [ins->dreg] = addresses [ins->sreg1];
					}
			} else if (ainfo->storage == LLVMArgGsharedvtFixed) {
				if (!addresses [ins->sreg1]) {
					addresses [ins->sreg1] = build_alloca_address (ctx, t);
					g_assert (values [ins->sreg1]);
				}
				/* Use the recorded element type, not a re-derivation (donor build_alloca_address form) */
				LLVMBuildStore (builder, convert (ctx, values [ins->sreg1], addresses [ins->sreg1]->type), addresses [ins->sreg1]->value);
				addresses [ins->dreg] = addresses [ins->sreg1];
			} else {
				LLVMTypeRef etype = type_to_llvm_type (ctx, t);

				if (!addresses [ins->sreg1]) {
					addresses [ins->sreg1] = build_alloca_address (ctx, t);
					g_assert (values [ins->sreg1]);
					LLVMBuildStore (builder, convert (ctx, values [ins->sreg1], etype), addresses [ins->sreg1]->value);
					addresses [ins->dreg] = addresses [ins->sreg1];
				} else if (ainfo->storage == LLVMArgVtypeAddr || values [ins->sreg1] == addresses [ins->sreg1]->value) {
					/* LLVMArgVtypeByRef/LLVMArgVtypeAddr, have to make a copy */
					addresses [ins->dreg] = build_alloca_address (ctx, t);
					LLVMValueRef v = LLVMBuildLoad2 (builder, etype, addresses [ins->sreg1]->value, "");
					LLVMBuildStore (builder, convert (ctx, v, etype), addresses [ins->dreg]->value);
				} else {
					addresses [ins->dreg] = addresses [ins->sreg1];
				}
			}
			break;
		}
		case OP_OBJC_GET_SELECTOR: {
			const char *name = (const char*)ins->inst_p0;
			LLVMValueRef var;

			if (!ctx->module->objc_selector_to_var) {
				ctx->module->objc_selector_to_var = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);

				LLVMValueRef info_var = LLVMAddGlobal (ctx->lmodule, LLVMArrayType (LLVMInt8Type (), 8), "@OBJC_IMAGE_INFO");
				int32_t objc_imageinfo [] = { 0, 16 };
				LLVMSetInitializer (info_var, mono_llvm_create_constant_data_array ((uint8_t *) &objc_imageinfo, 8));
				LLVMSetLinkage (info_var, LLVMPrivateLinkage);
				LLVMSetExternallyInitialized (info_var, TRUE);
				LLVMSetSection (info_var, "__DATA, __objc_imageinfo,regular,no_dead_strip");
				LLVMSetAlignment (info_var, sizeof (target_mgreg_t));
				mark_as_used (ctx->module, info_var);
			}

			var = (LLVMValueRef)g_hash_table_lookup (ctx->module->objc_selector_to_var, name);
			if (!var) {
				LLVMValueRef indexes [16];

				LLVMValueRef name_var = LLVMAddGlobal (ctx->lmodule, LLVMArrayType (LLVMInt8Type (), strlen (name) + 1), "@OBJC_METH_VAR_NAME_");
				LLVMSetInitializer (name_var, mono_llvm_create_constant_data_array ((const uint8_t*)name, strlen (name) + 1));
				LLVMSetLinkage (name_var, LLVMPrivateLinkage);
				LLVMSetSection (name_var, "__TEXT,__objc_methname,cstring_literals");
				mark_as_used (ctx->module, name_var);

				LLVMValueRef ref_var = LLVMAddGlobal (ctx->lmodule, LLVMPointerType (LLVMInt8Type (), 0), "@OBJC_SELECTOR_REFERENCES_");

				indexes [0] = LLVMConstInt (LLVMInt32Type (), 0, 0);
				indexes [1] = LLVMConstInt (LLVMInt32Type (), 0, 0);
				LLVMSetInitializer (ref_var, LLVMConstGEP2 (LLVMGlobalGetValueType (name_var), name_var, indexes, 2));
				LLVMSetLinkage (ref_var, LLVMPrivateLinkage);
				LLVMSetExternallyInitialized (ref_var, TRUE);
				LLVMSetSection (ref_var, "__DATA, __objc_selrefs, literal_pointers, no_dead_strip");
				LLVMSetAlignment (ref_var, sizeof (target_mgreg_t));
				mark_as_used (ctx->module, ref_var);

				g_hash_table_insert (ctx->module->objc_selector_to_var, g_strdup (name), ref_var);
				var = ref_var;
			}

			values [ins->dreg] = LLVMBuildLoad2 (builder, LLVMPointerType (LLVMInt8Type (), 0), var, "");
			break;
		}

			/* 
			 * SIMD
			 */
#if defined(TARGET_X86) || defined(TARGET_AMD64) || defined(TARGET_ARM64) || defined(TARGET_WASM)
		case OP_EXPAND_I1:
		case OP_EXPAND_I2:
		case OP_EXPAND_I4:
		case OP_EXPAND_I8:
		case OP_EXPAND_R4:
		case OP_EXPAND_R8: {
			LLVMTypeRef t;
			LLVMValueRef mask [32], v;
			int i;

#ifdef ENABLE_NETCORE
			t = simd_class_to_llvm_type (ctx, ins->klass);
#else
			t = simd_op_to_llvm_type (ins->opcode);
#endif
			for (i = 0; i < 32; ++i)
				mask [i] = LLVMConstInt (LLVMInt32Type (), 0, FALSE);

			v = convert (ctx, values [ins->sreg1], LLVMGetElementType (t));

			values [ins->dreg] = LLVMBuildInsertElement (builder, LLVMConstNull (t), v, LLVMConstInt (LLVMInt32Type (), 0, FALSE), "");
			values [ins->dreg] = LLVMBuildShuffleVector (builder, values [ins->dreg], LLVMGetUndef (t), LLVMConstVector (mask, LLVMGetVectorSize (t)), "");
			break;
		}
		case OP_XZERO: {
			values [ins->dreg] = LLVMConstNull (type_to_llvm_type (ctx, m_class_get_byval_arg (ins->klass)));
			break;
		}
		case OP_LOADX_MEMBASE: {
			LLVMTypeRef t = type_to_llvm_type (ctx, m_class_get_byval_arg (ins->klass));
			LLVMValueRef src;

			src = convert (ctx, LLVMBuildAdd (builder, convert (ctx, values [ins->inst_basereg], IntPtrType ()), LLVMConstInt (IntPtrType (), ins->inst_offset, FALSE), ""), LLVMPointerType (t, 0));
			values [ins->dreg] = mono_llvm_build_aligned_load (builder, t, src, "", FALSE, 1);
			break;
		}
		case OP_STOREX_MEMBASE: {
			LLVMTypeRef t = LLVMTypeOf (values [ins->sreg1]);
			LLVMValueRef dest;

			dest = convert (ctx, LLVMBuildAdd (builder, convert (ctx, values [ins->inst_destbasereg], IntPtrType ()), LLVMConstInt (IntPtrType (), ins->inst_offset, FALSE), ""), LLVMPointerType (t, 0));
			mono_llvm_build_aligned_store (builder, values [ins->sreg1], dest, FALSE, 1);
			break;
		}
#endif // defined(TARGET_X86) || defined(TARGET_AMD64) || defined(TARGET_ARM64) || defined(TARGET_WASM)

#if defined(TARGET_X86) || defined(TARGET_AMD64) || defined(TARGET_WASM)
		case OP_PADDB:
		case OP_PADDW:
		case OP_PADDD:
		case OP_PADDQ:
			values [ins->dreg] = LLVMBuildAdd (builder, lhs, rhs, "");
			break;
		case OP_ADDPD:
		case OP_ADDPS:
			values [ins->dreg] = LLVMBuildFAdd (builder, lhs, rhs, "");
			break;
		case OP_PSUBB:
		case OP_PSUBW:
		case OP_PSUBD:
		case OP_PSUBQ:
			values [ins->dreg] = LLVMBuildSub (builder, lhs, rhs, "");
			break;
		case OP_SUBPD:
		case OP_SUBPS:
			values [ins->dreg] = LLVMBuildFSub (builder, lhs, rhs, "");
			break;
		case OP_MULPD:
		case OP_MULPS:
			values [ins->dreg] = LLVMBuildFMul (builder, lhs, rhs, "");
			break;
		case OP_DIVPD:
		case OP_DIVPS:
			values [ins->dreg] = LLVMBuildFDiv (builder, lhs, rhs, "");
			break;
		case OP_PAND:
			values [ins->dreg] = LLVMBuildAnd (builder, lhs, rhs, "");
			break;
		case OP_POR:
			values [ins->dreg] = LLVMBuildOr (builder, lhs, rhs, "");
			break;
		case OP_PXOR:
			values [ins->dreg] = LLVMBuildXor (builder, lhs, rhs, "");
			break;
		case OP_PMULW:
		case OP_PMULD:
			values [ins->dreg] = LLVMBuildMul (builder, lhs, rhs, "");
			break;
		case OP_ANDPS:
		case OP_ANDNPS:
		case OP_ORPS:
		case OP_XORPS:
		case OP_ANDPD:
		case OP_ANDNPD:
		case OP_ORPD:
		case OP_XORPD: {
			LLVMTypeRef t, rt;
			LLVMValueRef v = NULL;

			switch (ins->opcode) {
			case OP_ANDPS:
			case OP_ANDNPS:
			case OP_ORPS:
			case OP_XORPS:
				t = LLVMVectorType (LLVMInt32Type (), 4);
				rt = LLVMVectorType (LLVMFloatType (), 4);
				break;
			case OP_ANDPD:
			case OP_ANDNPD:
			case OP_ORPD:
			case OP_XORPD:
				t = LLVMVectorType (LLVMInt64Type (), 2);
				rt = LLVMVectorType (LLVMDoubleType (), 2);
				break;
			default:
				t = LLVMInt32Type ();
				rt = LLVMInt32Type ();
				g_assert_not_reached ();
			}

			lhs = LLVMBuildBitCast (builder, lhs, t, "");
			rhs = LLVMBuildBitCast (builder, rhs, t, "");
			switch (ins->opcode) {
			case OP_ANDPS:
			case OP_ANDPD:
				v = LLVMBuildAnd (builder, lhs, rhs, "");
				break;
			case OP_ORPS:
			case OP_ORPD:
				v = LLVMBuildOr (builder, lhs, rhs, "");
				break;
			case OP_XORPS:
			case OP_XORPD:
				v = LLVMBuildXor (builder, lhs, rhs, "");
				break;
			case OP_ANDNPS:
			case OP_ANDNPD:
				v = LLVMBuildAnd (builder, rhs, LLVMBuildNot (builder, lhs, ""), "");
				break;
			}
			values [ins->dreg] = LLVMBuildBitCast (builder, v, rt, "");
			break;
		}
		case OP_PMIND_UN:
		case OP_PMINW_UN:
		case OP_PMINB_UN: {
			LLVMValueRef cmp = LLVMBuildICmp (builder, LLVMIntULT, lhs, rhs, "");
			values [ins->dreg] = LLVMBuildSelect (builder, cmp, lhs, rhs, "");
			break;
		}
		case OP_PMAXD_UN:
		case OP_PMAXW_UN:
		case OP_PMAXB_UN: {
			LLVMValueRef cmp = LLVMBuildICmp (builder, LLVMIntUGT, lhs, rhs, "");
			values [ins->dreg] = LLVMBuildSelect (builder, cmp, lhs, rhs, "");
			break;
		}
		case OP_PMINW: {
			LLVMValueRef cmp = LLVMBuildICmp (builder, LLVMIntSLT, lhs, rhs, "");
			values [ins->dreg] = LLVMBuildSelect (builder, cmp, lhs, rhs, "");
			break;
		}
		case OP_PMAXW: {
			LLVMValueRef cmp = LLVMBuildICmp (builder, LLVMIntSGT, lhs, rhs, "");
			values [ins->dreg] = LLVMBuildSelect (builder, cmp, lhs, rhs, "");
			break;
		}
		case OP_MINPD:
		case OP_MINPS:
		case OP_MAXPD:
		case OP_MAXPS:
		case OP_ADDSUBPD:
		case OP_ADDSUBPS:
		case OP_HADDPD:
		case OP_HADDPS:
		case OP_HSUBPD:
		case OP_HSUBPS:
		case OP_PACKW:
		case OP_PACKD:
		case OP_PACKW_UN:
		case OP_PACKD_UN:
		case OP_PMULW_HIGH:
		case OP_PMULW_HIGH_UN: {
			LLVMValueRef args [2];

			args [0] = lhs;
			args [1] = rhs;

			values [ins->dreg] = call_intrins (ctx, simd_ins_to_intrins (ins->opcode), args, "");
			break;
		}
		case OP_PAVGB_UN:
		case OP_PAVGW_UN: {
			LLVMValueRef ones_vec;
			LLVMValueRef ones [32];
			int vector_size = LLVMGetVectorSize (LLVMTypeOf (lhs));
			LLVMTypeRef ext_elem_type = vector_size == 16 ? LLVMInt16Type () : LLVMInt32Type ();

			for (int i = 0; i < 32; ++i)
				ones [i] = LLVMConstInt (ext_elem_type, 1, FALSE);
			ones_vec = LLVMConstVector (ones, vector_size);

			LLVMValueRef val;
			LLVMTypeRef ext_type = LLVMVectorType (ext_elem_type, vector_size);

			/* Have to increase the vector element size to prevent overflows */
			/* res = trunc ((zext (lhs) + zext (rhs) + 1) >> 1) */
			val = LLVMBuildAdd (builder, LLVMBuildZExt (builder, lhs, ext_type, ""), LLVMBuildZExt (builder, rhs, ext_type, ""), "");
			val = LLVMBuildAdd (builder, val, ones_vec, "");
			val = LLVMBuildLShr (builder, val, ones_vec, "");
			values [ins->dreg] = LLVMBuildTrunc (builder, val, LLVMTypeOf (lhs), "");
			break;
		}
		case OP_PCMPEQB:
		case OP_PCMPEQW:
		case OP_PCMPEQD:
		case OP_PCMPEQQ:
		case OP_PCMPGTB: {
			LLVMValueRef pcmp;
			LLVMTypeRef retType;
			LLVMIntPredicate cmpOp;

			if (ins->opcode == OP_PCMPGTB)
				cmpOp = LLVMIntSGT;
			else
				cmpOp = LLVMIntEQ;

			if (LLVMTypeOf (lhs) == LLVMTypeOf (rhs)) {
				pcmp = LLVMBuildICmp (builder, cmpOp, lhs, rhs, "");
				retType = LLVMTypeOf (lhs);
			} else {
				LLVMTypeRef flatType = LLVMVectorType (LLVMInt8Type (), 16);
				LLVMValueRef flatRHS = convert (ctx, rhs, flatType);
				LLVMValueRef flatLHS = convert (ctx, lhs, flatType);

				pcmp = LLVMBuildICmp (builder, cmpOp, flatLHS, flatRHS, "");
				retType = flatType;
			}

			values [ins->dreg] = LLVMBuildSExt (builder, pcmp, retType, "");
			break;
		}
		case OP_EXTRACT_R4:
		case OP_EXTRACT_R8:
		case OP_EXTRACT_I8:
		case OP_EXTRACT_I4:
		case OP_EXTRACT_I2:
		case OP_EXTRACT_U2:
		case OP_EXTRACTX_U2:
		case OP_EXTRACT_I1:
		case OP_EXTRACT_U1: {
			LLVMTypeRef t;
			gboolean zext = FALSE;

			t = simd_op_to_llvm_type (ins->opcode);

			switch (ins->opcode) {
			case OP_EXTRACT_R4:
			case OP_EXTRACT_R8:
			case OP_EXTRACT_I8:
			case OP_EXTRACT_I4:
			case OP_EXTRACT_I2:
			case OP_EXTRACT_I1:
				break;
			case OP_EXTRACT_U2:
			case OP_EXTRACTX_U2:
			case OP_EXTRACT_U1:
				zext = TRUE;
				break;
			default:
				t = LLVMInt32Type ();
				g_assert_not_reached ();
			}

			lhs = LLVMBuildBitCast (builder, lhs, t, "");
			values [ins->dreg] = LLVMBuildExtractElement (builder, lhs, LLVMConstInt (LLVMInt32Type (), ins->inst_c0, FALSE), "");
			if (zext)
				values [ins->dreg] = LLVMBuildZExt (builder, values [ins->dreg], LLVMInt32Type (), "");
			break;
		}

		case OP_INSERT_I1:
			values [ins->dreg] = LLVMBuildInsertElement (builder, values [ins->sreg1], convert (ctx, values [ins->sreg2], LLVMInt8Type ()), LLVMConstInt (LLVMInt32Type (), ins->inst_c0, FALSE), dname);
			break;
		case OP_INSERT_I2:
			values [ins->dreg] = LLVMBuildInsertElement (builder, values [ins->sreg1], convert (ctx, values [ins->sreg2], LLVMInt16Type ()), LLVMConstInt (LLVMInt32Type (), ins->inst_c0, FALSE), dname);
			break;
		case OP_INSERT_I4:
			values [ins->dreg] = LLVMBuildInsertElement (builder, values [ins->sreg1], convert (ctx, values [ins->sreg2], LLVMInt32Type ()), LLVMConstInt (LLVMInt32Type (), ins->inst_c0, FALSE), dname);
			break;
		case OP_INSERT_I8:
			values [ins->dreg] = LLVMBuildInsertElement (builder, values [ins->sreg1], convert (ctx, values [ins->sreg2], LLVMInt64Type ()), LLVMConstInt (LLVMInt32Type (), ins->inst_c0, FALSE), dname);
			break;
		case OP_INSERT_R4:
			values [ins->dreg] = LLVMBuildInsertElement (builder, values [ins->sreg1], convert (ctx, values [ins->sreg2], LLVMFloatType ()), LLVMConstInt (LLVMInt32Type (), ins->inst_c0, FALSE), dname);
			break;
		case OP_INSERT_R8:
			values [ins->dreg] = LLVMBuildInsertElement (builder, values [ins->sreg1], convert (ctx, values [ins->sreg2], LLVMDoubleType ()), LLVMConstInt (LLVMInt32Type (), ins->inst_c0, FALSE), dname);
			break;
		case OP_XINSERT_I2: {
			LLVMBasicBlockRef bbs [64];
			LLVMValueRef switch_ins;
			LLVMValueRef vector = lhs;
			LLVMValueRef value = rhs;
			LLVMValueRef index = values [ins->sreg3];
			LLVMValueRef phi_values [64];
			int nelems = LLVMGetVectorSize (LLVMTypeOf (lhs));
			int i;

			/*
			 * Many SIMD opcodes require an immediate operand, but can be called with a non-immediate.
			 * To handle these cases, generate a switch statement with one case for all possible
			 * values of the immediate.
			 * switch (index) {
			 * case i:
			 *   res = <op> (val, i)
			 *   break;
			 * }
			 */
			g_assert (nelems <= 64);
			for (i = 0; i < nelems; ++i)
				bbs [i] = gen_bb (ctx, "XINSERT_CASE_BB");
			cbb = gen_bb (ctx, "XINSERT_COND_BB");

			switch_ins = LLVMBuildSwitch (builder, LLVMBuildAnd (builder, index, const_int32 (0xf), ""), bbs [0], 0);
			for (i = 0; i < nelems; ++i) {
				LLVMAddCase (switch_ins, LLVMConstInt (LLVMInt32Type (), i, FALSE), bbs [i]);
				LLVMPositionBuilderAtEnd (builder, bbs [i]);
				phi_values [i] = LLVMBuildInsertElement (builder, vector, convert (ctx, value, LLVMGetElementType (LLVMTypeOf (vector))), LLVMConstInt (LLVMInt32Type (), i, FALSE), "");
				LLVMBuildBr (builder, cbb);
			}

			LLVMPositionBuilderAtEnd (builder, cbb);
			values [ins->dreg] = LLVMBuildPhi (builder, LLVMTypeOf (phi_values [0]), "");
			LLVMAddIncoming (values [ins->dreg], phi_values, bbs, nelems);

			ctx->bblocks [bb->block_num].end_bblock = cbb;
			break;
		}
		case OP_CVTDQ2PS: {
			LLVMValueRef i4 = LLVMBuildBitCast (builder, lhs, sse_i4_t, "");
			values [ins->dreg] = LLVMBuildSIToFP (builder, i4, sse_r4_t, dname);
			break;
		}
		case OP_CVTDQ2PD: {
			LLVMValueRef indexes [16];

			indexes [0] = LLVMConstInt (LLVMInt32Type (), 0, FALSE);
			indexes [1] = LLVMConstInt (LLVMInt32Type (), 1, FALSE);
			LLVMValueRef mask = LLVMConstVector (indexes, 2);
			LLVMValueRef shuffle = LLVMBuildShuffleVector (builder, lhs, LLVMConstNull (LLVMTypeOf (lhs)), mask, "");
			values [ins->dreg] = LLVMBuildSIToFP (builder, shuffle, LLVMVectorType (LLVMDoubleType (), 2), dname);
			break;
		}
		case OP_SSE2_CVTSS2SD: {
			LLVMValueRef rhs_elem = LLVMBuildExtractElement (builder, rhs, const_int32 (0), "");
			LLVMValueRef fpext = LLVMBuildFPExt (builder, rhs_elem, LLVMDoubleType (), dname);
			values [ins->dreg] = LLVMBuildInsertElement (builder, lhs, fpext, const_int32 (0), "");
			break;
		}
		case OP_CVTPS2PD: {
			LLVMValueRef indexes [16];

			indexes [0] = LLVMConstInt (LLVMInt32Type (), 0, FALSE);
			indexes [1] = LLVMConstInt (LLVMInt32Type (), 1, FALSE);
			LLVMValueRef mask = LLVMConstVector (indexes, 2);
			LLVMValueRef shuffle = LLVMBuildShuffleVector (builder, lhs, LLVMConstNull (LLVMTypeOf (lhs)), mask, "");
			values [ins->dreg] = LLVMBuildFPExt (builder, shuffle, LLVMVectorType (LLVMDoubleType (), 2), dname);
			break;
		}
		case OP_CVTTPS2DQ:
			values [ins->dreg] = LLVMBuildFPToSI (builder, lhs, LLVMVectorType (LLVMInt32Type (), 4), dname);
			break;

		case OP_CVTPD2DQ:
		case OP_CVTPS2DQ:
		case OP_CVTPD2PS:
		case OP_CVTTPD2DQ:
		case OP_EXTRACT_MASK:
		case OP_SQRTPS:
		case OP_SQRTPD:
		case OP_RSQRTPS:
		case OP_RCPPS: {
			LLVMValueRef v;

			v = convert (ctx, values [ins->sreg1], simd_op_to_llvm_type (ins->opcode));

			values [ins->dreg] = call_intrins (ctx, simd_ins_to_intrins (ins->opcode), &v, dname);
			break;
		}
		case OP_COMPPS:
		case OP_COMPPD: {
			LLVMRealPredicate op;

			switch (ins->inst_c0) {
			case SIMD_COMP_EQ:
				op = LLVMRealOEQ;
				break;
			case SIMD_COMP_LT:
				op = LLVMRealOLT;
				break;
			case SIMD_COMP_LE:
				op = LLVMRealOLE;
				break;
			case SIMD_COMP_UNORD:
				op = LLVMRealUNO;
				break;
			case SIMD_COMP_NEQ:
				op = LLVMRealUNE;
				break;
			case SIMD_COMP_NLT:
				op = LLVMRealUGE;
				break;
			case SIMD_COMP_NLE:
				op = LLVMRealUGT;
				break;
			case SIMD_COMP_ORD:
				op = LLVMRealORD;
				break;
			default:
				g_assert_not_reached ();
			}

			LLVMValueRef cmp = LLVMBuildFCmp (builder, op, lhs, rhs, "");
			if (ins->opcode == OP_COMPPD)
				values [ins->dreg] = LLVMBuildBitCast (builder, LLVMBuildSExt (builder, cmp, LLVMVectorType (LLVMInt64Type (), 2), ""), LLVMTypeOf (lhs), "");
			else
				values [ins->dreg] = LLVMBuildBitCast (builder, LLVMBuildSExt (builder, cmp, LLVMVectorType (LLVMInt32Type (), 4), ""), LLVMTypeOf (lhs), "");
			break;
		}
		case OP_ICONV_TO_X:
			/* This is only used for implementing shifts by non-immediate */
			values [ins->dreg] = lhs;
			break;

		case OP_PSHRW:
		case OP_PSHRD:
		case OP_PSHRQ:
		case OP_PSARW:
		case OP_PSARD:
		case OP_PSHLW:
		case OP_PSHLD:
		case OP_PSHLQ: {
			LLVMValueRef args [3];

			args [0] = lhs;
			args [1] = LLVMConstInt (LLVMInt32Type (), ins->inst_imm, FALSE);

			values [ins->dreg] = call_intrins (ctx, simd_ins_to_intrins (ins->opcode), args, dname);
			break;
		}

		case OP_PSHRW_REG:
		case OP_PSHRD_REG:
		case OP_PSHRQ_REG:
		case OP_PSARW_REG:
		case OP_PSARD_REG:
		case OP_PSHLW_REG:
		case OP_PSHLD_REG:
		case OP_PSHLQ_REG: {
			LLVMValueRef args [3];

			args [0] = lhs;
			args [1] = values [ins->sreg2];

			values [ins->dreg] = call_intrins (ctx, simd_ins_to_intrins (ins->opcode), args, dname);
			break;
		}

		case OP_SHUFPS:
		case OP_SHUFPD:
		case OP_PSHUFLED:
		case OP_PSHUFLEW_LOW:
		case OP_PSHUFLEW_HIGH: {
			int mask [16];
			LLVMValueRef v1 = NULL, v2 = NULL, mask_values [16];
			int i, mask_size = 0;
			int imask = ins->inst_c0;
	
			/* Convert the x86 shuffle mask to LLVM's */
			switch (ins->opcode) {
			case OP_SHUFPS:
				mask_size = 4;
				mask [0] = ((imask >> 0) & 3);
				mask [1] = ((imask >> 2) & 3);
				mask [2] = ((imask >> 4) & 3) + 4;
				mask [3] = ((imask >> 6) & 3) + 4;
				v1 = values [ins->sreg1];
				v2 = values [ins->sreg2];
				break;
			case OP_SHUFPD:
				mask_size = 2;
				mask [0] = ((imask >> 0) & 1);
				mask [1] = ((imask >> 1) & 1) + 2;
				v1 = values [ins->sreg1];
				v2 = values [ins->sreg2];
				break;
			case OP_PSHUFLEW_LOW:
				mask_size = 8;
				mask [0] = ((imask >> 0) & 3);
				mask [1] = ((imask >> 2) & 3);
				mask [2] = ((imask >> 4) & 3);
				mask [3] = ((imask >> 6) & 3);
				mask [4] = 4 + 0;
				mask [5] = 4 + 1;
				mask [6] = 4 + 2;
				mask [7] = 4 + 3;
				v1 = values [ins->sreg1];
				v2 = LLVMGetUndef (LLVMTypeOf (v1));
				break;
			case OP_PSHUFLEW_HIGH:
				mask_size = 8;
				mask [0] = 0;
				mask [1] = 1;
				mask [2] = 2;
				mask [3] = 3;
				mask [4] = 4 + ((imask >> 0) & 3);
				mask [5] = 4 + ((imask >> 2) & 3);
				mask [6] = 4 + ((imask >> 4) & 3);
				mask [7] = 4 + ((imask >> 6) & 3);
				v1 = values [ins->sreg1];
				v2 = LLVMGetUndef (LLVMTypeOf (v1));
				break;
			case OP_PSHUFLED:
				mask_size = 4;
				mask [0] = ((imask >> 0) & 3);
				mask [1] = ((imask >> 2) & 3);
				mask [2] = ((imask >> 4) & 3);
				mask [3] = ((imask >> 6) & 3);
				v1 = values [ins->sreg1];
				v2 = LLVMGetUndef (LLVMTypeOf (v1));
				break;
			default:
				g_assert_not_reached ();
			}
			for (i = 0; i < mask_size; ++i)
				mask_values [i] = LLVMConstInt (LLVMInt32Type (), mask [i], FALSE);

			values [ins->dreg] =
				LLVMBuildShuffleVector (builder, v1, v2,
										LLVMConstVector (mask_values, mask_size), dname);
			break;
		}

		case OP_UNPACK_LOWB:
		case OP_UNPACK_LOWW:
		case OP_UNPACK_LOWD:
		case OP_UNPACK_LOWQ:
		case OP_UNPACK_LOWPS:
		case OP_UNPACK_LOWPD:
		case OP_UNPACK_HIGHB:
		case OP_UNPACK_HIGHW:
		case OP_UNPACK_HIGHD:
		case OP_UNPACK_HIGHQ:
		case OP_UNPACK_HIGHPS:
		case OP_UNPACK_HIGHPD: {
			int mask [16];
			LLVMValueRef mask_values [16];
			int i, mask_size = 0;
			gboolean low = FALSE;

			switch (ins->opcode) {
			case OP_UNPACK_LOWB:
				mask_size = 16;
				low = TRUE;
				break;
			case OP_UNPACK_LOWW:
				mask_size = 8;
				low = TRUE;
				break;
			case OP_UNPACK_LOWD:
			case OP_UNPACK_LOWPS:
				mask_size = 4;
				low = TRUE;
				break;
			case OP_UNPACK_LOWQ:
			case OP_UNPACK_LOWPD:
				mask_size = 2;
				low = TRUE;
				break;
			case OP_UNPACK_HIGHB:
				mask_size = 16;
				break;
			case OP_UNPACK_HIGHW:
				mask_size = 8;
				break;
			case OP_UNPACK_HIGHD:
			case OP_UNPACK_HIGHPS:
				mask_size = 4;
				break;
			case OP_UNPACK_HIGHQ:
			case OP_UNPACK_HIGHPD:
				mask_size = 2;
				break;
			default:
				g_assert_not_reached ();
			}

			if (low) {
				for (i = 0; i < (mask_size / 2); ++i) {
					mask [(i * 2)] = i;
					mask [(i * 2) + 1] = mask_size + i;
				}
			} else {
				for (i = 0; i < (mask_size / 2); ++i) {
					mask [(i * 2)] = (mask_size / 2) + i;
					mask [(i * 2) + 1] = mask_size + (mask_size / 2) + i;
				}
			}

			for (i = 0; i < mask_size; ++i)
				mask_values [i] = LLVMConstInt (LLVMInt32Type (), mask [i], FALSE);
			
			values [ins->dreg] =
				LLVMBuildShuffleVector (builder, values [ins->sreg1], values [ins->sreg2],
										LLVMConstVector (mask_values, mask_size), dname);
			break;
		}

		case OP_DUPPD: {
			LLVMTypeRef t = simd_op_to_llvm_type (ins->opcode);
			LLVMValueRef v, val;

			v = LLVMBuildExtractElement (builder, lhs, LLVMConstInt (LLVMInt32Type (), 0, FALSE), "");
			val = LLVMConstNull (t);
			val = LLVMBuildInsertElement (builder, val, v, LLVMConstInt (LLVMInt32Type (), 0, FALSE), "");
			val = LLVMBuildInsertElement (builder, val, v, LLVMConstInt (LLVMInt32Type (), 1, FALSE), dname);

			values [ins->dreg] = val;
			break;
		}
		case OP_DUPPS_LOW:
		case OP_DUPPS_HIGH: {
			LLVMTypeRef t = simd_op_to_llvm_type (ins->opcode);
			LLVMValueRef v1, v2, val;
			

			if (ins->opcode == OP_DUPPS_LOW) {
				v1 = LLVMBuildExtractElement (builder, lhs, LLVMConstInt (LLVMInt32Type (), 0, FALSE), "");
				v2 = LLVMBuildExtractElement (builder, lhs, LLVMConstInt (LLVMInt32Type (), 2, FALSE), "");
			} else {
				v1 = LLVMBuildExtractElement (builder, lhs, LLVMConstInt (LLVMInt32Type (), 1, FALSE), "");
				v2 = LLVMBuildExtractElement (builder, lhs, LLVMConstInt (LLVMInt32Type (), 3, FALSE), "");
			}
			val = LLVMConstNull (t);
			val = LLVMBuildInsertElement (builder, val, v1, LLVMConstInt (LLVMInt32Type (), 0, FALSE), "");
			val = LLVMBuildInsertElement (builder, val, v1, LLVMConstInt (LLVMInt32Type (), 1, FALSE), "");
			val = LLVMBuildInsertElement (builder, val, v2, LLVMConstInt (LLVMInt32Type (), 2, FALSE), "");
			val = LLVMBuildInsertElement (builder, val, v2, LLVMConstInt (LLVMInt32Type (), 3, FALSE), "");
			
			values [ins->dreg] = val;
			break;
		}

		case OP_DPPS: {
			LLVMValueRef args [3];

			args [0] = lhs;
			args [1] = rhs;
			/* 0xf1 == multiply all 4 elements, add them together, and store the result to the lowest element */
			args [2] = LLVMConstInt (LLVMInt8Type (), 0xf1, FALSE);

			values [ins->dreg] = call_intrins (ctx, simd_ins_to_intrins (ins->opcode), args, dname);
			break;
		}

		case OP_FCONV_TO_R8_X: {
			values [ins->dreg] = LLVMBuildInsertElement (builder, LLVMConstNull (sse_r8_t), lhs, LLVMConstInt (LLVMInt32Type (), 0, FALSE), "");
			break;
		}

		case OP_FCONV_TO_R4_X: {
			values [ins->dreg] = LLVMBuildInsertElement (builder, LLVMConstNull (sse_r4_t), lhs, LLVMConstInt (LLVMInt32Type (), 0, FALSE), "");
			break;
		}

#if defined(TARGET_X86) || defined(TARGET_AMD64)
		case OP_SSE_MOVMSK: {
			LLVMValueRef args [1];
			if (ins->inst_c1 == MONO_TYPE_R4) {
				args [0] = lhs;
				values [ins->dreg] = call_intrins (ctx, INTRINS_SSE_MOVMSK_PS, args, dname);
			} else if (ins->inst_c1 == MONO_TYPE_R8) {
				args [0] = lhs;
				values [ins->dreg] = call_intrins (ctx, INTRINS_SSE_MOVMSK_PD, args, dname);
			} else {
				args [0] = convert (ctx, lhs, sse_i1_t);
				values [ins->dreg] = call_intrins (ctx, INTRINS_SSE_PMOVMSKB, args, dname);
			}
			break;
		}

		case OP_SSE_MOVS:
		case OP_SSE_MOVS2: {
			if (ins->inst_c1 == MONO_TYPE_R4)
				values [ins->dreg] = LLVMBuildShuffleVector (builder, rhs, lhs, create_const_vector_4_i32 (0, 5, 6, 7), "");
			else if (ins->inst_c1 == MONO_TYPE_R8)
				values [ins->dreg] = LLVMBuildShuffleVector (builder, rhs, lhs, create_const_vector_2_i32 (0, 3), "");
			else if (ins->inst_c1 == MONO_TYPE_I8 || ins->inst_c1 == MONO_TYPE_U8)
				values [ins->dreg] = LLVMBuildInsertElement (builder, lhs, 
					LLVMConstInt (LLVMInt64Type (), 0, FALSE), 
					LLVMConstInt (LLVMInt32Type (), 1, FALSE), "");
			else
				g_assert_not_reached (); // will be needed for other types later
			break;
		}

		case OP_SSE_MOVEHL: {
			if (ins->inst_c1 == MONO_TYPE_R4)
				values [ins->dreg] = LLVMBuildShuffleVector (builder, lhs, rhs, create_const_vector_4_i32 (6, 7, 2, 3), "");
			else
				g_assert_not_reached ();
			break;
		}

		case OP_SSE_MOVELH: {
			if (ins->inst_c1 == MONO_TYPE_R4)
				values [ins->dreg] = LLVMBuildShuffleVector (builder, lhs, rhs, create_const_vector_4_i32 (0, 1, 4, 5), "");
			else
				g_assert_not_reached ();
			break;
		}

		case OP_SSE_UNPACKLO: {
			if (ins->inst_c1 == MONO_TYPE_R8 || ins->inst_c1 == MONO_TYPE_I8 || ins->inst_c1 == MONO_TYPE_U8) {
				values [ins->dreg] = LLVMBuildShuffleVector (builder, lhs, rhs, create_const_vector_2_i32 (0, 2), "");
			} else if (ins->inst_c1 == MONO_TYPE_R4 || ins->inst_c1 == MONO_TYPE_I4  || ins->inst_c1 == MONO_TYPE_U4) {
				values [ins->dreg] = LLVMBuildShuffleVector (builder, lhs, rhs, create_const_vector_4_i32 (0, 4, 1, 5), "");
			} else if (ins->inst_c1 == MONO_TYPE_I2 || ins->inst_c1 == MONO_TYPE_U2) {
				const int mask_values [] = { 0, 8, 1, 9, 2, 10, 3, 11 };
				LLVMValueRef shuffled = LLVMBuildShuffleVector (builder, 
					convert (ctx, lhs, sse_i2_t),
					convert (ctx, rhs, sse_i2_t),
					create_const_vector_i32 (mask_values, 8), "");
				values [ins->dreg] = convert (ctx, shuffled, type_to_sse_type (ins->inst_c1));
			} else if (ins->inst_c1 == MONO_TYPE_I1 || ins->inst_c1 == MONO_TYPE_U1) {
				const int mask_values [] = { 0, 16, 1, 17, 2, 18, 3, 19, 4, 20, 5, 21, 6, 22, 7, 23 };
				LLVMValueRef shuffled = LLVMBuildShuffleVector (builder, 
					convert (ctx, lhs, sse_i1_t),
					convert (ctx, rhs, sse_i1_t),
					create_const_vector_i32 (mask_values, 16), "");
				values [ins->dreg] = convert (ctx, shuffled, type_to_sse_type (ins->inst_c1));
			} else {
				g_assert_not_reached ();
			}
			break;
		}

		case OP_SSE_UNPACKHI: {
			if (ins->inst_c1 == MONO_TYPE_R8 || ins->inst_c1 == MONO_TYPE_I8 || ins->inst_c1 == MONO_TYPE_U8) {
				values [ins->dreg] = LLVMBuildShuffleVector (builder, lhs, rhs, create_const_vector_2_i32 (1, 3), "");
			} else if (ins->inst_c1 == MONO_TYPE_R4 || ins->inst_c1 == MONO_TYPE_I4  || ins->inst_c1 == MONO_TYPE_U4) {
				values [ins->dreg] = LLVMBuildShuffleVector (builder, lhs, rhs, create_const_vector_4_i32 (2, 6, 3, 7), "");
			} else if (ins->inst_c1 == MONO_TYPE_I2 || ins->inst_c1 == MONO_TYPE_U2) {
				const int mask_values [] = { 4, 12, 5, 13, 6, 14, 7, 15 };
				LLVMValueRef shuffled = LLVMBuildShuffleVector (builder, 
					convert (ctx, lhs, sse_i2_t),
					convert (ctx, rhs, sse_i2_t),
					create_const_vector_i32 (mask_values, 8), "");
				values [ins->dreg] = convert (ctx, shuffled, type_to_sse_type (ins->inst_c1));
			} else if (ins->inst_c1 == MONO_TYPE_I1 || ins->inst_c1 == MONO_TYPE_U1) {
				const int mask_values [] = { 8, 24, 9, 25, 10, 26, 11, 27, 12, 28, 13, 29, 14, 30, 15, 31 };
				LLVMValueRef shuffled = LLVMBuildShuffleVector (builder, 
					convert (ctx, lhs, sse_i1_t),
					convert (ctx, rhs, sse_i1_t),
					create_const_vector_i32 (mask_values, 16), "");
				values [ins->dreg] = convert (ctx, shuffled, type_to_sse_type (ins->inst_c1));
			} else {
				g_assert_not_reached ();
			}
			break;
		}

		case OP_SSE_LOADU: {
			LLVMValueRef dst_ptr = convert (ctx, lhs, LLVMPointerType (primitive_type_to_llvm_type (inst_c1_type (ins)), 0));
			LLVMValueRef dst_vec = LLVMBuildBitCast (builder, dst_ptr, LLVMPointerType (type_to_sse_type (ins->inst_c1), 0), "");
			values [ins->dreg] = mono_llvm_build_aligned_load (builder, type_to_sse_type (ins->inst_c1), dst_vec, "", FALSE, ins->inst_c0); // inst_c0 is alignment
			break;
		}
		case OP_SSE_MOVSS: {
			LLVMValueRef addr = convert (ctx, lhs, LLVMPointerType (LLVMFloatType (), 0));
			LLVMValueRef val = mono_llvm_build_load (builder, LLVMFloatType (), addr, "", FALSE);
			values [ins->dreg] = LLVMBuildInsertElement (builder, LLVMConstNull (type_to_sse_type (ins->inst_c1)), val, LLVMConstInt (LLVMInt32Type (), 0, FALSE), "");
			break;
		}
		case OP_SSE_MOVSS_STORE: {
			LLVMValueRef addr = convert (ctx, lhs, LLVMPointerType (LLVMFloatType (), 0));
			LLVMValueRef val = LLVMBuildExtractElement (builder, rhs, LLVMConstInt (LLVMInt32Type (), 0, FALSE), "");
			mono_llvm_build_store (builder, val, addr, FALSE, LLVM_BARRIER_NONE);
			break;
		}
		case OP_SSE2_MOVD:
		case OP_SSE2_MOVQ:
		case OP_SSE2_MOVUPD: {
			LLVMTypeRef rty = NULL;
			switch (ins->opcode) {
			case OP_SSE2_MOVD: rty = sse_i4_t; break;
			case OP_SSE2_MOVQ: rty = sse_i8_t; break;
			case OP_SSE2_MOVUPD: rty = sse_r8_t; break;
			}
			LLVMTypeRef srcty = LLVMGetElementType (rty);
			LLVMValueRef zero = LLVMConstNull (rty);
			LLVMValueRef addr = convert (ctx, lhs, LLVMPointerType (srcty, 0));
			LLVMValueRef val = mono_llvm_build_aligned_load (builder, srcty, addr, "", FALSE, 1);
			values [ins->dreg] = LLVMBuildInsertElement (builder, zero, val, const_int32 (0), dname);
			break;
		}

		case OP_SSE_MOVLPS_LOAD:
		case OP_SSE_MOVHPS_LOAD: {
			LLVMTypeRef t = LLVMFloatType ();
			int size = 4;
			gboolean high = ins->opcode == OP_SSE_MOVHPS_LOAD;
			/* Load two floats from rhs and store them in the low/high part of lhs */
			LLVMValueRef addr = rhs;
			LLVMValueRef addr1 = convert (ctx, addr, LLVMPointerType (t, 0));
			LLVMValueRef addr2 = convert (ctx, LLVMBuildAdd (builder, convert (ctx, addr, IntPtrType ()), convert (ctx, LLVMConstInt (LLVMInt32Type (), size, FALSE), IntPtrType ()), ""), LLVMPointerType (t, 0));
			LLVMValueRef val1 = mono_llvm_build_load (builder, t, addr1, "", FALSE);
			LLVMValueRef val2 = mono_llvm_build_load (builder, t, addr2, "", FALSE);
			int index1, index2;

			index1 = high ? 2: 0;
			index2 = high ? 3 : 1;
			values [ins->dreg] = LLVMBuildInsertElement (builder, LLVMBuildInsertElement (builder, lhs, val1, LLVMConstInt (LLVMInt32Type (), index1, FALSE), ""), val2, LLVMConstInt (LLVMInt32Type (), index2, FALSE), "");
			break;
		}

		case OP_SSE2_MOVLPD_LOAD:
		case OP_SSE2_MOVHPD_LOAD: {
			LLVMTypeRef t = LLVMDoubleType ();
			LLVMValueRef addr = convert (ctx, rhs, LLVMPointerType (t, 0));
			LLVMValueRef val = mono_llvm_build_load (builder, t, addr, "", FALSE);
			int index = ins->opcode == OP_SSE2_MOVHPD_LOAD ? 1 : 0;
			values [ins->dreg] = LLVMBuildInsertElement (builder, lhs, val, const_int32 (index), "");
			break;
		}

		case OP_SSE_MOVLPS_STORE:
		case OP_SSE_MOVHPS_STORE: {
			/* Store two floats from the low/hight part of rhs into lhs */
			LLVMValueRef addr = lhs;
			LLVMValueRef addr1 = convert (ctx, addr, LLVMPointerType (LLVMFloatType (), 0));
			LLVMValueRef addr2 = convert (ctx, LLVMBuildAdd (builder, convert (ctx, addr, IntPtrType ()), convert (ctx, LLVMConstInt (LLVMInt32Type (), 4, FALSE), IntPtrType ()), ""), LLVMPointerType (LLVMFloatType (), 0));
			int index1 = ins->opcode == OP_SSE_MOVLPS_STORE ? 0 : 2;
			int index2 = ins->opcode == OP_SSE_MOVLPS_STORE ? 1 : 3;
			LLVMValueRef val1 = LLVMBuildExtractElement (builder, rhs, LLVMConstInt (LLVMInt32Type (), index1, FALSE), "");
			LLVMValueRef val2 = LLVMBuildExtractElement (builder, rhs, LLVMConstInt (LLVMInt32Type (), index2, FALSE), "");
			mono_llvm_build_store (builder, val1, addr1, FALSE, LLVM_BARRIER_NONE);
			mono_llvm_build_store (builder, val2, addr2, FALSE, LLVM_BARRIER_NONE);
			break;
		}

		case OP_SSE2_MOVLPD_STORE:
		case OP_SSE2_MOVHPD_STORE: {
			LLVMTypeRef t = LLVMDoubleType ();
			LLVMValueRef addr = convert (ctx, lhs, LLVMPointerType (t, 0));
			int index = ins->opcode == OP_SSE2_MOVHPD_STORE ? 1 : 0;
			LLVMValueRef val = LLVMBuildExtractElement (builder, rhs, const_int32 (index), "");
			mono_llvm_build_store (builder, val, addr, FALSE, LLVM_BARRIER_NONE);
			break;
		}

		case OP_SSE_STORE: {
			LLVMValueRef dst_vec = convert (ctx, lhs, LLVMPointerType (LLVMTypeOf (rhs), 0));
			mono_llvm_build_aligned_store (builder, rhs, dst_vec, FALSE, ins->inst_c0);
			break;
		}

		case OP_SSE_STORES: {
			LLVMValueRef first_elem = LLVMBuildExtractElement (builder, rhs, LLVMConstInt (LLVMInt32Type (), 0, FALSE), "");
			LLVMValueRef dst = convert (ctx, lhs, LLVMPointerType (LLVMTypeOf (first_elem), 0));
			mono_llvm_build_aligned_store (builder, first_elem, dst, FALSE, 1);
			break;
		}
		case OP_SSE_MOVNTPS: {
			LLVMValueRef addr = convert (ctx, lhs, LLVMPointerType (LLVMTypeOf (rhs), 0));
			LLVMValueRef store = mono_llvm_build_aligned_store (builder, rhs, addr, FALSE, ins->inst_c0);
			set_nontemporal_flag (store);
			break;
		}
		case OP_SSE_PREFETCHT0: {
			LLVMValueRef addr = convert (ctx, lhs, LLVMPointerType (LLVMInt8Type (), 0));
			LLVMValueRef args [] = { addr, const_int32 (0), const_int32 (3), const_int32 (1) };
			call_intrins (ctx, INTRINS_PREFETCH, args, "");
			break;
		}
		case OP_SSE_PREFETCHT1: {
			LLVMValueRef addr = convert (ctx, lhs, LLVMPointerType (LLVMInt8Type (), 0));
			LLVMValueRef args [] = { addr, const_int32 (0), const_int32 (2), const_int32 (1) };
			call_intrins (ctx, INTRINS_PREFETCH, args, "");
			break;
		}
		case OP_SSE_PREFETCHT2: {
			LLVMValueRef addr = convert (ctx, lhs, LLVMPointerType (LLVMInt8Type (), 0));
			LLVMValueRef args [] = { addr, const_int32 (0), const_int32 (1), const_int32 (1) };
			call_intrins (ctx, INTRINS_PREFETCH, args, "");
			break;
		}
		case OP_SSE_PREFETCHNTA: {
			LLVMValueRef addr = convert (ctx, lhs, LLVMPointerType (LLVMInt8Type (), 0));
			LLVMValueRef args [] = { addr, const_int32 (0), const_int32 (0), const_int32 (1) };
			call_intrins (ctx, INTRINS_PREFETCH, args, "");
			break;
		}

		case OP_SSE_SHUFFLE: {
			LLVMValueRef shuffle_vec = create_const_vector_4_i32 (
				((ins->inst_c0 >> 0) & 0x3) + 0, // take two elements from lhs
				((ins->inst_c0 >> 2) & 0x3) + 0, 
				((ins->inst_c0 >> 4) & 0x3) + 4, // and two from rhs
				((ins->inst_c0 >> 6) & 0x3) + 4);
			values [ins->dreg] = LLVMBuildShuffleVector (builder, lhs, rhs, shuffle_vec, "");
			break;
		}

		case OP_SSE2_SHUFFLE: {
			LLVMValueRef right_vec;
			LLVMValueRef shuffle_vec;
			if (ins->inst_c1 == MONO_TYPE_R8) {
				right_vec = rhs;
				shuffle_vec = create_const_vector_2_i32 (
					((ins->inst_c0 >> 0) & 0x1) + 0,
					((ins->inst_c0 >> 1) & 0x1) + 2);
			} else {
				right_vec = LLVMGetUndef (LLVMVectorType (LLVMInt32Type (), 4));
				shuffle_vec = create_const_vector_4_i32 (
					(ins->inst_c0 >> 0) & 0x3,
					(ins->inst_c0 >> 2) & 0x3, 
					(ins->inst_c0 >> 4) & 0x3,
					(ins->inst_c0 >> 6) & 0x3);
			}
			values [ins->dreg] = LLVMBuildShuffleVector (builder, lhs, right_vec, shuffle_vec, "");
			break;
		}

		case OP_SSE_OR: {
			LLVMValueRef vec_lhs_i64 = convert (ctx, lhs, sse_i8_t);
			LLVMValueRef vec_rhs_i64 = convert (ctx, rhs, sse_i8_t);
			LLVMValueRef vec_and = LLVMBuildOr (builder, vec_lhs_i64, vec_rhs_i64, "");
			values [ins->dreg] = LLVMBuildBitCast (builder, vec_and, type_to_sse_type (ins->inst_c1), "");
			break;
		}

		case OP_SSE_XOR: {
			LLVMValueRef vec_lhs_i64 = convert (ctx, lhs, sse_i8_t);
			LLVMValueRef vec_rhs_i64 = convert (ctx, rhs, sse_i8_t);
			LLVMValueRef vec_and = LLVMBuildXor (builder, vec_lhs_i64, vec_rhs_i64, "");
			values [ins->dreg] = LLVMBuildBitCast (builder, vec_and, type_to_sse_type (ins->inst_c1), "");
			break;
		}

		case OP_SSE_AND: {
			LLVMValueRef vec_lhs_i64 = convert (ctx, lhs, sse_i8_t);
			LLVMValueRef vec_rhs_i64 = convert (ctx, rhs, sse_i8_t);
			LLVMValueRef vec_and = LLVMBuildAnd (builder, vec_lhs_i64, vec_rhs_i64, "");
			values [ins->dreg] = LLVMBuildBitCast (builder, vec_and, type_to_sse_type (ins->inst_c1), "");
			break;
		}

		case OP_SSE_ANDN: {
			LLVMValueRef minus_one [2];
			minus_one [0] = LLVMConstInt (LLVMInt64Type (), -1, FALSE);
			minus_one [1] = LLVMConstInt (LLVMInt64Type (), -1, FALSE);
			LLVMValueRef vec_lhs_i64 = convert (ctx, lhs, sse_i8_t);
			LLVMValueRef vec_xor = LLVMBuildXor (builder, vec_lhs_i64, LLVMConstVector (minus_one, 2), "");
			LLVMValueRef vec_rhs_i64 = convert (ctx, rhs, sse_i8_t);
			LLVMValueRef vec_and = LLVMBuildAnd (builder, vec_rhs_i64, vec_xor, "");
			values [ins->dreg] = LLVMBuildBitCast (builder, vec_and, type_to_sse_type (ins->inst_c1), "");
			break;
		}

		case OP_SSE_ADDSS:
		case OP_SSE_SUBSS:
		case OP_SSE_DIVSS:
		case OP_SSE_MULSS:
		case OP_SSE2_ADDSD:
		case OP_SSE2_SUBSD:
		case OP_SSE2_DIVSD:
		case OP_SSE2_MULSD: {
			LLVMValueRef v1 = LLVMBuildExtractElement (builder, lhs, LLVMConstInt (LLVMInt32Type (), 0, FALSE), "");
			LLVMValueRef v2 = LLVMBuildExtractElement (builder, rhs, LLVMConstInt (LLVMInt32Type (), 0, FALSE), "");

			LLVMValueRef v = NULL;
			switch (ins->opcode) {
			case OP_SSE_ADDSS:
			case OP_SSE2_ADDSD:
				v = LLVMBuildFAdd (builder, v1, v2, "");
				break;
			case OP_SSE_SUBSS:
			case OP_SSE2_SUBSD:
				v = LLVMBuildFSub (builder, v1, v2, "");
				break;
			case OP_SSE_DIVSS:
			case OP_SSE2_DIVSD:
				v = LLVMBuildFDiv (builder, v1, v2, "");
				break;
			case OP_SSE_MULSS:
			case OP_SSE2_MULSD:
				v = LLVMBuildFMul (builder, v1, v2, "");
				break;
			default:
				g_assert_not_reached ();
			}
			values [ins->dreg] = LLVMBuildInsertElement (builder, lhs, v, LLVMConstInt (LLVMInt32Type (), 0, FALSE), "");
			break;
		}

		case OP_SSE_CMPSS:
		case OP_SSE2_CMPSD: {
			int imm = -1;
			switch (ins->inst_c0) {
			case CMP_EQ: imm = 0; break;
			case CMP_GT: imm = 6; break;
			case CMP_GE: imm = 5; break;
			case CMP_LT: imm = 1; break;
			case CMP_LE: imm = 2; break;
			case CMP_NE: imm = 4; break;
			case CMP_ORD: imm = 7; break;
			case CMP_UNORD: imm = 3; break;
			default: g_assert_not_reached (); break;
			}
			LLVMValueRef cmp = LLVMConstInt (LLVMInt8Type (), imm, FALSE);
			LLVMValueRef args [] = { lhs, rhs, cmp };
			switch (ins->opcode) {
			case OP_SSE_CMPSS:
				values [ins->dreg] = call_intrins (ctx, INTRINS_SSE_CMPSS, args, "");
				break;
			case OP_SSE2_CMPSD:
				values [ins->dreg] = call_intrins (ctx, INTRINS_SSE_CMPSD, args, "");
				break;
			default:
				g_assert_not_reached ();
				break;
			}
			break;
		}
		case OP_SSE_COMISS: {
			LLVMValueRef args [] = { lhs, rhs };
			IntrinsicId id = (IntrinsicId)0;
			switch (ins->inst_c0) {
			case CMP_EQ: id = INTRINS_SSE_COMIEQ_SS; break;
			case CMP_GT: id = INTRINS_SSE_COMIGT_SS; break;
			case CMP_GE: id = INTRINS_SSE_COMIGE_SS; break;
			case CMP_LT: id = INTRINS_SSE_COMILT_SS; break;
			case CMP_LE: id = INTRINS_SSE_COMILE_SS; break;
			case CMP_NE: id = INTRINS_SSE_COMINEQ_SS; break;
			default: g_assert_not_reached (); break;
			}
			values [ins->dreg] = call_intrins (ctx, id, args, "");
			break;
		}
		case OP_SSE_UCOMISS: {
			LLVMValueRef args [] = { lhs, rhs };
			IntrinsicId id = (IntrinsicId)0;
			switch (ins->inst_c0) {
			case CMP_EQ: id = INTRINS_SSE_UCOMIEQ_SS; break;
			case CMP_GT: id = INTRINS_SSE_UCOMIGT_SS; break;
			case CMP_GE: id = INTRINS_SSE_UCOMIGE_SS; break;
			case CMP_LT: id = INTRINS_SSE_UCOMILT_SS; break;
			case CMP_LE: id = INTRINS_SSE_UCOMILE_SS; break;
			case CMP_NE: id = INTRINS_SSE_UCOMINEQ_SS; break;
			default: g_assert_not_reached (); break;
			}
			values [ins->dreg] = call_intrins (ctx, id, args, "");
			break;
		}
		case OP_SSE2_COMISD: {
			LLVMValueRef args [] = { lhs, rhs };
			IntrinsicId id = (IntrinsicId)0;
			switch (ins->inst_c0) {
			case CMP_EQ: id = INTRINS_SSE_COMIEQ_SD; break;
			case CMP_GT: id = INTRINS_SSE_COMIGT_SD; break;
			case CMP_GE: id = INTRINS_SSE_COMIGE_SD; break;
			case CMP_LT: id = INTRINS_SSE_COMILT_SD; break;
			case CMP_LE: id = INTRINS_SSE_COMILE_SD; break;
			case CMP_NE: id = INTRINS_SSE_COMINEQ_SD; break;
			default: g_assert_not_reached (); break;
			}
			values [ins->dreg] = call_intrins (ctx, id, args, "");
			break;
		}
		case OP_SSE2_UCOMISD: {
			LLVMValueRef args [] = { lhs, rhs };
			IntrinsicId id = (IntrinsicId)0;
			switch (ins->inst_c0) {
			case CMP_EQ: id = INTRINS_SSE_UCOMIEQ_SD; break;
			case CMP_GT: id = INTRINS_SSE_UCOMIGT_SD; break;
			case CMP_GE: id = INTRINS_SSE_UCOMIGE_SD; break;
			case CMP_LT: id = INTRINS_SSE_UCOMILT_SD; break;
			case CMP_LE: id = INTRINS_SSE_UCOMILE_SD; break;
			case CMP_NE: id = INTRINS_SSE_UCOMINEQ_SD; break;
			default: g_assert_not_reached (); break;
			}
			values [ins->dreg] = call_intrins (ctx, id, args, "");
			break;
		}
		case OP_SSE_CVTSI2SS:
		case OP_SSE_CVTSI2SS64:
		case OP_SSE2_CVTSI2SD:
		case OP_SSE2_CVTSI2SD64: {
			LLVMTypeRef ty = LLVMFloatType ();
			switch (ins->opcode) {
			case OP_SSE2_CVTSI2SD:
			case OP_SSE2_CVTSI2SD64:
				ty = LLVMDoubleType ();
				break;
			}
			LLVMValueRef fp = LLVMBuildSIToFP (builder, rhs, ty, "");
			values [ins->dreg] = LLVMBuildInsertElement (builder, lhs, fp, const_int32 (0), dname);
			break;
		}
		case OP_SSE2_PMULUDQ: {
#if LLVM_API_VERSION < 700
			LLVMValueRef args [] = { lhs, rhs };
			values [ins->dreg] = call_intrins (ctx, INTRINS_SSE_PMULUDQ, args, dname);
#else
			LLVMValueRef i32_max = LLVMConstInt (LLVMInt64Type (), UINT32_MAX, FALSE);
			LLVMValueRef maskvals [] = { i32_max, i32_max };
			LLVMValueRef mask = LLVMConstVector (maskvals, 2);
			LLVMValueRef l = LLVMBuildAnd (builder, convert (ctx, lhs, sse_i8_t), mask, "");
			LLVMValueRef r = LLVMBuildAnd (builder, convert (ctx, rhs, sse_i8_t), mask, "");
			values [ins->dreg] = LLVMBuildNUWMul (builder, l, r, dname);
#endif
			break;
		}
		case OP_SSE_SQRTSS:
		case OP_SSE2_SQRTSD: {
#if LLVM_API_VERSION < 700
			LLVMValueRef result = call_intrins (ctx, simd_ins_to_intrins (ins->opcode), &rhs, dname);
			const int maskf32[] = { 0, 5, 6, 7 };
			const int maskf64[] = { 0, 1 };
			const int *mask = NULL;
			int mask_len = 0;
			switch (ins->opcode) {
			case OP_SSE_SQRTSS: mask = maskf32; mask_len = 4; break;
			case OP_SSE2_SQRTSD: mask = maskf64; mask_len = 2; break;
			default: g_assert_not_reached (); break;
			}
			LLVMValueRef shufmask = create_const_vector_i32 (mask, mask_len);
			values [ins->dreg] = LLVMBuildShuffleVector (builder, result, lhs, shufmask, "");
#else
			LLVMValueRef upper = values [ins->sreg1];
			LLVMValueRef lower = values [ins->sreg2];
			LLVMValueRef scalar = LLVMBuildExtractElement (builder, lower, const_int32 (0), "");
			LLVMValueRef result = call_intrins (ctx, simd_ins_to_intrins (ins->opcode), &scalar, dname);
			values [ins->dreg] = LLVMBuildInsertElement (builder, upper, result, const_int32 (0), "");
#endif
			break;
		}
		case OP_SSE_RCPSS:
		case OP_SSE_RSQRTSS: {
			IntrinsicId id = (IntrinsicId)0;
			switch (ins->opcode) {
			case OP_SSE_RCPSS: id = INTRINS_SSE_RCP_SS; break;
			case OP_SSE_RSQRTSS: id = INTRINS_SSE_RSQRT_SS; break;
			default: g_assert_not_reached (); break;
			};
			LLVMValueRef result = call_intrins (ctx, id, &rhs, dname);
			const int mask[] = { 0, 5, 6, 7 };
			LLVMValueRef shufmask = create_const_vector_i32 (mask, 4);
			values [ins->dreg] = LLVMBuildShuffleVector (builder, result, lhs, shufmask, "");
			break;
		}
		case OP_XOP: {
			IntrinsicId id = (IntrinsicId)0;
			switch (ins->inst_c0) {
			case SIMD_OP_SSE_LFENCE: id = INTRINS_SSE_LFENCE; break;
			case SIMD_OP_SSE_SFENCE: id = INTRINS_SSE_SFENCE; break;
			case SIMD_OP_SSE_MFENCE: id = INTRINS_SSE_MFENCE; break;
			default: g_assert_not_reached (); break;
			}
			call_intrins (ctx, id, NULL, "");
			break;
		}
		case OP_XOP_X_I:
		case OP_XOP_X_X: {
			IntrinsicId id = (IntrinsicId)0;
			switch (ins->inst_c0) {
			case SIMD_OP_SSE_SQRTPS: id = INTRINS_SSE_SQRT_PS; break;
			case SIMD_OP_SSE_RCPPS: id = INTRINS_SSE_RCP_PS; break;
			case SIMD_OP_SSE_RSQRTPS: id = INTRINS_SSE_RSQRT_PS; break;
			case SIMD_OP_SSE_SQRTPD: id = INTRINS_SSE_SQRT_PD; break;
			case SIMD_OP_SSE_LDDQU: id = INTRINS_SSE_LDU_DQ; break;
			case SIMD_OP_SSE_PHMINPOSUW: id = INTRINS_SSE_PHMINPOSUW; break;
			case SIMD_OP_AES_IMC: id = INTRINS_AESNI_AESIMC; break;
			default: g_assert_not_reached (); break;
			}
			LLVMValueRef arg0 = lhs;
			values [ins->dreg] = call_intrins (ctx, id, &arg0, "");
			break;
		}
		case OP_XOP_I4_X:
		case OP_XOP_I8_X: {
			IntrinsicId id = (IntrinsicId)0;
			switch (ins->inst_c0) {
			case SIMD_OP_SSE_CVTSS2SI: id = INTRINS_SSE_CVTSS2SI; break;
			case SIMD_OP_SSE_CVTTSS2SI: id = INTRINS_SSE_CVTTSS2SI; break;
			case SIMD_OP_SSE_CVTSS2SI64: id = INTRINS_SSE_CVTSS2SI64; break;
			case SIMD_OP_SSE_CVTTSS2SI64: id = INTRINS_SSE_CVTTSS2SI64; break;
			case SIMD_OP_SSE_CVTSD2SI: id = INTRINS_SSE_CVTSD2SI; break;
			case SIMD_OP_SSE_CVTTSD2SI: id = INTRINS_SSE_CVTTSD2SI; break;
			case SIMD_OP_SSE_CVTSD2SI64: id = INTRINS_SSE_CVTSD2SI64; break;
			case SIMD_OP_SSE_CVTTSD2SI64: id = INTRINS_SSE_CVTTSD2SI64; break;
			default: g_assert_not_reached (); break;
			}
			values [ins->dreg] = call_intrins (ctx, id, &lhs, "");
			break;
		}
		case OP_XOP_I4_X_X: {
			gboolean to_i8_t = FALSE;
			gboolean ret_bool = FALSE;
			IntrinsicId id = (IntrinsicId)0;
			switch (ins->inst_c0) {
			case SIMD_OP_SSE_TESTC:  id = INTRINS_SSE_TESTC;  to_i8_t = TRUE; ret_bool = TRUE; break;
			case SIMD_OP_SSE_TESTZ:  id = INTRINS_SSE_TESTZ;  to_i8_t = TRUE; ret_bool = TRUE; break;
			case SIMD_OP_SSE_TESTNZ: id = INTRINS_SSE_TESTNZ; to_i8_t = TRUE; ret_bool = TRUE; break;
			default: g_assert_not_reached (); break;
			}
			LLVMValueRef args [] = { lhs, rhs };
			if (to_i8_t) {
				args [0] = convert (ctx, args [0], sse_i8_t);
				args [1] = convert (ctx, args [1], sse_i8_t);
			}
			
			LLVMValueRef call = call_intrins (ctx, id, args, "");
			if (ret_bool) {
				// if return type is bool (it's still i32) we need to normalize it to 1/0
				LLVMValueRef cmp_zero = LLVMBuildICmp (builder, LLVMIntNE, call, LLVMConstInt (LLVMInt32Type (), 0, FALSE), "");
				values [ins->dreg] = LLVMBuildZExt (builder, cmp_zero, LLVMInt8Type (), "");
			} else {
				values [ins->dreg] = call;
			}
			break;
		}
		case OP_XOP_X_X_X:
		case OP_XOP_X_X_I4:
		case OP_XOP_X_X_I8: {
			LLVMValueRef args [] = { lhs, rhs };
			IntrinsicId id = (IntrinsicId)0;
			switch (ins->inst_c0) {
			case SIMD_OP_SSE_CVTSD2SS: id = INTRINS_SSE_CVTSD2SS; break; 
			case SIMD_OP_SSE_MAXPS: id = INTRINS_SSE_MAXPS; break;
			case SIMD_OP_SSE_MAXSS: id = INTRINS_SSE_MAXSS; break;
			case SIMD_OP_SSE_MINPS: id = INTRINS_SSE_MINPS; break;
			case SIMD_OP_SSE_MINSS: id = INTRINS_SSE_MINSS; break;
			case SIMD_OP_SSE_MAXPD: id = INTRINS_SSE_MAXPD; break;
			case SIMD_OP_SSE_MAXSD: id = INTRINS_SSE_MAXSD; break;
			case SIMD_OP_SSE_MINPD: id = INTRINS_SSE_MINPD; break;
			case SIMD_OP_SSE_MINSD: id = INTRINS_SSE_MINSD; break;
			case SIMD_OP_SSE_PMADDWD: id = INTRINS_SSE_PMADDWD; break;
			case SIMD_OP_SSE_PMULHW: id = INTRINS_SSE_PMULHW; break;
			case SIMD_OP_SSE_PMULHUW: id = INTRINS_SSE_PMULHUW; break;
			case SIMD_OP_SSE_PACKSSWB: id = INTRINS_SSE_PACKSSWB; break;
			case SIMD_OP_SSE_PACKSSDW: id = INTRINS_SSE_PACKSSDW; break;
			case SIMD_OP_SSE_PSRLW_IMM: id = INTRINS_SSE_PSRLI_W; break;
			case SIMD_OP_SSE_PSRLD_IMM: id = INTRINS_SSE_PSRLI_D; break;
			case SIMD_OP_SSE_PSRLQ_IMM: id = INTRINS_SSE_PSRLI_Q; break;
			case SIMD_OP_SSE_PSRLW: id = INTRINS_SSE_PSRL_W; break;
			case SIMD_OP_SSE_PSRLD: id = INTRINS_SSE_PSRL_D; break;
			case SIMD_OP_SSE_PSRLQ: id = INTRINS_SSE_PSRL_Q; break;
			case SIMD_OP_SSE_PSLLW_IMM: id = INTRINS_SSE_PSLLI_W; break;
			case SIMD_OP_SSE_PSLLD_IMM: id = INTRINS_SSE_PSLLI_D; break;
			case SIMD_OP_SSE_PSLLQ_IMM: id = INTRINS_SSE_PSLLI_Q; break;
			case SIMD_OP_SSE_PSLLW: id = INTRINS_SSE_PSLL_W; break;
			case SIMD_OP_SSE_PSLLD: id = INTRINS_SSE_PSLL_D; break;
			case SIMD_OP_SSE_PSLLQ: id = INTRINS_SSE_PSLL_Q; break;
			case SIMD_OP_SSE_PSRAW_IMM: id = INTRINS_SSE_PSRAI_W; break;
			case SIMD_OP_SSE_PSRAD_IMM: id = INTRINS_SSE_PSRAI_D; break;
			case SIMD_OP_SSE_PSRAW: id = INTRINS_SSE_PSRA_W; break;
			case SIMD_OP_SSE_PSRAD: id = INTRINS_SSE_PSRA_D; break;
			case SIMD_OP_SSE_PSADBW: id = INTRINS_SSE_PSADBW; break;
			case SIMD_OP_SSE_ADDSUBPS: id = INTRINS_SSE_ADDSUBPS; break;
			case SIMD_OP_SSE_ADDSUBPD: id = INTRINS_SSE_ADDSUBPD; break;
			case SIMD_OP_SSE_HADDPS: id = INTRINS_SSE_HADDPS; break;
			case SIMD_OP_SSE_HADDPD: id = INTRINS_SSE_HADDPD; break;
			case SIMD_OP_SSE_PHADDW: id = INTRINS_SSE_PHADDW; break;
			case SIMD_OP_SSE_PHADDD: id = INTRINS_SSE_PHADDD; break;
			case SIMD_OP_SSE_PHSUBW: id = INTRINS_SSE_PHSUBW; break;
			case SIMD_OP_SSE_PHSUBD: id = INTRINS_SSE_PHSUBD; break;
			case SIMD_OP_SSE_HSUBPS: id = INTRINS_SSE_HSUBPS; break;
			case SIMD_OP_SSE_HSUBPD: id = INTRINS_SSE_HSUBPD; break;
			case SIMD_OP_SSE_PHADDSW: id = INTRINS_SSE_PHADDSW; break;
			case SIMD_OP_SSE_PHSUBSW: id = INTRINS_SSE_PHSUBSW; break;
			case SIMD_OP_SSE_PSIGNB: id = INTRINS_SSE_PSIGNB; break;
			case SIMD_OP_SSE_PSIGNW: id = INTRINS_SSE_PSIGNW; break;
			case SIMD_OP_SSE_PSIGND: id = INTRINS_SSE_PSIGND; break;
			case SIMD_OP_SSE_PMADDUBSW: id = INTRINS_SSE_PMADDUBSW; break;
			case SIMD_OP_SSE_PMULHRSW: id = INTRINS_SSE_PMULHRSW; break;
			case SIMD_OP_SSE_PACKUSDW: id = INTRINS_SSE_PACKUSDW; break;
			case SIMD_OP_AES_DEC: id = INTRINS_AESNI_AESDEC; break;
			case SIMD_OP_AES_DECLAST: id = INTRINS_AESNI_AESDECLAST; break;
			case SIMD_OP_AES_ENC: id = INTRINS_AESNI_AESENC; break;
			case SIMD_OP_AES_ENCLAST: id = INTRINS_AESNI_AESENCLAST; break;
			default: g_assert_not_reached (); break;
			}

			values [ins->dreg] = call_intrins (ctx, id, args, "");
			break;
		}

		case OP_SSE2_MASKMOVDQU: {
			LLVMTypeRef i8ptr = LLVMPointerType (LLVMInt8Type (), 0);
			LLVMValueRef dstaddr = convert (ctx, values [ins->sreg3], i8ptr);
			LLVMValueRef src = convert (ctx, lhs, sse_i1_t);
			LLVMValueRef mask = convert (ctx, rhs, sse_i1_t);
			LLVMValueRef args[] = { src, mask, dstaddr };
			call_intrins (ctx, INTRINS_SSE_MASKMOVDQU, args, "");
			break;
		}

		case OP_PADDB_SAT:
		case OP_PADDW_SAT:
		case OP_PSUBB_SAT:
		case OP_PSUBW_SAT:
		case OP_PADDB_SAT_UN:
		case OP_PADDW_SAT_UN:
		case OP_PSUBB_SAT_UN:
		case OP_PSUBW_SAT_UN:
		case OP_SSE2_ADDS:
		case OP_SSE2_SUBS: {
			IntrinsicId id = (IntrinsicId)0;
			int type = 0;
			gboolean is_add = TRUE;
			switch (ins->opcode) {
			case OP_PADDB_SAT: type = MONO_TYPE_I1; break;
			case OP_PADDW_SAT: type = MONO_TYPE_I2; break;
			case OP_PSUBB_SAT: type = MONO_TYPE_I1; is_add = FALSE; break;
			case OP_PSUBW_SAT: type = MONO_TYPE_I2; is_add = FALSE; break;
			case OP_PADDB_SAT_UN: type = MONO_TYPE_U1; break;
			case OP_PADDW_SAT_UN: type = MONO_TYPE_U2; break;
			case OP_PSUBB_SAT_UN: type = MONO_TYPE_U1; is_add = FALSE; break;
			case OP_PSUBW_SAT_UN: type = MONO_TYPE_U2; is_add = FALSE; break;
			case OP_SSE2_ADDS: type = ins->inst_c1; break;
			case OP_SSE2_SUBS: type = ins->inst_c1; is_add = FALSE; break;
			default: g_assert_not_reached ();
			}
			if (is_add) {
				switch (type) {
				case MONO_TYPE_I1: id = INTRINS_SSE_SADD_SATI8; break;
				case MONO_TYPE_U1: id = INTRINS_SSE_UADD_SATI8; break;
				case MONO_TYPE_I2: id = INTRINS_SSE_SADD_SATI16; break;
				case MONO_TYPE_U2: id = INTRINS_SSE_UADD_SATI16; break;
				default: g_assert_not_reached (); break;
				}
			} else {
				switch (type) {
				case MONO_TYPE_I1: id = INTRINS_SSE_SSUB_SATI8; break;
				case MONO_TYPE_U1: id = INTRINS_SSE_USUB_SATI8; break;
				case MONO_TYPE_I2: id = INTRINS_SSE_SSUB_SATI16; break;
				case MONO_TYPE_U2: id = INTRINS_SSE_USUB_SATI16; break;
				default: g_assert_not_reached (); break;
				}
			}
			LLVMTypeRef vecty = type_to_sse_type (type);
			LLVMValueRef args [] = { convert (ctx, lhs, vecty), convert (ctx, rhs, vecty) };
			LLVMValueRef result = call_intrins (ctx, id, args, dname);
			values [ins->dreg] = convert (ctx, result, vecty);
			break;
		}

		case OP_SSE2_PACKUS: {
			LLVMValueRef args [2];
			args [0] = convert (ctx, lhs, sse_i2_t);
			args [1] = convert (ctx, rhs, sse_i2_t);
			values [ins->dreg] = convert (ctx, 
				call_intrins (ctx, INTRINS_SSE_PACKUSWB, args, dname),
				type_to_sse_type (ins->inst_c1));
			break;
		}

		case OP_SSE2_SRLI: {
			LLVMValueRef args [] = { lhs, rhs };
			values [ins->dreg] = convert (ctx, 
				call_intrins (ctx, INTRINS_SSE_PSRLI_W, args, dname),
				type_to_sse_type (ins->inst_c1));
			break;
		}

		case OP_SSE2_PSLLDQ:
		case OP_SSE2_PSRLDQ: {
			LLVMBasicBlockRef bbs [16 + 1];
			LLVMValueRef switch_ins;
			LLVMValueRef value = lhs;
			LLVMValueRef index = rhs;
			LLVMValueRef phi_values [16 + 1];
			LLVMTypeRef t = sse_i1_t;
			int nelems = 16;
			int i;
			gboolean shift_right = (ins->opcode == OP_SSE2_PSRLDQ);

			value = convert (ctx, value, t);

			// No corresponding LLVM intrinsics
			// FIXME: Optimize const count
			for (i = 0; i < nelems; ++i)
				bbs [i] = gen_bb (ctx, "PSLLDQ_CASE_BB");
			bbs [nelems] = gen_bb (ctx, "PSLLDQ_DEF_BB");
			cbb = gen_bb (ctx, "PSLLDQ_COND_BB");

			switch_ins = LLVMBuildSwitch (builder, index, bbs [nelems], 0);
			for (i = 0; i < nelems; ++i) {
				LLVMAddCase (switch_ins, LLVMConstInt (LLVMInt32Type (), i, FALSE), bbs [i]);
				LLVMPositionBuilderAtEnd (builder, bbs [i]);

				int mask_values [16];
				// Implement shift using a shuffle
				if (shift_right) {
					for (int j = 0; j < nelems - i; ++j)
						mask_values [j] = i + j;
					for (int j = nelems -i ; j < nelems; ++j)
						mask_values [j] = nelems;
				} else {
					for (int j = 0; j < i; ++j)
						mask_values [j] = nelems;
					for (int j = 0; j < nelems - i; ++j)
						mask_values [j + i] = j;
				}
				phi_values [i] = LLVMBuildShuffleVector (builder, value, LLVMGetUndef (t), create_const_vector_i32 (mask_values, nelems), "");
				LLVMBuildBr (builder, cbb);
			}
			/* Default case */
			LLVMPositionBuilderAtEnd (builder, bbs [nelems]);
			phi_values [nelems] = LLVMConstNull (t);
			LLVMBuildBr (builder, cbb);

			LLVMPositionBuilderAtEnd (builder, cbb);
			values [ins->dreg] = LLVMBuildPhi (builder, LLVMTypeOf (phi_values [0]), "");
			LLVMAddIncoming (values [ins->dreg], phi_values, bbs, nelems + 1);
			values [ins->dreg] = convert (ctx, values [ins->dreg], type_to_sse_type (ins->inst_c1));

			ctx->bblocks [bb->block_num].end_bblock = cbb;
			break;
		}

		case OP_SSE2_PSRAW_IMM:
		case OP_SSE2_PSRAD_IMM:
		case OP_SSE2_PSRLW_IMM:
		case OP_SSE2_PSRLD_IMM:
		case OP_SSE2_PSRLQ_IMM: {
			LLVMValueRef value = lhs;
			LLVMValueRef index = rhs;
			IntrinsicId id;

			// FIXME: Optimize const index case

			/* Use the non-immediate version */
			switch (ins->opcode) {
			case OP_SSE2_PSRAW_IMM: id = INTRINS_SSE_PSRA_W; break;
			case OP_SSE2_PSRAD_IMM: id = INTRINS_SSE_PSRA_D; break;
			case OP_SSE2_PSRLW_IMM: id = INTRINS_SSE_PSRL_W; break;
			case OP_SSE2_PSRLD_IMM: id = INTRINS_SSE_PSRL_D; break;
			case OP_SSE2_PSRLQ_IMM: id = INTRINS_SSE_PSRL_Q; break;
			default: g_assert_not_reached (); break;
			}

			LLVMTypeRef t = LLVMTypeOf (value);
			LLVMValueRef index_vect = LLVMBuildInsertElement (builder, LLVMConstNull (t), convert (ctx, index, LLVMGetElementType (t)), const_int32 (0), "");
			LLVMValueRef args [] = { value, index_vect };
			values [ins->dreg] = call_intrins (ctx, id, args, "");
			break;
		}

		case OP_SSE2_SHUFPD:
		case OP_SSE2_PSHUFD:
		case OP_SSE2_PSHUFHW:
		case OP_SSE2_PSHUFLW: {
			LLVMBasicBlockRef bbs [256 + 1];
			LLVMValueRef switch_ins;
			LLVMValueRef v1, v2, mask;
			LLVMValueRef phi_values [256 + 1];
			int ncases;

			// FIXME: Optimize constant shuffle mask

			if (ins->opcode == OP_SSE2_SHUFPD) {
				/* 3 parameter version */
				v1 = lhs;
				v2 = rhs;
				mask = values [ins->sreg3];
				ncases = 4;
			} else {
				/* 2 parameter version */
				v1 = v2 = lhs;
				mask = rhs;
				ncases = 256;
			}

			for (int i = 0; i < ncases; ++i)
				bbs [i] = gen_bb (ctx, "PSHUFHW_CASE_BB");
			cbb = gen_bb (ctx, "PSHUFHW_COND_BB");
			/* No default case */
			switch_ins = LLVMBuildSwitch (builder, mask, bbs [0], 0);
			for (int i = 0; i < ncases; ++i) {
				LLVMAddCase (switch_ins, LLVMConstInt (LLVMInt32Type (), i, FALSE), bbs [i]);
				LLVMPositionBuilderAtEnd (builder, bbs [i]);

				/* Convert the x86 shuffle mask to LLVM's */
				guint32 imask = i;
				int mask_values [8];
				int mask_len = 0;
				switch (ins->opcode) {
				case OP_SSE2_SHUFPD:
					/* Bit 0 selects v1[0] or v1[1], bit 1 selects v2[0] or v2[1] */
					mask_len = 2;
					mask_values [0] = ((imask >> 0) & 1);
					mask_values [1] = ((imask >> 1) & 1) + 2;
					break;
				case OP_SSE2_PSHUFD:
					/*
					 * Each 2 bits in mask selects 1 dword from the the source and copies it to the
					 * destination.
					 */
					mask_len = 4;
					for (int j = 0; j < 4; ++j) {
						int windex = (imask >> (j * 2)) & 0x3;
						mask_values [j] = windex;
					}
					break;
				case OP_SSE2_PSHUFHW:
					/*
					 * Each 2 bits in mask selects 1 word from the high quadword of the source and copies it to the
					 * high quadword of the destination.
					 */
					mask_len = 8;
					/* The low quadword stays the same */
					for (int j = 0; j < 4; ++j)
						mask_values [j] = j;
					for (int j = 0; j < 4; ++j) {
						int windex = (imask >> (j * 2)) & 0x3;
						mask_values [j + 4] = 4 + windex;
					}
					break;
				case OP_SSE2_PSHUFLW:
					mask_len = 8;
					/* The high quadword stays the same */
					for (int j = 0; j < 4; ++j)
						mask_values [j + 4] = j + 4;
					for (int j = 0; j < 4; ++j) {
						int windex = (imask >> (j * 2)) & 0x3;
						mask_values [j] = windex;
					}
					break;
				default:
					g_assert_not_reached ();
					break;
				}
				phi_values [i] = LLVMBuildShuffleVector (builder, v1, v2, create_const_vector_i32 (mask_values, mask_len), "");
				LLVMBuildBr (builder, cbb);
			}

			LLVMPositionBuilderAtEnd (builder, cbb);
			values [ins->dreg] = LLVMBuildPhi (builder, LLVMTypeOf (phi_values [0]), "");
			LLVMAddIncoming (values [ins->dreg], phi_values, bbs, ncases);
			break;
		}

		case OP_SSE3_MOVDDUP: {
			int mask [] = { 0, 0 };
			values [ins->dreg] = LLVMBuildShuffleVector (builder, lhs,
				LLVMGetUndef (LLVMTypeOf (lhs)),
				create_const_vector_i32 (mask, 2), "");
			break;
		}
		case OP_SSE3_MOVDDUP_MEM: {
			int mask [] = { 0, 0 };
			LLVMTypeRef t = type_to_sse_type (ins->inst_c1);
			LLVMValueRef value = mono_llvm_build_load (builder, t, convert (ctx, lhs, LLVMPointerType (t, 0)), "", FALSE);
			values [ins->dreg] = LLVMBuildShuffleVector (builder, value, LLVMGetUndef (LLVMTypeOf (value)), create_const_vector_i32 (mask, 2), "");
			break;
		}
		case OP_SSE3_MOVSHDUP: {
			int mask [] = { 1, 1, 3, 3 };
			values [ins->dreg] = LLVMBuildShuffleVector (builder, lhs, LLVMConstNull (LLVMTypeOf (lhs)), create_const_vector_i32 (mask, 4), "");
			break;
		}
		case OP_SSE3_MOVSLDUP: {
			int mask [] = { 0, 0, 2, 2 };
			values [ins->dreg] = LLVMBuildShuffleVector (builder, lhs, LLVMConstNull (LLVMTypeOf (lhs)), create_const_vector_i32 (mask, 4), "");
			break;
		}

		case OP_SSSE3_SHUFFLE: {
			LLVMValueRef args [] = { lhs, rhs };
			values [ins->dreg] = call_intrins (ctx, INTRINS_SSE_PSHUFB, args, dname);
			break;
		}

		case OP_SSSE3_ABS: {
			// %sub = sub <16 x i8> zeroinitializer, %arg
			// %cmp = icmp sgt <16 x i8> %arg, zeroinitializer
			// %abs = select <16 x i1> %cmp, <16 x i8> %arg, <16 x i8> %sub
			LLVMTypeRef typ = type_to_sse_type (ins->inst_c1);
			LLVMValueRef sub = LLVMBuildSub(builder, LLVMConstNull(typ), lhs, "");
			LLVMValueRef cmp = LLVMBuildICmp(builder, LLVMIntSGT, lhs, LLVMConstNull(typ), "");
			LLVMValueRef abs = LLVMBuildSelect (builder, cmp, lhs, sub, "");
			values [ins->dreg] = convert (ctx, abs, typ);
			break;
		}
		
		case OP_SSSE3_ALIGNR: {
			LLVMValueRef mask_values [16];
			for (int i = 0; i < 16; i++)
				mask_values [i] = LLVMConstInt (LLVMInt32Type (), i + ins->inst_c0, FALSE);
			LLVMValueRef shuffled = LLVMBuildShuffleVector (builder, 
				convert (ctx, rhs, sse_i1_t),
				convert (ctx, lhs, sse_i1_t),
				LLVMConstVector (mask_values, 16), "");
			values [ins->dreg] = convert (ctx, shuffled, type_to_sse_type (ins->inst_c1));
			break;
		}

		case OP_CREATE_SCALAR:
		case OP_CREATE_SCALAR_UNSAFE: {
			MonoTypeEnum primty = inst_c1_type (ins);
			LLVMTypeRef type = type_to_sse_type (primty);
			// use undef vector (most likely empty but may contain garbage values) for OP_CREATE_SCALAR_UNSAFE
			// and zero one for OP_CREATE_SCALAR
			LLVMValueRef vector = (ins->opcode == OP_CREATE_SCALAR) ? LLVMConstNull (type) : LLVMGetUndef (type);
			LLVMValueRef insert_pos = LLVMConstInt (LLVMInt32Type (), 0, FALSE);
			LLVMValueRef val = convert_full (ctx, lhs, primitive_type_to_llvm_type (primty), primitive_type_is_unsigned (primty));
			values [ins->dreg] = LLVMBuildInsertElement (builder, vector, val, insert_pos, "");
			break;
		}
		case OP_SSE41_ROUNDP: {
			LLVMValueRef args [] = { lhs, LLVMConstInt (LLVMInt32Type (), ins->inst_c0, FALSE) };
			values [ins->dreg] = call_intrins (ctx, ins->inst_c1 == MONO_TYPE_R4 ? INTRINS_SSE_ROUNDPS : INTRINS_SSE_ROUNDPD, args, dname);
			break;
		}
		case OP_SSE41_ROUNDS: {
			LLVMValueRef args [3];
			args [0] = lhs;
			args [1] = rhs;
			args [2] = LLVMConstInt (LLVMInt32Type (), ins->inst_c0, FALSE);
			values [ins->dreg] = call_intrins (ctx, ins->inst_c1 == MONO_TYPE_R4 ? INTRINS_SSE_ROUNDSS : INTRINS_SSE_ROUNDSD, args, dname);
			break;
		}

		case OP_SSE41_DPPS_IMM: {
			LLVMValueRef args [] = { lhs, rhs, LLVMConstInt (LLVMInt8Type (), ins->inst_c0, FALSE) };
			values [ins->dreg] = call_intrins (ctx, INTRINS_SSE_DPPS, args, dname);
			break;
		}

		case OP_SSE41_DPPD_IMM: {
			LLVMValueRef args [] = { lhs, rhs, LLVMConstInt (LLVMInt8Type (), ins->inst_c0, FALSE) };
			values [ins->dreg] = call_intrins (ctx, INTRINS_SSE_DPPD, args, dname);
			break;
		}

		case OP_SSE41_MPSADBW_IMM: {
			LLVMValueRef args [3];
			args [0] = LLVMBuildBitCast (ctx->builder, lhs, sse_i1_t, "");
			args [1] = LLVMBuildBitCast (ctx->builder, rhs, sse_i1_t, "");
			args [2] = LLVMConstInt (LLVMInt8Type (), ins->inst_c0, FALSE);
			values [ins->dreg] = call_intrins (ctx, INTRINS_SSE_MPSADBW, args, dname);
			break;
		}

		case OP_SSE41_INSERT: {
			if (ins->inst_c1 == MONO_TYPE_R4) {
				// special case for <float> overload
				LLVMValueRef args [3];
				args [0] = values [ins->sreg1];
				args [1] = values [ins->sreg2];
				args [2] = convert (ctx, values [ins->sreg3], LLVMInt8Type ());
				values [ins->dreg] = call_intrins (ctx, INTRINS_SSE_INSERTPS, args, dname);
			} else {
				// other overloads are implemented with `insertelement`
				values [ins->dreg] = LLVMBuildInsertElement (builder, 
					values [ins->sreg1], 
					convert (ctx, values [ins->sreg2], primitive_type_to_llvm_type (inst_c1_type (ins))),
					convert (ctx, values [ins->sreg3], LLVMInt8Type ()), dname);
			}
			break;
		}

		case OP_SSE41_BLEND_IMM: {
			int nelem = LLVMGetVectorSize (LLVMTypeOf (lhs));
			g_assert(nelem >= 2 && nelem <= 8); // I2, U2, R4, R8
			
			int mask_values [8];
			for (int i = 0; i < nelem; i++) {
				// n-bit in inst_c0 (control byte) is set to 1
				gboolean bit_set = ((ins->inst_c0 & ( 1 << i )) >> i);
				mask_values [i] = i + (bit_set ? 1 : 0) * nelem;
			}
			
			LLVMValueRef mask = create_const_vector_i32 (mask_values, nelem);
			values [ins->dreg] = LLVMBuildShuffleVector (builder, lhs, rhs, mask, "");
			break;
		}

		case OP_SSE41_BLENDV: {
			LLVMValueRef args [] = { lhs, rhs, values [ins->sreg3] };
			if (ins->inst_c1 == MONO_TYPE_R4) {
				values [ins->dreg] = call_intrins (ctx, INTRINS_SSE_BLENDVPS, args, dname);
			} else if (ins->inst_c1 == MONO_TYPE_R8) {
				values [ins->dreg] = call_intrins (ctx, INTRINS_SSE_BLENDVPD, args, dname);
			} else {
				// for other non-fp type just convert to <16 x i8> and pass to @llvm.x86.sse41.pblendvb
				args [0] = LLVMBuildBitCast (ctx->builder, args [0], sse_i1_t, "");
				args [1] = LLVMBuildBitCast (ctx->builder, args [1], sse_i1_t, "");
				args [2] = LLVMBuildBitCast (ctx->builder, args [2], sse_i1_t, "");
				values [ins->dreg] = call_intrins (ctx, INTRINS_SSE_PBLENDVB, args, dname);
			}
			break;
		}

		case OP_SSE_CVTII: {
			gboolean is_signed = (ins->inst_c1 == MONO_TYPE_I1) || 
				(ins->inst_c1 == MONO_TYPE_I2) || (ins->inst_c1 == MONO_TYPE_I4);

			LLVMTypeRef vec_type;
			if ((ins->inst_c1 == MONO_TYPE_I1) || (ins->inst_c1 == MONO_TYPE_U1))
				vec_type = sse_i1_t;
			else if ((ins->inst_c1 == MONO_TYPE_I2) || (ins->inst_c1 == MONO_TYPE_U2))
				vec_type = sse_i2_t;
			else
				vec_type = sse_i4_t;

			LLVMValueRef value;
			if (LLVMGetTypeKind (LLVMTypeOf (lhs)) != LLVMVectorTypeKind) {
				LLVMValueRef bitcasted = LLVMBuildBitCast (ctx->builder, lhs, LLVMPointerType (vec_type, 0), "");
				value = mono_llvm_build_aligned_load (builder, vec_type, bitcasted, "", FALSE, 1);
			} else {
				value = LLVMBuildBitCast (ctx->builder, lhs, vec_type, "");
			}

			const int mask_values [] = { 0, 1, 2, 3, 4, 5, 6, 7 };
			LLVMValueRef mask_vec;
			LLVMTypeRef dst_type;
			if (ins->inst_c0 == MONO_TYPE_I2) {
				mask_vec = create_const_vector_i32 (mask_values, 8);
				dst_type = sse_i2_t;
			} else if (ins->inst_c0 == MONO_TYPE_I4) {
				mask_vec = create_const_vector_i32 (mask_values, 4);
				dst_type = sse_i4_t;
			} else {
				g_assert (ins->inst_c0 == MONO_TYPE_I8);
				mask_vec = create_const_vector_i32 (mask_values, 2);
				dst_type = sse_i8_t;
			}

			LLVMValueRef shuffled = LLVMBuildShuffleVector (builder, value,
				LLVMGetUndef (vec_type), mask_vec, "");

			if (is_signed)
				values [ins->dreg] = LLVMBuildSExt (ctx->builder, shuffled, dst_type, "");
			else
				values [ins->dreg] = LLVMBuildZExt (ctx->builder, shuffled, dst_type, "");
			break;
		}

		case OP_SSE41_LOADANT: {
			LLVMValueRef dst_ptr = convert (ctx, lhs, LLVMPointerType (primitive_type_to_llvm_type (inst_c1_type (ins)), 0));
			LLVMValueRef dst_vec = LLVMBuildBitCast (builder, dst_ptr, LLVMPointerType (type_to_sse_type (ins->inst_c1), 0), "");
			LLVMValueRef load = mono_llvm_build_aligned_load (builder, type_to_sse_type (ins->inst_c1), dst_vec, "", FALSE, 16);
			set_nontemporal_flag (load);
			values [ins->dreg] = load;
			break;
		}

		case OP_SSE41_MUL: {
#if LLVM_API_VERSION < 700
			LLVMValueRef args [] = { lhs, rhs };
			values [ins->dreg] = call_intrins (ctx, INTRINS_SSE_PMULDQ, args, dname);
#else
			const int shift_vals [] = { 32, 32 };
			const LLVMValueRef args [] = {
				convert (ctx, lhs, sse_i8_t),
				convert (ctx, rhs, sse_i8_t),
			};
			LLVMValueRef mul_args [2] = { 0 };
			LLVMValueRef shift_vec = create_const_vector (LLVMInt64Type (), shift_vals, 2);
			for (int i = 0; i < 2; ++i) {
				LLVMValueRef padded = LLVMBuildShl (builder, args [i], shift_vec, "");
				mul_args[i] = mono_llvm_build_exact_ashr (builder, padded, shift_vec);
			}
			values [ins->dreg] = LLVMBuildNSWMul (builder, mul_args [0], mul_args [1], dname);
#endif
			break;	
		}

		case OP_SSE41_MULLO: {
			values [ins->dreg] = LLVMBuildMul (ctx->builder, lhs, rhs, "");
			break;	
		}

		case OP_SSE42_CRC32:
		case OP_SSE42_CRC64: {
			LLVMValueRef args [2];
			args [0] = lhs;
			args [1] = convert (ctx, rhs, primitive_type_to_llvm_type ((MonoTypeEnum)ins->inst_c0));
			IntrinsicId id;
			switch (ins->inst_c0) {
			case MONO_TYPE_U1: id = INTRINS_SSE_CRC32_32_8; break;
			case MONO_TYPE_U2: id = INTRINS_SSE_CRC32_32_16; break;
			case MONO_TYPE_U4: id = INTRINS_SSE_CRC32_32_32; break;
			case MONO_TYPE_U8: id = INTRINS_SSE_CRC32_64_64; break;
			default: g_assert_not_reached (); break;
			}
			values [ins->dreg] = call_intrins (ctx, id, args, "");
			break;
		}

		case OP_PCLMULQDQ_IMM: {
			LLVMValueRef args [] = { lhs, rhs, LLVMConstInt (LLVMInt8Type (), ins->inst_c0, FALSE) };
			values [ins->dreg] = call_intrins (ctx, INTRINS_PCLMULQDQ, args, "");
			break;
		}

		case OP_AES_KEYGEN_IMM: {
			LLVMValueRef args [] = { lhs, LLVMConstInt (LLVMInt8Type (), ins->inst_c0, FALSE) };
			values [ins->dreg] = call_intrins (ctx, INTRINS_AESNI_AESKEYGENASSIST, args, "");
			break;
		}
#endif

#ifdef ENABLE_NETCORE
		case OP_XCAST: {
			LLVMTypeRef t = simd_class_to_llvm_type (ctx, ins->klass);

			values [ins->dreg] = LLVMBuildBitCast (builder, lhs, t, "");
			break;
		}
		case OP_XCOMPARE_FP: {
			LLVMRealPredicate pred = fpcond_to_llvm_cond [ins->inst_c0];
			LLVMValueRef cmp = LLVMBuildFCmp (builder, pred, lhs, rhs, "");
			int nelems = LLVMGetVectorSize (LLVMTypeOf (cmp));
			g_assert (LLVMTypeOf (lhs) == LLVMTypeOf (rhs));
			if (ins->inst_c1 == MONO_TYPE_R8)
				values [ins->dreg] = LLVMBuildBitCast (builder, LLVMBuildSExt (builder, cmp, LLVMVectorType (LLVMInt64Type (), nelems), ""), LLVMTypeOf (lhs), "");
			else
				values [ins->dreg] = LLVMBuildBitCast (builder, LLVMBuildSExt (builder, cmp, LLVMVectorType (LLVMInt32Type (), nelems), ""), LLVMTypeOf (lhs), "");
			break;
		}
		case OP_XCOMPARE: {
			LLVMIntPredicate pred = cond_to_llvm_cond [ins->inst_c0];
			LLVMValueRef cmp = LLVMBuildICmp (builder, pred, lhs, rhs, "");
			g_assert (LLVMTypeOf (lhs) == LLVMTypeOf (rhs));
			values [ins->dreg] = LLVMBuildSExt (builder, cmp, LLVMTypeOf (lhs), "");
			break;
		}
		case OP_XEQUAL: {
			LLVMTypeRef t;
			LLVMValueRef cmp, mask [32], shuffle;
			int nelems;

#if defined(TARGET_WASM) && LLVM_API_VERSION >= 800
			/* The wasm code generator doesn't understand the shuffle/and code sequence below */
			LLVMValueRef val;
			if (LLVMIsNull (lhs) || LLVMIsNull (rhs)) {
				val = LLVMIsNull (lhs) ? rhs : lhs;
				nelems = LLVMGetVectorSize (LLVMTypeOf (lhs));

				IntrinsicId intrins = (IntrinsicId)0;
				switch (nelems) {
				case 16:
					intrins = INTRINS_WASM_ANYTRUE_V16;
					break;
				case 8:
					intrins = INTRINS_WASM_ANYTRUE_V8;
					break;
				case 4:
					intrins = INTRINS_WASM_ANYTRUE_V4;
					break;
				case 2:
					intrins = INTRINS_WASM_ANYTRUE_V2;
					break;
				default:
					g_assert_not_reached ();
				}
				/* res = !wasm.anytrue (val) */
				values [ins->dreg] = call_intrins (ctx, intrins, &val, "");
				values [ins->dreg] = LLVMBuildZExt (builder, LLVMBuildICmp (builder, LLVMIntEQ, values [ins->dreg], LLVMConstInt (LLVMInt32Type (), 0, FALSE), ""), LLVMInt32Type (), dname);
				break;
			}
#endif
			LLVMTypeRef srcelemt = LLVMGetElementType (LLVMTypeOf (lhs));

			//%c = icmp sgt <16 x i8> %a0, %a1
			if (srcelemt == LLVMDoubleType () || srcelemt == LLVMFloatType ())
				cmp = LLVMBuildFCmp (builder, LLVMRealOEQ, lhs, rhs, "");
			else
				cmp = LLVMBuildICmp (builder, LLVMIntEQ, lhs, rhs, "");
			nelems = LLVMGetVectorSize (LLVMTypeOf (cmp));

			LLVMTypeRef elemt;
			if (srcelemt == LLVMDoubleType ())
				elemt = LLVMInt64Type ();
			else if (srcelemt == LLVMFloatType ())
				elemt = LLVMInt32Type ();
			else
				elemt = srcelemt;

			t = LLVMVectorType (elemt, nelems);
			cmp = LLVMBuildSExt (builder, cmp, t, "");
			// cmp is a <nelems x elemt> vector, each element is either 0xff... or 0
			int half = nelems / 2;
			while (half >= 1) {
				// AND the top and bottom halfes into the bottom half
				for (int i = 0; i < half; ++i)
					mask [i] = LLVMConstInt (LLVMInt32Type (), half + i, FALSE);
				for (int i = half; i < nelems; ++i)
					mask [i] = LLVMConstInt (LLVMInt32Type (), 0, FALSE);
				shuffle = LLVMBuildShuffleVector (builder, cmp, LLVMGetUndef (t), LLVMConstVector (mask, LLVMGetVectorSize (t)), "");
				cmp = LLVMBuildAnd (builder, cmp, shuffle, "");
				half = half / 2;
			}
			// Extract [0]
			LLVMValueRef first_elem = LLVMBuildExtractElement (builder, cmp, LLVMConstInt (LLVMInt32Type (), 0, FALSE), "");
			// convert to 0/1
			LLVMValueRef cmp_zero = LLVMBuildICmp (builder, LLVMIntNE, first_elem, LLVMConstInt (elemt, 0, FALSE), "");
			values [ins->dreg] = LLVMBuildZExt (builder, cmp_zero, LLVMInt8Type (), "");
			break;
		}
		case OP_XBINOP: {
			switch (ins->inst_c0) {
			case OP_IADD:
				values [ins->dreg] = LLVMBuildAdd (builder, lhs, rhs, "");
				break;
			case OP_ISUB:
				values [ins->dreg] = LLVMBuildSub (builder, lhs, rhs, "");
				break;
			case OP_IAND:
				values [ins->dreg] = LLVMBuildAnd (builder, lhs, rhs, "");
				break;
			case OP_IOR:
				values [ins->dreg] = LLVMBuildOr (builder, lhs, rhs, "");
				break;
			case OP_IXOR:
				values [ins->dreg] = LLVMBuildXor (builder, lhs, rhs, "");
				break;
			case OP_FADD:
				values [ins->dreg] = LLVMBuildFAdd (builder, lhs, rhs, "");
				break;
			case OP_FSUB:
				values [ins->dreg] = LLVMBuildFSub (builder, lhs, rhs, "");
				break;
			case OP_FMUL:
				values [ins->dreg] = LLVMBuildFMul (builder, lhs, rhs, "");
				break;
			case OP_FDIV:
				values [ins->dreg] = LLVMBuildFDiv (builder, lhs, rhs, "");
				break;
			case OP_FMAX:
			case OP_FMIN: {
#if defined(TARGET_X86) || defined(TARGET_AMD64)
				LLVMValueRef args [] = { lhs, rhs };

				gboolean is_r4 = ins->inst_c1 == MONO_TYPE_R4;
				if (ins->inst_c0 == OP_FMAX)
					values [ins->dreg] = call_intrins (ctx, is_r4 ? INTRINS_SSE_MAXPS : INTRINS_SSE_MAXPD, args, dname);
				else
					values [ins->dreg] = call_intrins (ctx, is_r4 ? INTRINS_SSE_MINPS : INTRINS_SSE_MINPD, args, dname);
#else
				NOT_IMPLEMENTED;
#endif
				break;
			}
			case OP_IMAX: {
				gboolean is_unsigned = ins->inst_c1 == MONO_TYPE_U1 || ins->inst_c1 == MONO_TYPE_U2 || ins->inst_c1 == MONO_TYPE_U4 || ins->inst_c1 == MONO_TYPE_U8;
				LLVMValueRef cmp = LLVMBuildICmp (builder, is_unsigned ? LLVMIntUGT : LLVMIntSGT, lhs, rhs, "");
				values [ins->dreg] = LLVMBuildSelect (builder, cmp, lhs, rhs, "");
				break;
			}
			case OP_IMIN: {
				gboolean is_unsigned = ins->inst_c1 == MONO_TYPE_U1 || ins->inst_c1 == MONO_TYPE_U2 || ins->inst_c1 == MONO_TYPE_U4 || ins->inst_c1 == MONO_TYPE_U8;
				LLVMValueRef cmp = LLVMBuildICmp (builder, is_unsigned ? LLVMIntULT : LLVMIntSLT, lhs, rhs, "");
				values [ins->dreg] = LLVMBuildSelect (builder, cmp, lhs, rhs, "");
			}
			break;

			default:
				g_assert_not_reached ();
			}
			break;
		}
		case OP_XEXTRACT_I32:
		case OP_XEXTRACT_I64:
		case OP_XEXTRACT_R8:
		case OP_XEXTRACT_R4: {
			LLVMTypeRef rhst = LLVMTypeOf (rhs);
			LLVMValueRef mask = NULL;
			switch (ins->opcode) {
			case OP_XEXTRACT_I32: case OP_XEXTRACT_R4:
				mask = LLVMConstInt (rhst, 0x3, FALSE); break;
			case OP_XEXTRACT_I64: case OP_XEXTRACT_R8:
				mask = LLVMConstInt (rhst, 0x1, FALSE); break;
			default:
				g_assert_not_reached ();
			}
			LLVMValueRef selector = LLVMBuildAnd (builder, rhs, mask, "");
			values [ins->dreg] = LLVMBuildExtractElement (builder, lhs, selector, "");
			break;
		}
		case OP_POPCNT32:
			values [ins->dreg] = call_intrins (ctx, INTRINS_CTPOP_I32, &lhs, "");
			break;
		case OP_POPCNT64:
			values [ins->dreg] = call_intrins (ctx, INTRINS_CTPOP_I64, &lhs, "");
			break;
		case OP_CTTZ32:
		case OP_CTTZ64: {
			LLVMValueRef args [2];
			args [0] = lhs;
			args [1] = LLVMConstInt (LLVMInt1Type (), 0, FALSE);
			values [ins->dreg] = call_intrins (ctx, ins->opcode == OP_CTTZ32 ? INTRINS_CTTZ_I32 : INTRINS_CTTZ_I64, args, "");
			break;
		}
		case OP_BEXTR32:
		case OP_BEXTR64: {
			LLVMValueRef args [2];
			args [0] = lhs;
			args [1] = convert (ctx, rhs, ins->opcode == OP_BEXTR32 ? LLVMInt32Type () : LLVMInt64Type ()); // cast ushort to u32/u64
			values [ins->dreg] = call_intrins (ctx, ins->opcode == OP_BEXTR32 ? INTRINS_BEXTR_I32 : INTRINS_BEXTR_I64, args, "");
			break;
		}
		case OP_BZHI32:
		case OP_BZHI64: {
			LLVMValueRef args [2];
			args [0] = lhs;
			args [1] = rhs;
			values [ins->dreg] = call_intrins (ctx, ins->opcode == OP_BZHI32 ? INTRINS_BZHI_I32 : INTRINS_BZHI_I64, args, "");
			break;
		}
		case OP_MULX_H32:
		case OP_MULX_H64:
		case OP_MULX_HL32:
		case OP_MULX_HL64: {
			gboolean is_64 = ins->opcode == OP_MULX_H64 || ins->opcode == OP_MULX_HL64;
			gboolean only_high = ins->opcode == OP_MULX_H32 || ins->opcode == OP_MULX_H64;
			LLVMValueRef lx = LLVMBuildZExt (ctx->builder, lhs, LLVMInt128Type (), "");
			LLVMValueRef rx = LLVMBuildZExt (ctx->builder, rhs, LLVMInt128Type (), "");
			LLVMValueRef mulx = LLVMBuildMul (ctx->builder, lx, rx, "");
			if (!only_high) {
				LLVMValueRef lowx = LLVMBuildTrunc (ctx->builder, mulx, is_64 ? LLVMInt64Type () : LLVMInt32Type (), "");
				LLVMBuildStore (ctx->builder, lowx, values [ins->sreg3]);
			}
			LLVMValueRef shift = LLVMConstInt (LLVMInt128Type (), is_64 ? 64 : 32, FALSE);
			LLVMValueRef highx = LLVMBuildLShr (ctx->builder, mulx, shift, "");
			values [ins->dreg] = LLVMBuildTrunc (ctx->builder, highx, is_64 ? LLVMInt64Type () : LLVMInt32Type (), "");
			break;
		}
		case OP_PEXT32:
		case OP_PEXT64: {
			LLVMValueRef args [2];
			args [0] = lhs;
			args [1] = rhs;
			values [ins->dreg] = call_intrins (ctx, ins->opcode == OP_PEXT32 ? INTRINS_PEXT_I32 : INTRINS_PEXT_I64, args, "");
			break;
		}
		case OP_PDEP32:
		case OP_PDEP64: {
			LLVMValueRef args [2];
			args [0] = lhs;
			args [1] = rhs;
			values [ins->dreg] = call_intrins (ctx, ins->opcode == OP_PDEP32 ? INTRINS_PDEP_I32 : INTRINS_PDEP_I64, args, "");
			break;
		}
#endif /* ENABLE_NETCORE */
#endif /* defined(TARGET_X86) || defined(TARGET_AMD64) */

// Shared between ARM64 and X86
#if defined(ENABLE_NETCORE) && (defined(TARGET_ARM64) || defined(TARGET_X86) || defined(TARGET_AMD64))
		case OP_LZCNT32:
		case OP_LZCNT64: {
			LLVMValueRef args [2];
			args [0] = lhs;
			args [1] = LLVMConstInt (LLVMInt1Type (), 1, FALSE);
			values [ins->dreg] = LLVMBuildCall2 (builder, LLVMGlobalGetValueType (get_intrins (ctx, ins->opcode == OP_LZCNT32 ? INTRINS_CTLZ_I32 : INTRINS_CTLZ_I64)), get_intrins (ctx, ins->opcode == OP_LZCNT32 ? INTRINS_CTLZ_I32 : INTRINS_CTLZ_I64), args, 2, "");
			break;
		}
#endif

#if defined(ENABLE_NETCORE) && defined(TARGET_ARM64)
		case OP_XOP_I4_I4:
		case OP_XOP_I8_I8: {
			IntrinsicId id = (IntrinsicId)0;
			switch (ins->inst_c0) {
			case SIMD_OP_ARM64_RBIT32: id = INTRINS_BITREVERSE_I32; break;
			case SIMD_OP_ARM64_RBIT64: id = INTRINS_BITREVERSE_I64; break;
			default: g_assert_not_reached (); break;
			}
			values [ins->dreg] = call_intrins (ctx, id, &lhs, "");
			break;
		}
		case OP_XOP_X_X_X:
		case OP_XOP_I4_I4_I4:
		case OP_XOP_I4_I4_I8: {
			IntrinsicId id = (IntrinsicId)0;
			gboolean zext_last = FALSE;
			switch (ins->inst_c0) {
			case SIMD_OP_ARM64_CRC32B: id = INTRINS_AARCH64_CRC32B; zext_last = TRUE; break;
			case SIMD_OP_ARM64_CRC32H: id = INTRINS_AARCH64_CRC32H; zext_last = TRUE; break;
			case SIMD_OP_ARM64_CRC32W: id = INTRINS_AARCH64_CRC32W; zext_last = TRUE; break;
			case SIMD_OP_ARM64_CRC32X: id = INTRINS_AARCH64_CRC32X; break;
			case SIMD_OP_ARM64_CRC32CB: id = INTRINS_AARCH64_CRC32CB; zext_last = TRUE; break;
			case SIMD_OP_ARM64_CRC32CH: id = INTRINS_AARCH64_CRC32CH; zext_last = TRUE; break;
			case SIMD_OP_ARM64_CRC32CW: id = INTRINS_AARCH64_CRC32CW; zext_last = TRUE; break;
			case SIMD_OP_ARM64_CRC32CX: id = INTRINS_AARCH64_CRC32CX; break;
			case SIMD_OP_ARM64_SHA1SU1: id = INTRINS_AARCH64_SHA1SU1; break;
			case SIMD_OP_ARM64_SHA256SU0: id = INTRINS_AARCH64_SHA256SU0; break;
			default: g_assert_not_reached (); break;
			}
			LLVMValueRef arg1 = rhs;
			if (zext_last)
				arg1 = LLVMBuildZExt (ctx->builder, arg1, LLVMInt32Type (), "");
			LLVMValueRef args [] = { lhs, arg1 };
			values [ins->dreg] = call_intrins (ctx, id, args, "");
			break;
		}
		case OP_XOP_X_X_X_X: {
			IntrinsicId id = (IntrinsicId)0;
			switch (ins->inst_c0) {
			case SIMD_OP_ARM64_SHA1SU0: id = INTRINS_AARCH64_SHA1SU0; break;
			case SIMD_OP_ARM64_SHA256H: id = INTRINS_AARCH64_SHA256H; break;
			case SIMD_OP_ARM64_SHA256H2: id = INTRINS_AARCH64_SHA256H2; break;
			case SIMD_OP_ARM64_SHA256SU1: id = INTRINS_AARCH64_SHA256SU1; break;
			default: g_assert_not_reached (); break;
			}
			LLVMValueRef args [] = { lhs, rhs, arg3 };
			values [ins->dreg] = call_intrins (ctx, id, args, "");
			break;
		}
		case OP_XOP_X_X: {
			IntrinsicId id = (IntrinsicId)0;
			switch (ins->inst_c0) {
			case SIMD_OP_LLVM_FABS: id = INTRINS_AARCH64_ADV_SIMD_ABS_FLOAT; break;
			case SIMD_OP_LLVM_DABS: id = INTRINS_AARCH64_ADV_SIMD_ABS_DOUBLE; break;
			case SIMD_OP_LLVM_I8ABS: id = INTRINS_AARCH64_ADV_SIMD_ABS_INT8; break;
			case SIMD_OP_LLVM_I16ABS: id = INTRINS_AARCH64_ADV_SIMD_ABS_INT16; break;
			case SIMD_OP_LLVM_I32ABS: id = INTRINS_AARCH64_ADV_SIMD_ABS_INT32; break;
			case SIMD_OP_LLVM_I64ABS: id = INTRINS_AARCH64_ADV_SIMD_ABS_INT64; break;
			default: g_assert_not_reached (); break;
			}

			LLVMValueRef arg0 = lhs;
			values [ins->dreg] = call_intrins (ctx, id, &arg0, "");
			break;
		}
		case OP_LSCNT32:
		case OP_LSCNT64: {
			// %shr = ashr i32 %x, 31
			// %xor = xor i32 %shr, %x
			// %mul = shl i32 %xor, 1
			// %add = or i32 %mul, 1
			// %0 = tail call i32 @llvm.ctlz.i32(i32 %add, i1 false)
			LLVMValueRef shr = LLVMBuildAShr (builder, lhs, ins->opcode == OP_LSCNT32 ? 
				LLVMConstInt (LLVMInt32Type (), 31, FALSE) : 
				LLVMConstInt (LLVMInt64Type (), 63, FALSE), "");
			LLVMValueRef one = ins->opcode == OP_LSCNT32 ? 
				LLVMConstInt (LLVMInt32Type (), 1, FALSE) : 
				LLVMConstInt (LLVMInt64Type (), 1, FALSE);
			LLVMValueRef xor = LLVMBuildXor (builder, shr, lhs, "");
			LLVMValueRef mul = LLVMBuildShl (builder, xor, one, "");
			LLVMValueRef add = LLVMBuildOr (builder, mul, one, "");
			
			LLVMValueRef args [2];
			args [0] = add;
			args [1] = LLVMConstInt (LLVMInt1Type (), 0, FALSE);
			values [ins->dreg] = LLVMBuildCall2 (builder, LLVMGlobalGetValueType (get_intrins (ctx, ins->opcode == OP_LSCNT32 ? INTRINS_CTLZ_I32 : INTRINS_CTLZ_I64)), get_intrins (ctx, ins->opcode == OP_LSCNT32 ? INTRINS_CTLZ_I32 : INTRINS_CTLZ_I64), args, 2, "");
			break;
		}
		case OP_ARM64_SMULH:
		case OP_ARM64_UMULH: {
			LLVMValueRef op1, op2;
			if (ins->opcode == OP_ARM64_SMULH) {
				op1 = LLVMBuildSExt (builder, lhs, LLVMInt128Type (), "");
				op2 = LLVMBuildSExt (builder, rhs, LLVMInt128Type (), "");
			} else {
				op1 = LLVMBuildZExt (builder, lhs, LLVMInt128Type (), "");
				op2 = LLVMBuildZExt (builder, rhs, LLVMInt128Type (), "");
			}
			LLVMValueRef mul = LLVMBuildMul (builder, op1, op2, "");
			LLVMValueRef hi64 = LLVMBuildLShr (builder, mul,
				LLVMConstInt (LLVMInt128Type (), 64, FALSE), "");
			values [ins->dreg] = LLVMBuildTrunc (builder, hi64, LLVMInt64Type (), "");
			break;
		}
#endif

		case OP_DUMMY_USE:
			break;

			/*
			 * EXCEPTION HANDLING
			 */
		case OP_IMPLICIT_EXCEPTION:
			/* This marks a place where an implicit exception can happen */
			if (bb->region != -1)
				set_failure (ctx, "implicit-exception");
			break;
		case OP_THROW:
		case OP_RETHROW: {
			gboolean rethrow = (ins->opcode == OP_RETHROW);
			emit_throw (ctx, bb, rethrow, lhs);
			builder = ctx->builder;
			break;
		}
		case OP_CALL_HANDLER: {
			/* 
			 * We don't 'call' handlers, but instead simply branch to them.
			 * The code generated by ENDFINALLY will branch back to us.
			 */
			LLVMBasicBlockRef noex_bb;
			GSList *bb_list;
			BBInfo *info = &bblocks [ins->inst_target_bb->block_num];

			bb_list = info->call_handler_return_bbs;

			/* 
			 * Set the indicator variable for the finally clause.
			 */
			lhs = info->finally_ind;
			g_assert (lhs);
			LLVMBuildStore (builder, LLVMConstInt (LLVMInt32Type (), g_slist_length (bb_list) + 1, FALSE), lhs);
				
			/* Branch to the finally clause */
			LLVMBuildBr (builder, info->call_handler_target_bb);

			noex_bb = gen_bb (ctx, "CALL_HANDLER_CONT_BB");
			info->call_handler_return_bbs = g_slist_append_mempool (cfg->mempool, info->call_handler_return_bbs, noex_bb);

			builder = ctx->builder = create_builder (ctx);
			LLVMPositionBuilderAtEnd (ctx->builder, noex_bb);

			bblocks [bb->block_num].end_bblock = noex_bb;
			break;
		}
		case OP_START_HANDLER: {
			break;
		}
		case OP_ENDFINALLY: {
			LLVMBasicBlockRef resume_bb;
			MonoBasicBlock *handler_bb;
			LLVMValueRef val, switch_ins, callee;
			GSList *bb_list;
			BBInfo *info;
			gboolean is_fault = MONO_REGION_FLAGS (bb->region) == MONO_EXCEPTION_CLAUSE_FAULT;

			/*
			 * Fault clauses are like finally clauses, but they are only called if an exception is thrown.
			 */
			if (!is_fault) {
				handler_bb = (MonoBasicBlock*)g_hash_table_lookup (ctx->region_to_handler, GUINT_TO_POINTER (mono_get_block_region_notry (cfg, bb->region)));
				g_assert (handler_bb);
				info = &bblocks [handler_bb->block_num];
				lhs = info->finally_ind;
				g_assert (lhs);

				bb_list = info->call_handler_return_bbs;

				resume_bb = gen_bb (ctx, "ENDFINALLY_RESUME_BB");

				/* Load the finally variable */
				val = LLVMBuildLoad2 (builder, LLVMInt32Type (), lhs, "");

				/* Reset the variable */
				LLVMBuildStore (builder, LLVMConstInt (LLVMInt32Type (), 0, FALSE), lhs);

				/* Branch to either resume_bb, or to the bblocks in bb_list */
				switch_ins = LLVMBuildSwitch (builder, val, resume_bb, g_slist_length (bb_list));
				/*
				 * The other targets are added at the end to handle OP_CALL_HANDLER
				 * opcodes processed later.
				 */
				info->endfinally_switch_ins_list = g_slist_append_mempool (cfg->mempool, info->endfinally_switch_ins_list, switch_ins);

				builder = ctx->builder = create_builder (ctx);
				LLVMPositionBuilderAtEnd (ctx->builder, resume_bb);
			}

			{
				LLVMTypeRef icall_sig = LLVMFunctionType (LLVMVoidType (), NULL, 0, FALSE);
				callee = get_jit_callee (ctx, "llvm_resume_unwind_trampoline", icall_sig, MONO_PATCH_INFO_JIT_ICALL_ID, GUINT_TO_POINTER (MONO_JIT_ICALL_mono_llvm_resume_unwind_trampoline));
				LLVMBuildCall2 (builder, icall_sig, callee, NULL, 0, "");
				LLVMBuildUnreachable (builder);
			}

			has_terminator = TRUE;
			break;
		}
		case OP_IL_SEQ_POINT:
			break;
		default: {
			char reason [128];

			sprintf (reason, "opcode %s", mono_inst_name (ins->opcode));
			set_failure (ctx, reason);
			break;
		}
		}

		if (!ctx_ok (ctx))
			break;

		/* Convert the value to the type required by phi nodes */
		if (spec [MONO_INST_DEST] != ' ' && !MONO_IS_STORE_MEMBASE (ins) && ctx->vreg_types [ins->dreg]) {
			if (ctx->is_vphi [ins->dreg])
				/* vtypes */
				values [ins->dreg] = addresses [ins->dreg]->value;
			else
				values [ins->dreg] = convert (ctx, values [ins->dreg], ctx->vreg_types [ins->dreg]);
		}

		/* Add stores for volatile variables */
		if (!skip_volatile_store && spec [MONO_INST_DEST] != ' ' && spec [MONO_INST_DEST] != 'v' && !MONO_IS_STORE_MEMBASE (ins))
			emit_volatile_store (ctx, ins->dreg);
	}

	if (!ctx_ok (ctx))
		return;

	if (!has_terminator && bb->next_bb && (bb == cfg->bb_entry || bb->in_count > 0)) {
		LLVMBuildBr (builder, get_bb (ctx, bb->next_bb));
	}

	if (bb == cfg->bb_exit && sig->ret->type == MONO_TYPE_VOID) {
		LLVMBuildRetVoid (builder);
	}

	if (bb == cfg->bb_entry)
		ctx->last_alloca = LLVMGetLastInstruction (get_bb (ctx, cfg->bb_entry));
}


#endif /* DISABLE_JIT */

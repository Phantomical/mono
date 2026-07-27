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
#include "mono_lsda.hpp"

#include <cctype>
#include <mutex>
#include <string>
#include <unordered_map>

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

/*
 * Tag INS as one half of a class-init barrier for KLASS: "mono.class-init" on
 * the call to the cctor trigger, "mono.class-init-check" on the branch that
 * tests vtable->initialized. The two together let a pass find the barriers in a
 * body, and say which class each one is for, without matching on their shape.
 */
static void
tag_class_init (LLVMValueRef ins, const char *tag, MonoClass *klass)
{
	char *name = mono_class_full_name (klass);

	mono_llvm_add_string_metadata (ins, tag, name);
	g_free (name);
}

/*
 * mono_llvm_method_symbol:
 *
 *   Return a stable, linker-safe symbol name for METHOD, for naming a direct
 * `call @name` edge to its stable trampoline entry (see process_call () below).
 * The same method always gets the same name back - cached for the life of the
 * process, since the name has to stay valid for as long as any JITted caller
 * might still reference it - and two distinct methods never collide.
 *
 *   Adapted from aot-compiler.c's get_debug_sym (): same mangling, but backed
 * by a live, mutex-guarded, process-wide cache instead of a single AOT image's
 * GHashTable, since the JIT compiles methods one at a time for as long as the
 * process runs rather than once per image.
 */
/*
 * The two halves of the method<->symbol mapping. mono_llvm_method_symbol ()
 * fills them; mono_llvm_method_from_symbol () reads `sym_taken` to map a direct
 * `call @name` target back to the MonoMethod it names, which is how the tier-1
 * inliner recovers a call site's callee. Hoisted to file scope (from
 * mono_llvm_method_symbol ()'s locals) so both directions share one table under
 * one lock.
 */
static std::mutex sym_mutex;
static std::unordered_map<MonoMethod *, std::string> *sym_names;
static std::unordered_map<std::string, MonoMethod *> *sym_taken;

const char *
mono_llvm_method_symbol (MonoMethod *method)
{
	std::mutex &mutex = sym_mutex;

	std::lock_guard<std::mutex> lock (mutex);

	if (!sym_names) {
		sym_names = new std::unordered_map<MonoMethod *, std::string> ();
		sym_taken = new std::unordered_map<std::string, MonoMethod *> ();
	}
	auto *names = sym_names;
	auto *taken = sym_taken;

	auto found = names->find (method);
	if (found != names->end ())
		return found->second.c_str ();

	char *full_name = mono_method_full_name (method, TRUE);
	size_t len = strlen (full_name);

	std::string mangled = "mono_method_";
	mangled.reserve (mangled.size () + len);
	for (size_t i = 0; i < len; ++i) {
		char c = full_name [i];

		if (i == 0 && c >= '0' && c <= '9')
			mangled += '_';
		else if (isalnum ((unsigned char) c))
			mangled += c;
		else if (c == ' ' && i + 2 < len && full_name [i + 1] == '(' && full_name [i + 2] == ')')
			i += 2;
		else if (c == ',' && i + 1 < len && full_name [i + 1] == ' ')
			mangled += '_', i++;
		else if (c == '(' || c == ')' || c == '>')
			/* drop */;
		else
			mangled += '_';
	}
	g_free (full_name);

	/* Disambiguate the rare case where two distinct methods mangle the same
	 * (e.g. two generic instantiations whose type-argument names happen to
	 * stringify identically). */
	std::string name = mangled;
	for (int suffix = 0; taken->count (name) != 0; ++suffix)
		name = mangled + "_" + std::to_string (suffix);

	auto ins = names->emplace (method, std::move (name));
	(*taken) [ins.first->second] = method;
	return ins.first->second.c_str ();
}

/*
 * mono_llvm_method_from_symbol:
 *
 *   Inverse of mono_llvm_method_symbol (): return the MonoMethod a direct-call
 * symbol names, or NULL if NAME was never handed out as a method symbol (an
 * icall/intrinsic/other declaration, or a not-yet-seen method). The tier-1
 * inliner uses this to resolve a `call @name` back to its managed callee.
 */
MonoMethod *
mono_llvm_method_from_symbol (const char *name)
{
	if (!name)
		return NULL;

	std::lock_guard<std::mutex> lock (sym_mutex);
	if (!sym_taken)
		return NULL;
	auto found = sym_taken->find (std::string (name));
	if (found == sym_taken->end ())
		return NULL;
	return found->second;
}

void
EmitContext::emit_div_check (llvm::IRBuilder<> *builder, MonoBasicBlock *bb, MonoInst *ins, llvm::Value *lhs, llvm::Value *rhs)
{
	bool need_div_check = this->cfg->backend->need_div_check;

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
		emit_cond_system_exception (bb, "DivideByZeroException", cmp, FALSE);
		if (!this->ok ())
			break;
		builder = this->builder;

		/* b == -1 && a == 0x80000000 */
		if (is_signed) {
			llvm::Value *c = (lhs->getType () == llvm::Type::getInt32Ty (this->llvm_ctx ())) ? llvm::ConstantInt::get (lhs->getType (), 0x80000000, false) : llvm::ConstantInt::get (lhs->getType (), 0x8000000000000000LL, false);
			llvm::Value *cond1 = builder->CreateICmp (to_llvm_pred (LLVMIntEQ), rhs, llvm::ConstantInt::get (rhs->getType (), -1, false), "");
			llvm::Value *cond2 = builder->CreateICmp (to_llvm_pred (LLVMIntEQ), lhs, c, "");

			cmp = llvm::wrap (builder->CreateICmp (to_llvm_pred (LLVMIntEQ), builder->CreateAnd (cond1, cond2, ""), llvm::ConstantInt::get (llvm::Type::getInt1Ty (this->llvm_ctx ()), 1, false), ""));
			emit_cond_system_exception (bb, "OverflowException", cmp, FALSE);
			if (!this->ok ())
				break;
			builder = this->builder;
		}
		break;
	}
	default:
		break;
	}
}

/*
 * Record the frame home of SLOT (an alloca) with an llvm.experimental.stackmap
 * intrinsic carrying ID, so that after code emission the backend can read the
 * slot's home register+offset - and the stackmap's own PC - back out of the
 * `.llvm_stackmaps` section (the recovery passes in translator.cpp).
 *
 * Stock LLVM has no other way to publish where an alloca ended up, and LLVM
 * lowers an alloca operand to a Direct location {DwarfReg, Offset} whose value
 * is the slot's ADDRESS - exactly what a stack walk over a live frame needs.
 *
 * The intrinsic is declared on demand in the method's own module (every method
 * gets its own, so declaring by name is race-free), variadic void(i64,i32,...).
 */
static llvm::CallInst *
emit_slot_stackmap (EmitContext *ctx, llvm::IRBuilder<> *builder, LLVMValueRef slot, guint64 id)
{
	LLVMTypeRef params [] = { llvm::wrap (llvm::Type::getInt64Ty (ctx->llvm_ctx ())), llvm::wrap (llvm::Type::getInt32Ty (ctx->llvm_ctx ())) };
	LLVMTypeRef sm_type = LLVMFunctionType (llvm::wrap (llvm::Type::getVoidTy (ctx->llvm_ctx ())), params, 2, TRUE);
	LLVMValueRef sm = LLVMGetNamedFunction (ctx->lmodule, "llvm.experimental.stackmap");

	if (!sm)
		sm = LLVMAddFunction (ctx->lmodule, "llvm.experimental.stackmap", sm_type);

	LLVMValueRef args [] = {
		llvm::wrap (llvm::ConstantInt::get (llvm::Type::getInt64Ty (ctx->llvm_ctx ()), id, false)),
		llvm::wrap (llvm::ConstantInt::get (llvm::Type::getInt32Ty (ctx->llvm_ctx ()), 0, false)),
		slot,
	};
	/* A marker with no slot records only its own ID and PC. */
	llvm::CallInst *call = builder->CreateCall (llvm::cast<llvm::FunctionType> (llvm::unwrap (sm_type)), llvm::unwrap (sm), gep_index_list (args, slot ? 3 : 2), "");

	/*
	 * llvm.experimental.stackmap is declared Throws (Intrinsics.td), so left alone
	 * a marker planted inside a protected region reads as a call that can unwind
	 * out of it - which changes the EH geometry the gather pass then describes.
	 * A stackmap emits no code at all, so say what is actually true.
	 */
	call->setDoesNotThrow ();
	return call;
}

/*
 * Publish the home of the slot holding this/mrgctx as cfg->llvm_this_reg /
 * llvm_this_offset (mini.c:2573-2577), so a stack walk over a live frame of a
 * gshared method can reconstruct its generic context. Without it those methods
 * reach mini.c's g_assert (cfg->llvm_this_reg != -1) with the field unset.
 */
void
EmitContext::emit_this_slot_stackmap (llvm::IRBuilder<> *builder, LLVMValueRef slot)
{
	emit_slot_stackmap (this, builder, slot, MONO_LLVM_THIS_SLOT_STACKMAP_ID);
}

/*
 * Open CLAUSE_INDEX's handler body, over SLOT - that clause's thread-abort exvar.
 *
 * This and emit_finally_end_stackmap () are what the runtime's abort guard is
 * built from. They are the ONLY thing about the body that survives codegen: an
 * instruction moves, is cloned and is merged along with the code around it,
 * whereas a block loses its identity to the first merge that touches it.
 * MonoFinallyRangePass (passes/finally-range.cpp) walks between the two to recover which PCs
 * are body.
 *
 * SLOT must be the same exvar the shared IR checks after the finally returns
 * (method-to-ir.c, the OP_ENDFINALLY abort check) - install_handler_block_guard ()
 * writes *(bp + exvar_offset) = 1 and that check is what reads it - or the
 * guard's write would never be seen.
 */
void
EmitContext::emit_finally_guard_stackmap (llvm::IRBuilder<> *builder, LLVMValueRef slot, int clause_index)
{
	guint64 id = MONO_LLVM_FINALLY_STACKMAP_ID_BASE | (guint64) (guint32) this->clause_id (clause_index);

	emit_slot_stackmap (this, builder, slot, id);
}

/*
 * Close CLAUSE_INDEX's handler body: from here on control is leaving the finally,
 * so a thread stopped past this point is no longer inside it and the abort needs
 * no deferring. Carries no slot - the opening marker already named the exvar.
 */
void
EmitContext::emit_finally_end_stackmap (llvm::IRBuilder<> *builder, int clause_index)
{
	guint64 id = MONO_LLVM_FINALLY_END_STACKMAP_ID_BASE | (guint64) (guint32) this->clause_id (clause_index);

	emit_slot_stackmap (this, builder, nullptr, id);
}

/*
 * Mark the point OP_IL_SEQ_POINT sat at, so translator.cpp's recover_il_seq_points ()
 * can read back a native_offset -> il_offset mapping for this method after codegen (used
 * for stack traces and profiler attribution; the debugger's real seq points, OP_SEQ_POINT,
 * are a separate opcode this backend does not translate).
 *
 * Unlike the other markers this one needs a location operand: parse_stackmap_records ()
 * skips zero-location records, since the this-slot/finally-guard readers only ever want the
 * PC of a marker that also names a frame slot. This marker has no slot of its own to name,
 * so it carries a throwaway constant purely to give its record one - the value is never
 * read back, only the record's ID and PC are.
 */
void
EmitContext::emit_il_seq_point_stackmap (llvm::IRBuilder<> *builder, guint32 il_offset)
{
	guint64 id = MONO_LLVM_IL_SEQ_POINT_STACKMAP_ID_BASE | (guint64) il_offset;
	LLVMValueRef marker = llvm::wrap (llvm::ConstantInt::get (llvm::Type::getInt32Ty (this->llvm_ctx ()), 0));

	emit_slot_stackmap (this, builder, marker, id);
}

/*
 * emit_entry_bb:
 *
 *   Emit code to load/convert arguments.
 */
void
EmitContext::emit_entry_bb (llvm::IRBuilder<> *builder)
{
	int i, pindex;
	MonoCompile *cfg = this->cfg;
	MonoMethodSignature *sig = this->sig;
	LLVMCallInfo *linfo = this->linfo;
	MonoBasicBlock *bb;
	char **names;

	llvm::IRBuilder<> *old_builder = this->builder;
	this->builder = builder;

	this->alloca_builder = this->create_builder ();

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
		if (this->has_safepoints && (MONO_TYPE_IS_REFERENCE (var->inst_vtype) || var->inst_vtype->type == MONO_TYPE_I) && var != this->cfg->rgctx_var)
			var->flags |= MONO_INST_INDIRECT;
#endif

		if (var->flags & (MONO_INST_VOLATILE|MONO_INST_INDIRECT) || (mini_type_is_vtype (var->inst_vtype) && !MONO_CLASS_IS_SIMD (this->cfg, var->klass))) {
			vtype = this->type_to_llvm_type (var->inst_vtype);
			if (!this->ok ())
				return;
			/* Could be already created by an OP_VPHI */
			if (!this->addresses [var->dreg]) {
				if (var->flags & MONO_INST_LMF) {
					LLVMTypeRef lmf_type = LLVMArrayType (llvm::wrap (llvm::Type::getInt8Ty (this->llvm_ctx ())), MONO_ABI_SIZEOF (MonoLMF));
					this->addresses [var->dreg] = this->create_address (this->build_alloca_llvm_type_name (lmf_type, sizeof (target_mgreg_t), "entry_lmf"), lmf_type);
				} else {
					this->addresses [var->dreg] = this->create_address (this->build_named_alloca (var->inst_vtype, "entry"), vtype);
				}
				//LLVMSetValueName (this->addresses [var->dreg], g_strdup_printf ("vreg_loc_%d", var->dreg));
			}
			this->vreg_cli_types [var->dreg] = var->inst_vtype;
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
				args [0] = LLVMGetParam (this->lmethod, pindex);
				if (ainfo->pair_storage [1] != LLVMArgNone)
					args [1] = LLVMGetParam (this->lmethod, pindex + 1);
			} else {
				g_assert (ainfo->nslots <= 8);
				for (j = 0; j < ainfo->nslots; ++j)
					args [j] = LLVMGetParam (this->lmethod, pindex + j);
			}
			this->addresses [reg] = this->build_alloca_address (ainfo->type);

			emit_args_to_vtype (builder, ainfo->type, llvm::wrap (this->addresses [reg]->value), ainfo, args);
			break;
		}
		case LLVMArgVtypeByVal: {
			/* Element type must match the declaration in sig_to_llvm_sig_full () */
			this->addresses [reg] = this->create_address (LLVMGetParam (this->lmethod, pindex), this->type_to_llvm_arg_type (ainfo->type));
			break;
		}
		case LLVMArgVtypeAddr:
		case LLVMArgVtypeByRef: {
			/* The argument is passed by ref */
			/* Element type must match the declaration in sig_to_llvm_sig_full () */
			this->addresses [reg] = this->create_address (LLVMGetParam (this->lmethod, pindex), this->type_to_llvm_arg_type (ainfo->type));
			break;
		}
		case LLVMArgAsIArgs: {
			LLVMValueRef arg = LLVMGetParam (this->lmethod, pindex);
			int size;
			MonoType *t = mini_get_underlying_type (ainfo->type);

			/* The argument is received as an array of ints, store it into the real argument */
			this->addresses [reg] = this->build_alloca_address (t);

			size = mono_class_value_size (mono_class_from_mono_type_internal (t), NULL);
			if (size == 0) {
			} else if (size < TARGET_SIZEOF_VOID_P) {
				/* The upper bits of the registers might not be valid */
				llvm::Value *val = builder->CreateExtractValue (llvm::unwrap (arg), {0}, "");
				llvm::Value *dest = this->convert (this->addresses [reg]->value, llvm::PointerType::get (this->llvm_ctx (), 0));
				llvm::wrap (this->builder->CreateStore (builder->CreateTrunc (val, llvm::Type::getIntNTy (this->llvm_ctx (), size * 8), ""), dest));
			} else {
				llvm::wrap (this->builder->CreateStore (llvm::unwrap (arg), this->convert (this->addresses [reg]->value, llvm::PointerType::get (this->llvm_ctx (), 0))));
			}
			break;
		}
		case LLVMArgVtypeAsScalar:
			g_assert_not_reached ();
			break;
		case LLVMArgGsharedvtFixed: {
			/* These are non-gsharedvt arguments passed by ref, the rest of the IR treats them as scalars */
			LLVMValueRef arg = LLVMGetParam (this->lmethod, pindex);

			if (names [i])
				name = g_strdup_printf ("arg_%s", names [i]);
			else
				name = g_strdup_printf ("arg_%d", i);

			this->values [reg] = builder->CreateLoad (llvm::unwrap (this->type_to_llvm_type (ainfo->type)), this->convert (llvm::unwrap (arg), llvm::PointerType::get (this->llvm_ctx (), 0)), name);
			break;
		}
		case LLVMArgGsharedvtFixedVtype: {
			LLVMValueRef arg = LLVMGetParam (this->lmethod, pindex);

			if (names [i])
				name = g_strdup_printf ("vtype_arg_%s", names [i]);
			else
				name = g_strdup_printf ("vtype_arg_%d", i);

			/* Non-gsharedvt vtype argument passed by ref, the rest of the IR treats it as a vtype */
			g_assert (this->addresses [reg]);
			LLVMSetValueName (llvm::wrap (this->addresses [reg]->value), name);
			llvm::wrap (builder->CreateStore (builder->CreateLoad (llvm::unwrap (this->type_to_llvm_type (ainfo->type)), this->convert (llvm::unwrap (arg), llvm::PointerType::get (this->llvm_ctx (), 0)), ""), this->addresses [reg]->value));
			break;
		}
		case LLVMArgGsharedvtVariable:
			/* The IR treats these as variables with addresses */
			/* Element type must match the declaration in sig_to_llvm_sig_full () */
			if (!this->addresses [reg])
				this->addresses [reg] = this->create_address (LLVMGetParam (this->lmethod, pindex), IntPtrType ());
			break;
		default: {
			LLVMTypeRef t;
			/* Needed to avoid phi argument mismatch errors since operations on pointers produce i32/i64 */
			if (ainfo->type->byref)
				t = IntPtrType ();
			else
				t = this->type_to_llvm_type (ainfo->type);
			this->values [reg] = this->convert_full (this->values [reg], llvm::unwrap (llvm_type_to_stack_type (cfg, t)), this->type_is_unsigned (ainfo->type));
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
			if (MONO_CLASS_IS_SIMD (this->cfg, mono_class_from_mono_type_internal (ainfo->type)))
				/* Treat these as normal values */
				this->values [reg] = builder->CreateLoad (this->addresses [reg]->type, this->addresses [reg]->value, "simd_vtype");
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
		if (this->vreg_types [reg])
			this->values [reg] = this->convert (this->values [reg], this->vreg_types [reg]);
	}

	if (cfg->vret_addr)
		emit_volatile_store (cfg->vret_addr->dreg);
	if (sig->hasthis)
		emit_volatile_store (cfg->args [0]->dreg);
	for (i = 0; i < sig->param_count; ++i)
		if (!mini_type_is_vtype (sig->params [i]))
			emit_volatile_store (cfg->args [i + sig->hasthis]->dreg);

	if (sig->hasthis && !cfg->rgctx_var && cfg->gshared) {
		LLVMValueRef this_alloc;

		/*
		 * The exception handling code needs the location where the this argument was
		 * stored for gshared methods. We create a separate alloca to hold it, and mark it
		 * with the "mono.this" custom metadata to tell llvm that it needs to save its
		 * location into the LSDA.
		 */
		this_alloc = mono_llvm_build_alloca (llvm::wrap (builder), ThisType (), llvm::wrap (llvm::ConstantInt::get (llvm::Type::getInt32Ty (this->llvm_ctx ()), 1, false)), 0, "");
		/* This volatile store will keep the alloca alive */
		mono_llvm_build_store (llvm::wrap (builder), llvm::wrap (this->values [cfg->args [0]->dreg]), this_alloc, TRUE, LLVM_BARRIER_NONE);

		set_metadata_flag (this_alloc, "mono.this");

		/*
		 * Stock LLVM 18 ignores "mono.this"; record the slot's home location via a
		 * stackmap so the backend can recover cfg->llvm_this_reg/offset (#15, S6.1).
		 */
		emit_this_slot_stackmap (builder, this_alloc);
	}

	if (cfg->rgctx_var) {
		if (!(cfg->rgctx_var->flags & MONO_INST_VOLATILE)) {
			/* FIXME: This could be volatile even in llvmonly mode if used inside a clause etc. */
			g_assert (!this->addresses [cfg->rgctx_var->dreg]);
			this->values [cfg->rgctx_var->dreg] = this->rgctx_arg;

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
				LLVMValueRef rgctx_slot = mono_llvm_build_alloca (llvm::wrap (builder), IntPtrType (), llvm::wrap (llvm::ConstantInt::get (llvm::Type::getInt32Ty (this->llvm_ctx ()), 1, false)), 0, "");
				/* Volatile store keeps the slot alive. */
				mono_llvm_build_store (llvm::wrap (builder), llvm::wrap (this->convert (this->rgctx_arg, llvm::unwrap (IntPtrType ()))), rgctx_slot, TRUE, LLVM_BARRIER_NONE);
				set_metadata_flag (rgctx_slot, "mono.this");
				emit_this_slot_stackmap (builder, rgctx_slot);
			}
		} else {
			LLVMValueRef rgctx_alloc, store;

			/*
			 * We handle the rgctx arg similarly to the this pointer.
			 */
			g_assert (this->addresses [cfg->rgctx_var->dreg]);
			rgctx_alloc = llvm::wrap (this->addresses [cfg->rgctx_var->dreg]->value);
			/* This volatile store will keep the alloca alive */
			store = mono_llvm_build_store (llvm::wrap (builder), llvm::wrap (this->convert (this->rgctx_arg, llvm::unwrap (IntPtrType ()))), rgctx_alloc, TRUE, LLVM_BARRIER_NONE);

			set_metadata_flag (rgctx_alloc, "mono.this");

			/*
			 * MRGCTX gshared (#15, S6.2): rgctx_alloc already holds the rgctx via
			 * the volatile store above; record its home slot so the backend can
			 * recover cfg->llvm_this_reg/offset for stack-walk generic-context
			 * reconstruction.
			 */
			if (cfg->gshared)
				emit_this_slot_stackmap (builder, rgctx_alloc);
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
			val = builder->CreateAlloca (llvm::Type::getInt32Ty (this->llvm_ctx ()), nullptr, name);
			builder->CreateStore (llvm::ConstantInt::get (llvm::Type::getInt32Ty (this->llvm_ctx ()), 0, false), val);

			this->bblocks [bb->block_num].finally_ind = val;
		} else {
			/* Create a variable to hold the exception var */
			if (!this->ex_var)
				this->ex_var = builder->CreateAlloca (llvm::unwrap (ObjRefType ()), nullptr, "exvar");
		}
	}
	this->builder = old_builder;
}

/*
 * emit_class_init_guards:
 *
 *   Emit the class-init preamble: a guarded call to the cctor trigger for this
 * method's declaring class, if that cctor still owes a run.
 *
 * Normally the trigger is the managed call that reaches the method - the first
 * one lands in a jit trampoline, which compiles the method and then runs the
 * declaring class's cctor. A tier-1 body that the inliner folds into a caller has
 * no such call left, so it carries the trigger itself:
 *
 *   if (!vtable->initialized)
 *           mono_generic_class_init (vtable);
 *
 * the same sequence the front-end emits for an in-body class-init barrier,
 * hoisted to the prologue.
 *
 * It is tempting to follow that with llvm.invariant.start / llvm.assume on the
 * `initialized` byte, so that the barriers the front-end left further down for
 * the same class fold away. That would be unsound. When the calling thread is
 * the one already running that cctor, mono_runtime_class_init_full () returns
 * success with the byte still 0 (object.c, the initializing_tid checks) - and
 * reaching a method of C from C's own cctor is just what a static field
 * initializer calling a helper looks like. So an assume of `initialized == 1`
 * would be plainly false there, and an invariant.start would be claiming a byte
 * immutable that the cctor further up our own stack is about to write.
 */
void
EmitContext::emit_class_init_guards (llvm::IRBuilder<> *builder)
{
	MonoCompile *cfg = this->cfg;

	/*
	 * AOT would want a GOT slot rather than a baked vtable pointer, shared
	 * generic code has no single vtable to bake at all, and a wrapper's cctor
	 * relationship to its declaring class is the runtime's business (the
	 * trampoline skips it outright for the remoting ones).
	 */
	if (cfg->compile_aot || cfg->gshared || cfg->gsharedvt)
		return;
	if (cfg->method->wrapper_type != MONO_WRAPPER_NONE)
		return;

	bool indeterminate = false;
	MonoVTable *vtable = mono::pending_class_init_vtable (cfg, &indeterminate);

	if (indeterminate && this->translate_only) {
		/*
		 * A body destined to be inlined has to carry the trigger for the cctor it
		 * owes, so not being able to name the vtable is a refusal:
		 * materialize_callee () hands back nothing and the inliner leaves the call
		 * in place, which initializes the class the usual way. A root needs no such
		 * refusal - it keeps its own managed entry point either way.
		 */
		this->set_failure ("indeterminate pending class init");
		return;
	}

	if (!vtable)
		return;

	llvm::IRBuilder<> *old_builder = this->builder;
	this->builder = builder;

	llvm::Type *byte_type = llvm::Type::getInt8Ty (this->llvm_ctx ());
	llvm::Type *ptr_type = llvm::PointerType::get (this->llvm_ctx (), 0);
	llvm::Type *intptr_type = llvm::unwrap (IntPtrType ());

	/*
	 * Close the entry block with an unconditional branch and emit the check in
	 * fresh blocks after it, rather than growing the entry block itself. The entry
	 * block is special in more ways than one - it is where every alloca has to live
	 * for SROA to see it, and build_alloca_llvm_type_name () keeps inserting into
	 * it for the rest of the method - so leaving it as nothing but prologue, and
	 * terminated once, keeps the two concerns apart.
	 */
	LLVMBasicBlockRef check_bb = this->gen_bb ("CLASS_INIT_CHECK_BB");
	llvm::Instruction *entry_br = builder->CreateBr (llvm::unwrap (check_bb));
	/*
	 * Those allocas now have to land before that branch, and a null last_alloca
	 * means "at the end of the entry block" - which from here would be past it,
	 * leaving the block with no terminator as far as LLVM is concerned.
	 */
	this->last_alloca = entry_br;
	builder->SetInsertPoint (llvm::unwrap (check_bb));

	llvm::Value *vtable_val = llvm::ConstantInt::get (intptr_type, (guint64) (gsize) vtable, false);
	/* Fold the field offset into the constant rather than emitting a GEP. */
	llvm::Value *flag_addr = builder->CreateIntToPtr (
		llvm::ConstantInt::get (intptr_type,
		                        (guint64) ((gsize) vtable + MONO_STRUCT_OFFSET (MonoVTable, initialized)),
		                        false),
		ptr_type, "class_init_flag");

	llvm::Value *inited = builder->CreateLoad (byte_type, flag_addr, "");
	LLVMValueRef cmp = llvm::wrap (builder->CreateICmpNE (inited, llvm::ConstantInt::get (byte_type, 0)));

	LLVMValueRef expect_args [2];
	expect_args [0] = cmp;
	expect_args [1] = llvm::wrap (llvm::ConstantInt::get (llvm::Type::getInt1Ty (this->llvm_ctx ()), 1, false));
	cmp = this->call_intrins (INTRINS_EXPECT_I1, expect_args, "");

	LLVMBasicBlockRef init_bb = this->gen_bb ("CLASS_INIT_BB");
	LLVMBasicBlockRef cont_bb = this->gen_bb ("CLASS_INIT_CONT_BB");
	LLVMValueRef br = mono_llvm_build_weighted_branch (llvm::wrap (builder), cmp, cont_bb, init_bb, 1000, 1);
	tag_class_init (br, "mono.class-init-check", vtable->klass);

	// if not initialized then we need to make a call to mono_generic_class_init
	builder->SetInsertPoint (llvm::unwrap (init_bb));
	MonoJitICallInfo *info = mono_find_jit_icall_info (MONO_JIT_ICALL_mono_generic_class_init);
	LLVMTypeRef icall_sig = this->sig_to_llvm_sig (info->sig);
	LLVMValueRef callee = this->get_jit_callee ("", icall_sig, MONO_PATCH_INFO_JIT_ICALL_ID,
	                                           GUINT_TO_POINTER (MONO_JIT_ICALL_mono_generic_class_init));
	/*
	 * A mismatch would mean the front-end's own barrier declared this icall with a
	 * different type, which would make the call below malformed.
	 */
	g_assert (LLVMGlobalGetValueType (callee) == icall_sig);
	llvm::CallInst *init_call = builder->CreateCall (llvm::cast<llvm::FunctionType> (llvm::unwrap (icall_sig)),
	                                                llvm::unwrap (callee), { vtable_val });
	tag_class_init (llvm::wrap (init_call), "mono.class-init", vtable->klass);
	builder->CreateBr (llvm::unwrap (cont_bb));

	/*
	 * The rest of bb_entry, and the branch out of it, belong in the join block -
	 * process_bb () picks the entry bb up from here (get_end_bb ()), and so do the
	 * phi incoming edges.
	 */
	builder->SetInsertPoint (llvm::unwrap (cont_bb));
	this->bblocks [cfg->bb_entry->block_num].end_bblock = cont_bb;
	this->builder = old_builder;
}

bool
EmitContext::is_supported_callconv (MonoCallInst *call)
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
EmitContext::process_call (MonoBasicBlock *bb, llvm::IRBuilder<> **builder_ref, MonoInst *ins)
{
	MonoCompile *cfg = this->cfg;
	llvm::Value **values = this->values;
	Address **addresses = this->addresses;
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

	if (!this->is_supported_callconv (call)) {
		this->set_failure ("non-default callconv");
		return;
	}

	cinfo = call->cinfo;
	g_assert (cinfo);
	if (call->rgctx_arg_reg)
		cinfo->rgctx_arg = TRUE;
	if (call->imt_arg_reg)
		cinfo->imt_arg = TRUE;
	vretaddr = (cinfo->ret.storage == LLVMArgVtypeRetAddr || cinfo->ret.storage == LLVMArgVtypeByRef || cinfo->ret.storage == LLVMArgGsharedvtFixed || cinfo->ret.storage == LLVMArgGsharedvtVariable || cinfo->ret.storage == LLVMArgGsharedvtFixedVtype);

	llvm_sig = this->sig_to_llvm_sig_full (sig, cinfo);
	if (!this->ok ())
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
				callee = this->lmethod;
			} else {
				ERROR_DECL (error);

				/*
				 * Call the callee's stable specific-trampoline entry directly, by
				 * name, instead of loading it out of a module-local global baked
				 * with its address. The trampoline forwards to whatever is
				 * currently compiled for the method - lazily compiling it on the
				 * first call - and its address never changes for the method's
				 * life, so an ordinary `call @symbol` always reaches the right
				 * place. Redirecting a caller once the callee is promoted to tier 1
				 * is the entry sled's job (at the callee's own entry), not this
				 * call site's - there is nothing here left to repoint.
				 */
				target = mono_create_jit_trampoline (mono_domain_get (), call->method, error);
				if (!is_ok (error)) {
					this->set_failure (mono_error_get_message (error));
					mono_error_cleanup (error);
					return;
				}

				callee = this->get_direct_callee (mono_llvm_method_symbol (call->method), llvm_sig, target);
			}
		}

		if (call->method && strstr (m_class_get_name (call->method->klass), "AsyncVoidMethodBuilder")) {
			/* LLVM miscompiles async methods */
			this->set_failure ("#13734");
			return;
		}
	} else if (calli) {
	} else {
		const MonoJitICallId jit_icall_id = call->jit_icall_id;

		if (jit_icall_id) {
			callee = this->get_jit_callee ("", llvm_sig, MONO_PATCH_INFO_JIT_ICALL_ID, GUINT_TO_POINTER (jit_icall_id));
		} else {
			{
				if (cfg->abs_patches) {
					MonoJumpInfo *abs_ji = static_cast<MonoJumpInfo*>(g_hash_table_lookup (cfg->abs_patches, call->fptr));
					if (abs_ji) {
						ERROR_DECL (error);

						target = mono_resolve_patch_target (cfg->method, cfg->domain, NULL, abs_ji, FALSE, error);
						mono_error_assert_ok (error);
						callee = this->get_jit_callee ("", llvm_sig, abs_ji->type, abs_ji->data.target);
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
		index = llvm::wrap (llvm::ConstantInt::get (llvm::Type::getInt32Ty (this->llvm_ctx ()), ins->inst_offset / size, false));

		callee = llvm::wrap (this->convert (builder->CreateLoad (llvm::PointerType::get (this->llvm_ctx (), 0), builder->CreateGEP (llvm::PointerType::get (this->llvm_ctx (), 0), this->convert (values [ins->inst_basereg], llvm::PointerType::get (this->llvm_ctx (), 0)), gep_index_list (&index, 1), ""), ""), llvm::PointerType::get (this->llvm_ctx (), 0)));
	} else if (calli) {
		callee = llvm::wrap (this->convert (values [ins->sreg1], llvm::PointerType::get (this->llvm_ctx (), 0)));
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

	/*
	 * Grab the trigger's vtable argument while the arg list is still untouched -
	 * the loop below consumes L, and the conversion it does buries the constant
	 * under an inttoptr. call->args would be the obvious place to read it from,
	 * but it points at the emitter's stack frame and is long gone by now.
	 */
	llvm::Value *class_init_vtable = nullptr;
	if (call->jit_icall_id == MONO_JIT_ICALL_mono_generic_class_init && l)
		class_init_vtable = values [static_cast<guint32>(reinterpret_cast<gssize>(l->data)) & 0xffffff];

	if (call->rgctx_arg_reg) {
		g_assert (values [call->rgctx_arg_reg]);
		g_assert (cinfo->rgctx_arg_pindex < nargs);
		/*
		 * On ARM, the imt/rgctx argument is passed in a caller save register, but some of our trampolines etc. clobber it, leading to
		 * problems is LLVM moves the arg assignment earlier. To work around this, save the argument into a stack slot and load
		 * it using a volatile load.
		 */
#ifdef TARGET_ARM
		if (!this->imt_rgctx_loc)
			this->imt_rgctx_loc = llvm::unwrap (this->build_alloca_llvm_type (this->module->ptr_type, TARGET_SIZEOF_VOID_P));
		builder->CreateStore (this->convert (this->values [call->rgctx_arg_reg], llvm::unwrap (this->module->ptr_type)), this->imt_rgctx_loc);
		args [cinfo->rgctx_arg_pindex] = mono_llvm_build_load (llvm::wrap (builder), this->module->ptr_type, llvm::wrap (this->imt_rgctx_loc), "", TRUE);
#else
		args [cinfo->rgctx_arg_pindex] = llvm::wrap (this->convert (values [call->rgctx_arg_reg], llvm::unwrap (this->module->ptr_type)));
#endif
	}
	if (call->imt_arg_reg) {
		g_assert (values [call->imt_arg_reg]);
		g_assert (cinfo->imt_arg_pindex < nargs);
#ifdef TARGET_ARM
		if (!this->imt_rgctx_loc)
			this->imt_rgctx_loc = llvm::unwrap (this->build_alloca_llvm_type (this->module->ptr_type, TARGET_SIZEOF_VOID_P));
		builder->CreateStore (this->convert (this->values [call->imt_arg_reg], llvm::unwrap (this->module->ptr_type)), this->imt_rgctx_loc);
		args [cinfo->imt_arg_pindex] = mono_llvm_build_load (llvm::wrap (builder), this->module->ptr_type, llvm::wrap (this->imt_rgctx_loc), "", TRUE);
#else
		args [cinfo->imt_arg_pindex] = llvm::wrap (this->convert (values [call->imt_arg_reg], llvm::unwrap (this->module->ptr_type)));
#endif
	}
	switch (cinfo->ret.storage) {
	case LLVMArgGsharedvtVariable: {
		MonoInst *var = get_vreg_to_inst (cfg, call->inst.dreg);

		if (var && var->opcode == OP_GSHAREDVT_LOCAL) {
			args [cinfo->vret_arg_pindex] = llvm::wrap (this->convert (llvm::unwrap (this->emit_gsharedvt_ldaddr (var->dreg)), llvm::unwrap (IntPtrType ())));
		} else {
			g_assert (addresses [call->inst.dreg]);
			args [cinfo->vret_arg_pindex] = llvm::wrap (this->convert (addresses [call->inst.dreg]->value, llvm::unwrap (IntPtrType ())));
		}
		break;
	}
	default:
		if (vretaddr) {
			if (!addresses [call->inst.dreg])
				addresses [call->inst.dreg] = this->build_alloca_address (sig->ret);
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
				args [pindex + j] = llvm::wrap (llvm::Constant::getNullValue (llvm::Type::getDoubleTy (this->llvm_ctx ())));
			pindex += ainfo->ndummy_fpargs;

			g_assert (addresses [reg]);
			this->emit_vtype_to_args (builder, ainfo->type, llvm::wrap (addresses [reg]->value), ainfo, args + pindex, &nargs);
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
			args [pindex] = llvm::wrap (this->convert (addresses [reg]->value, llvm::PointerType::get (this->llvm_ctx (), 0)));
			break;
		}
		case LLVMArgAsIArgs:
			g_assert (addresses [reg]);
			if (ainfo->esize == 8)
				args [pindex] = llvm::wrap (this->builder->CreateLoad (llvm::unwrap (LLVMArrayType (llvm::wrap (llvm::Type::getInt64Ty (this->llvm_ctx ())), ainfo->nslots)), this->convert (addresses [reg]->value, llvm::PointerType::get (this->llvm_ctx (), 0)), ""));
			else
				args [pindex] = llvm::wrap (this->builder->CreateLoad (llvm::unwrap (LLVMArrayType (IntPtrType (), ainfo->nslots)), this->convert (addresses [reg]->value, llvm::PointerType::get (this->llvm_ctx (), 0)), ""));
			break;
		case LLVMArgVtypeAsScalar:
			g_assert_not_reached ();
			break;
		case LLVMArgGsharedvtFixed:
		case LLVMArgGsharedvtFixedVtype:
			g_assert (addresses [reg]);
			args [pindex] = llvm::wrap (this->convert (addresses [reg]->value, llvm::PointerType::get (this->llvm_ctx (), 0)));
			break;
		case LLVMArgGsharedvtVariable:
			g_assert (addresses [reg]);
			args [pindex] = llvm::wrap (this->convert (addresses [reg]->value, llvm::PointerType::get (this->llvm_ctx (), 0)));
			break;
		default:
			g_assert (args [pindex]);
			if (i == 0 && sig->hasthis)
				args [pindex] = llvm::wrap (this->convert (llvm::unwrap (args [pindex]), llvm::unwrap (param_types [pindex])));
			else
				args [pindex] = llvm::wrap (this->convert (llvm::unwrap (args [pindex]), llvm::unwrap (this->type_to_llvm_arg_type (ainfo->type))));
			break;
		}
		g_assert (pindex <= nargs);

		l = l->next;
	}

	if (call->cinfo->dummy_arg) {
		g_assert (call->cinfo->dummy_arg_pindex < nargs);
		args [call->cinfo->dummy_arg_pindex] = llvm::wrap (llvm::Constant::getNullValue (llvm::unwrap (this->module->ptr_type)));
	}

	// FIXME: Align call sites

	/*
	 * Emit the call
	 */
	lcall = this->emit_call (bb, &builder, llvm_sig, callee, args, LLVMCountParamTypes (llvm_sig));

	// If we just allocated an object, it's not null.
	if (call->method && call->method->wrapper_type == MONO_WRAPPER_ALLOC) {
		mono_llvm_set_call_nonnull_ret (lcall);
	}

	/*
	 * Tag the GC write barrier so passes/wbarrier.hpp can find it. This is the
	 * out-of-line form mini_emit_write_barrier () falls back to when the GC
	 * exposes no card table to inline against - boehm, in practice. The whole
	 * wrapper is one call to mono_gc_wbarrier_generic_nostore_internal (the
	 * address to mark being its only argument), which is what the lowering
	 * replaces; matching the tag rather than the callee keeps the pass out of
	 * the business of knowing how a wrapper's entry point got materialized.
	 */
	if (call->method && call->method->wrapper_type == MONO_WRAPPER_WRITE_BARRIER) {
		auto &ctx = this->llvm_ctx ();
		llvm::unwrap<llvm::Instruction> (lcall)->setMetadata ("mono.wbarrier",
		                                                      llvm::MDNode::get (ctx, {}));
	}

	if (ins->opcode != OP_TAILCALL && ins->opcode != OP_TAILCALL_MEMBASE && LLVMGetInstructionOpcode (lcall) == LLVMCall)
		mono_llvm_set_call_notailcall (lcall);

	// Add original method name we are currently emitting as a custom string metadata (the only way to leave comments in LLVM IR)
	if (mono_debug_enabled () && call && call->method)
		mono_llvm_add_string_metadata (lcall, "managed_name", mono_method_full_name (call->method, TRUE));

	/*
	 * Tag the front-end's in-body class-init barrier. The vtable is a baked
	 * constant except in shared generic code, which loads it out of the rgctx -
	 * and then there is no single class to name, so the barrier goes untagged.
	 */
	if (auto *baked = llvm::dyn_cast_or_null<llvm::ConstantInt> (class_init_vtable)) {
		MonoClass *init_klass = ((MonoVTable *) (gsize) baked->getZExtValue ())->klass;

		tag_class_init (lcall, "mono.class-init", init_klass);

		/*
		 * emit_class_init () gives the trigger call a block of its own, entered
		 * only from the block that tested vtable->initialized, so the check is
		 * that block's terminator.
		 */
		llvm::BasicBlock *check_bb = llvm::unwrap<llvm::Instruction> (lcall)->getParent ()->getUniquePredecessor ();
		auto *check_br = check_bb ? llvm::dyn_cast<llvm::BranchInst> (check_bb->getTerminator ()) : nullptr;

		if (check_br && check_br->isConditional ())
			tag_class_init (llvm::wrap (check_br), "mono.class-init-check", init_klass);
	}

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
		mono_llvm_add_instr_attr_with_type (lcall, 1 + cinfo->vret_arg_pindex, LLVM_ATTR_STRUCT_RET, this->type_to_llvm_type (sig->ret));
	if (call->rgctx_arg_reg)
		mono_llvm_add_instr_attr (lcall, 1 + cinfo->rgctx_arg_pindex, LLVM_ATTR_NEST);
	if (call->imt_arg_reg)
		mono_llvm_add_instr_attr (lcall, 1 + cinfo->imt_arg_pindex, LLVM_ATTR_NEST);

	/* Add byval attributes if needed */
	for (i = 0; i < sig->param_count; ++i) {
		LLVMArgInfo *ainfo = &call->cinfo->args [i + sig->hasthis];

		if (ainfo && ainfo->storage == LLVMArgVtypeByVal)
			mono_llvm_add_instr_attr_with_type (lcall, 1 + ainfo->pindex, LLVM_ATTR_BY_VAL, this->type_to_llvm_arg_type (ainfo->type));
	}

	/*
	 * Convert the result
	 */
	switch (cinfo->ret.storage) {
	case LLVMArgVtypeInReg: {
		LLVMValueRef regs [2];

		if (LLVMTypeOf (lcall) == llvm::wrap (llvm::Type::getVoidTy (this->llvm_ctx ())))
			/* Empty struct */
			break;

		if (!addresses [ins->dreg])
			addresses [ins->dreg] = this->build_alloca_address (sig->ret);

		regs [0] = llvm::wrap (builder->CreateExtractValue (llvm::unwrap (lcall), {0}, ""));
		if (cinfo->ret.pair_storage [1] != LLVMArgNone)
			regs [1] = llvm::wrap (builder->CreateExtractValue (llvm::unwrap (lcall), {1}, ""));
		this->emit_args_to_vtype (builder, sig->ret, llvm::wrap (addresses [ins->dreg]->value), &cinfo->ret, regs);
		break;
	}
	case LLVMArgVtypeByVal:
		if (!addresses [call->inst.dreg])
			addresses [call->inst.dreg] = this->build_alloca_address (sig->ret);
		llvm::wrap (builder->CreateStore (llvm::unwrap (lcall), addresses [call->inst.dreg]->value));
		break;
	case LLVMArgAsIArgs:
	case LLVMArgFpStruct:
		if (!addresses [call->inst.dreg])
			addresses [call->inst.dreg] = this->build_alloca_address (sig->ret);
		llvm::wrap (builder->CreateStore (llvm::unwrap (lcall), this->convert_full (addresses [call->inst.dreg]->value, llvm::PointerType::get (this->llvm_ctx (), 0), FALSE)));
		break;
	case LLVMArgVtypeAsScalar:
		if (!addresses [call->inst.dreg])
			addresses [call->inst.dreg] = this->build_alloca_address (sig->ret);
		llvm::wrap (builder->CreateStore (llvm::unwrap (lcall), this->convert_full (addresses [call->inst.dreg]->value, llvm::PointerType::get (this->llvm_ctx (), 0), FALSE)));
		if (MONO_CLASS_IS_SIMD (this->cfg, mono_class_from_mono_type_internal (sig->ret)))
			values [ins->dreg] = builder->CreateBitCast (llvm::unwrap (lcall), llvm::unwrap (this->type_to_llvm_type (sig->ret)), "callret_cvt_simd");
		break;
	case LLVMArgVtypeRetAddr:
	case LLVMArgVtypeByRef:
		if (MONO_CLASS_IS_SIMD (this->cfg, mono_class_from_mono_type_internal (sig->ret))) {
			/* Some opcodes like STOREX_MEMBASE access these by value */
			g_assert (addresses [call->inst.dreg]);
			values [ins->dreg] = builder->CreateLoad (llvm::unwrap (this->type_to_llvm_type (sig->ret)), this->convert_full (addresses [call->inst.dreg]->value, llvm::PointerType::get (this->llvm_ctx (), 0), FALSE), "");
		}
		break;
	case LLVMArgGsharedvtVariable:
		break;
	case LLVMArgGsharedvtFixed:
	case LLVMArgGsharedvtFixedVtype:
		values [ins->dreg] = builder->CreateLoad (llvm::unwrap (this->type_to_llvm_type (sig->ret)), this->convert_full (addresses [call->inst.dreg]->value, llvm::PointerType::get (this->llvm_ctx (), 0), FALSE), "");
		break;
	default:
		if (sig->ret->type != MONO_TYPE_VOID)
			/* If the method returns an unsigned value, need to zext it */
			values [ins->dreg] = this->convert_full (llvm::unwrap (lcall), llvm::unwrap (llvm_type_to_stack_type (cfg, this->type_to_llvm_type (sig->ret))), this->type_is_unsigned (sig->ret));
		break;
	}

	*builder_ref = this->builder;
}

void
EmitContext::emit_throw (MonoBasicBlock *bb, gboolean rethrow, LLVMValueRef exc)
{
	MonoMethodSignature *throw_sig;

	LLVMValueRef * const pcallee = rethrow ? &this->module->rethrow : &this->module->throw_icall;
	LLVMValueRef callee = *pcallee;
	char const * const icall_name = rethrow ? "mono_arch_rethrow_exception" : "mono_arch_throw_exception";
#ifndef TARGET_X86
	const
#endif
	MonoJitICallId icall_id = rethrow ? MONO_JIT_ICALL_mono_arch_rethrow_exception  : MONO_JIT_ICALL_mono_arch_throw_exception;

	/*
	 * emit_call () needs the signature the callee was declared with -- never
	 * derive it from the callee value itself, whose type need not match this
	 * call site.
	 *
	 * The MonoMethodSignature is built once for the process, not once per
	 * compile: mono_metadata_signature_alloc () allocates out of corlib's image
	 * mempool, which is never freed, so building one per compile would grow
	 * that mempool for the life of the run. The LLVM function type derived from
	 * it, by contrast, has to be per compile - it is uniqued in this compile's
	 * context - so that one is cached on the module.
	 */
	static std::once_flag throw_sig_once;
	static MonoMethodSignature *cached_throw_sig;
	std::call_once (throw_sig_once, [] () {
		MonoMethodSignature *s = mono_metadata_signature_alloc (mono_get_corlib (), 1);
		s->ret = m_class_get_byval_arg (mono_get_void_class ());
		s->params [0] = m_class_get_byval_arg (mono_get_object_class ());
		cached_throw_sig = s;
	});
	throw_sig = cached_throw_sig;

	if (!this->module->throw_sig_type)
		this->module->throw_sig_type = this->sig_to_llvm_sig (throw_sig);
	LLVMTypeRef sig = this->module->throw_sig_type;

	if (!callee) {
		{
#ifdef TARGET_X86
			/*
			 * LLVM doesn't push the exception argument, so we need a different
			 * trampoline.
			 */
			icall_id =  rethrow ? MONO_JIT_ICALL_mono_llvm_rethrow_exception_trampoline : MONO_JIT_ICALL_mono_llvm_throw_exception_trampoline;
#endif
			callee = this->get_jit_callee (icall_name, sig, MONO_PATCH_INFO_JIT_ICALL_ID, GUINT_TO_POINTER (icall_id));
		}

		mono_memory_barrier ();
	}
	LLVMValueRef arg;
	arg = llvm::wrap (this->convert (llvm::unwrap (exc), llvm::unwrap (this->type_to_llvm_type (m_class_get_byval_arg (mono_get_object_class ())))));
	LLVMValueRef lcall = emit_call (bb, &this->builder, sig, callee, &arg, 1);

	/*
	 * Without this, two `throw new X (...)` statements whose only difference
	 * is a constant argument (a message string, an exception type) look
	 * identical to the optimizer once that argument is hoisted into each
	 * predecessor, and it tail-merges them into one shared call - see
	 * LLVM_ATTR_NO_MERGE's doc comment (translator-cpp.hpp).
	 */
	mono_llvm_add_instr_attr (lcall, LLVMAttributeFunctionIndex, LLVM_ATTR_NO_MERGE);
}

LLVMValueRef
EmitContext::create_const_vector (LLVMTypeRef t, const int *vals, int count)
{
	g_assert (count <= 16);
	LLVMValueRef llvm_vals [16];
	for (int i = 0; i < count; i++)
		llvm_vals [i] = llvm::wrap (llvm::ConstantInt::get (llvm::unwrap (t), vals [i], false));
	return const_vector (llvm_vals, count);
}

LLVMValueRef
EmitContext::create_const_vector_i32 (const int *mask, int count)
{
	return create_const_vector (llvm::wrap (llvm::Type::getInt32Ty (llvm_ctx ())), mask, count);
}

LLVMValueRef
EmitContext::create_const_vector_4_i32 (int v0, int v1, int v2, int v3)
{
	LLVMValueRef mask [4];
	mask [0] = llvm::wrap (llvm::ConstantInt::get (llvm::Type::getInt32Ty (llvm_ctx ()), v0, false));
	mask [1] = llvm::wrap (llvm::ConstantInt::get (llvm::Type::getInt32Ty (llvm_ctx ()), v1, false));
	mask [2] = llvm::wrap (llvm::ConstantInt::get (llvm::Type::getInt32Ty (llvm_ctx ()), v2, false));
	mask [3] = llvm::wrap (llvm::ConstantInt::get (llvm::Type::getInt32Ty (llvm_ctx ()), v3, false));
	return const_vector (mask, 4);
}

LLVMValueRef
EmitContext::create_const_vector_2_i32 (int v0, int v1)
{
	LLVMValueRef mask [2];
	mask [0] = llvm::wrap (llvm::ConstantInt::get (llvm::Type::getInt32Ty (llvm_ctx ()), v0, false));
	mask [1] = llvm::wrap (llvm::ConstantInt::get (llvm::Type::getInt32Ty (llvm_ctx ()), v1, false));
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
LLVMValueRef
EmitContext::get_mono_personality ()
{
	if (this->personality)
		return this->personality;

	/* Can't cache across methods as each method is in its own llvm module */
	LLVMTypeRef personality_type = LLVMFunctionType (llvm::wrap (llvm::Type::getInt32Ty (this->llvm_ctx ())), NULL, 0, TRUE);
	LLVMValueRef personality = LLVMAddFunction (this->lmodule, "mono_personality", personality_type);
	mono_llvm_add_func_attr (personality, LLVM_ATTR_NO_UNWIND);
	LLVMBasicBlockRef entry_bb = append_basic_block (personality, "ENTRY");
	llvm::IRBuilder<> builder2_storage (llvm_ctx ());
	llvm::IRBuilder<> *builder2 = &builder2_storage;
	builder2->SetInsertPoint (llvm::unwrap (entry_bb));
	llvm::wrap (builder2->CreateRet (llvm::ConstantInt::get (llvm::Type::getInt32Ty (this->llvm_ctx ()), 0, false)));

	LLVMSetPersonalityFn (this->lmethod, personality);

	this->personality = personality;
	return personality;
}

/*
 * emit_resume_unwind:
 *
 *   Emit the tail of a finally/fault handler's exceptional exit: a call to
 * llvm_resume_unwind_trampoline, handing the frame back to the runtime to carry on
 * looking for a handler. The trampoline never returns to its caller, so whatever
 * follows it is unreachable.
 *
 *   When another clause protects this one the unwind does NOT leave the frame: the
 * runtime finds that encloser in this same method and re-enters, with the
 * encloser's index in the selector, to run its handler. There is a real edge from
 * here to that handler, and the values this cleanup just wrote are live across it -
 * the enclosing handler goes on to read the locals the finally updated. With a
 * plain call the handler's block ends in `unreachable`, LLVM sees those stores as
 * dead, and SROA folds the enclosing handler's incoming values as though the
 * cleanup had never run.
 *
 *   So the trampoline becomes an invoke unwinding to a landing pad of this
 * cleanup's own - reached ONLY from here, and so carrying exactly the post-cleanup
 * state - whose selector switch routes each of this clause's enclosers to its
 * handler body. It cannot instead unwind to the encloser's own pad: that pad is
 * shared with the call sites in the encloser's try region, and entering it from
 * here would hand the encloser's body the state those call sites had, not ours.
 *
 *   build_ex_info () completes the loop: it recognises this pad by the
 * MONO_LSDA_KIND_RESUME_PAD marker on its landingpad clause and makes it the
 * handler_start of the entries it synthesises for this clause's enclosers, so the
 * block the runtime jumps to is the block this edge names.
 */
void
EmitContext::emit_resume_unwind (MonoBasicBlock *bb, llvm::IRBuilder<> **builder_ref)
{
	MonoCompile *cfg = this->cfg;
	llvm::IRBuilder<> *builder = *builder_ref;
	LLVMTypeRef icall_sig = LLVMFunctionType (llvm::wrap (llvm::Type::getVoidTy (this->llvm_ctx ())), NULL, 0, FALSE);
	llvm::FunctionType *fn_type = llvm::cast<llvm::FunctionType> (llvm::unwrap (icall_sig));
	LLVMValueRef callee = this->get_jit_callee ("llvm_resume_unwind_trampoline", icall_sig, MONO_PATCH_INFO_JIT_ICALL_ID, GUINT_TO_POINTER (MONO_JIT_ICALL_mono_llvm_resume_unwind_trampoline));
	llvm::Type *i32_ty = llvm::Type::getInt32Ty (this->llvm_ctx ());
	int clause_index = MONO_REGION_CLAUSE_INDEX (mono_get_block_region_notry (cfg, bb->region));
	MonoExceptionClause *self = &cfg->header->clauses [clause_index];
	std::vector<int> enclosers;
	int i;

	for (i = 0; i < cfg->header->num_clauses; ++i) {
		if (i == clause_index || !clause_encloses (self, &cfg->header->clauses [i]))
			continue;
		if (this->clause_to_handler.find (i) == this->clause_to_handler.end ())
			continue;
		enclosers.push_back (i);
	}

	if (enclosers.empty ()) {
		builder->CreateCall (fn_type, llvm::unwrap (callee), gep_index_list (NULL, 0), "");
		builder->CreateUnreachable ();
		*builder_ref = this->builder = builder;
		return;
	}

	LLVMBasicBlockRef pad_bb = this->gen_bb ("RESUME_PAD_BB");
	LLVMBasicBlockRef cont_bb = this->gen_bb ("RESUME_UNWIND_CONT_BB");
	llvm::IRBuilder<> *pad_builder, *cont_builder;
	LLVMValueRef landing_pad, type_info;
	LLVMTypeRef ti_members [2] = { llvm::wrap (i32_ty), llvm::wrap (i32_ty) };
	LLVMTypeRef ti_type = struct_type (llvm_ctx (), ti_members, 2, FALSE);
	LLVMValueRef ti_init [2];
	llvm::SwitchInst *switch_ins;
	char ti_name [128];

	builder->CreateInvoke (fn_type, llvm::unwrap (callee), llvm::unwrap (cont_bb), llvm::unwrap (pad_bb), llvm::ArrayRef<llvm::Value *> ());

	cont_builder = this->create_builder ();
	cont_builder->SetInsertPoint (llvm::unwrap (cont_bb));
	cont_builder->CreateUnreachable ();

	pad_builder = this->create_builder ();
	pad_builder->SetInsertPoint (llvm::unwrap (pad_bb));

	this->get_mono_personality ();

	{
		LLVMTypeRef members [2], ret_type;

		members [0] = llvm::wrap (llvm::PointerType::get (this->llvm_ctx (), 0));
		members [1] = llvm::wrap (i32_ty);
		ret_type = struct_type (llvm_ctx (), members, 2, FALSE);

		landing_pad = llvm::wrap (pad_builder->CreateLandingPad (llvm::unwrap (ret_type), 1, ""));
	}

	sprintf (ti_name, "resume_type_info_%d", this->clause_id (clause_index));
	ti_init [0] = llvm::wrap (llvm::ConstantInt::get (i32_ty, this->clause_id (clause_index), false));
	ti_init [1] = llvm::wrap (llvm::ConstantInt::get (i32_ty, mono::MONO_LSDA_KIND_RESUME_PAD, false));
	type_info = LLVMAddGlobal (this->lmodule, ti_type, ti_name);
	LLVMSetInitializer (type_info, LLVMConstNamedStruct (ti_type, ti_init, 2));
	LLVMAddClause (landing_pad, type_info);

	/*
	 * Route the encloser the runtime picked to its handler body. A catch encloser
	 * reads the exception object out of ex_var, which only a catch pad stores at
	 * entry - this pad never did - so the store goes on that encloser's own edge,
	 * in a trampoline block. Cleanup enclosers take no store and branch straight in.
	 */
	auto dispatch_target = [&] (int encloser) {
		MonoBasicBlock *handler_bb = this->clause_to_handler [encloser];
		LLVMBasicBlockRef target = this->bblocks [handler_bb->block_num].call_handler_target_bb;

		g_assert (target);

		if (cfg->header->clauses [encloser].flags == MONO_EXCEPTION_CLAUSE_NONE && this->ex_var) {
			LLVMBasicBlockRef store_bb = append_basic_block (this->lmethod, "resume_to_catch");
			llvm::IRBuilder<> *store_builder = this->create_builder ();
			llvm::IRBuilder<> *saved = this->builder;

			store_builder->SetInsertPoint (llvm::unwrap (store_bb));
			this->builder = store_builder;
			llvm::Value *ex_obj = store_builder->CreateExtractValue (llvm::unwrap (landing_pad), {0}, "ex_obj");
			store_builder->CreateStore (this->convert (ex_obj, llvm::unwrap (ObjRefType ())), this->ex_var);
			store_builder->CreateBr (llvm::unwrap (target));
			this->builder = saved;

			target = store_bb;
		}

		return target;
	};

	{
		llvm::Value *ex_selector = pad_builder->CreateExtractValue (llvm::unwrap (landing_pad), {1}, "ex_selector");

		switch_ins = pad_builder->CreateSwitch (ex_selector, llvm::unwrap (dispatch_target (enclosers [0])), enclosers.size () - 1);
		for (std::size_t k = 1; k < enclosers.size (); ++k)
			switch_ins->addCase (llvm::cast<llvm::ConstantInt> (llvm::ConstantInt::get (i32_ty, this->clause_id (enclosers [k]), false)), llvm::unwrap (dispatch_target (enclosers [k])));
	}

	*builder_ref = this->builder = builder;
}

/*
 * clause_type_info_global:
 *
 *   The global that names IL clause CLAUSE_INDEX where a landing pad has to refer
 * to it, created on first use. Its 2-word {i32 clause_index, i32 kind} initializer
 * smuggles both the join key and the clause's flags past the ttype table, which
 * MonoEHGatherPass (eh-gather.cpp) reads back in-process, before any relocation.
 *
 * One global per clause rather than one per mention: a clause covers every pad
 * nested inside it, so the same clause is named from several pads, and sharing the
 * global is what makes those mentions recognisably the same clause.
 */
LLVMValueRef
EmitContext::clause_type_info_global (int clause_index)
{
	auto known = this->clause_type_info.find (clause_index);

	if (known != this->clause_type_info.end ())
		return known->second;

	/*
	 * Only has to be unique within a module, but it is cheaper to keep one
	 * counter for the process than to thread one through - and it has to be
	 * atomic, because several compiles run at once.
	 */
	static std::atomic<int> ti_generator;
	llvm::Type *i32_ty = llvm::Type::getInt32Ty (this->llvm_ctx ());
	LLVMTypeRef ti_members [2] = { llvm::wrap (i32_ty), llvm::wrap (i32_ty) };
	LLVMTypeRef ti_type = struct_type (this->llvm_ctx (), ti_members, 2, FALSE);
	LLVMValueRef ti_init [2];
	LLVMValueRef type_info;
	char ti_name [128];

	sprintf (ti_name, "type_info_%d", ti_generator.fetch_add (1, std::memory_order_relaxed));

	ti_init [0] = llvm::wrap (llvm::ConstantInt::get (i32_ty, this->clause_id (clause_index), false));
	ti_init [1] = llvm::wrap (llvm::ConstantInt::get (i32_ty, this->cfg->header->clauses [clause_index].flags, false));

	type_info = LLVMAddGlobal (this->lmodule, ti_type, ti_name);
	LLVMSetInitializer (type_info, LLVMConstNamedStruct (ti_type, ti_init, 2));

	this->clause_type_info [clause_index] = type_info;
	return type_info;
}

/*
 * add_covering_clauses:
 *
 *   Name, on LANDING_PAD, every clause that covers it: CLAUSE_INDEX's own sibling
 * group first, then each enclosing clause from innermost outwards.
 *
 * That operand list is the nesting chain the published table is built from. The
 * gather pass reads it back and `.mono_lsda` keeps it in order, so build_ex_info ()
 * gets the chain for a faulting PC without having to re-derive it from IL offsets -
 * which is what lets a chain span methods, since an inlined body's IL offsets mean
 * nothing in the caller. LLVM's own inliner extends the chain for us: folding a
 * body into an invoke appends the call site's chain to every pad it brought along
 * (InlineFunction.cpp), which is exactly the outer half of the nest.
 *
 * Ascending IL clause index is innermost-first (ECMA-335 12.4.2.5 puts a more
 * deeply nested clause before its enclosers), which is the order the runtime's flat
 * first-match walk needs to run an intervening finally before an enclosing catch.
 */
void
EmitContext::add_covering_clauses (LLVMValueRef landing_pad, int clause_index)
{
	MonoCompile *cfg = this->cfg;
	MonoExceptionClause *self = &cfg->header->clauses [clause_index];

	for (int i = 0; i < cfg->header->num_clauses; ++i) {
		MonoExceptionClause *c = &cfg->header->clauses [i];

		/*
		 * A finally/fault owns its pad alone - it takes no exception object and
		 * has no siblings. A catch shares one pad with every catch over the
		 * identical try region, in declaration order, so the runtime's isinst
		 * walk tries the more-derived one first.
		 */
		if (self->flags == MONO_EXCEPTION_CLAUSE_NONE) {
			if (c->flags != MONO_EXCEPTION_CLAUSE_NONE)
				continue;
			if (c->try_offset != self->try_offset || c->try_len != self->try_len)
				continue;
		} else if (i != clause_index) {
			continue;
		}

		LLVMAddClause (landing_pad, this->clause_type_info_global (i));
	}

	for (int i = 0; i < cfg->header->num_clauses; ++i) {
		if (clause_encloses (self, &cfg->header->clauses [i]))
			LLVMAddClause (landing_pad, this->clause_type_info_global (i));
	}
}

void
EmitContext::emit_handler_start (MonoBasicBlock *bb, llvm::IRBuilder<> *builder)
{
	MonoCompile *cfg = this->cfg;
	llvm::Value **values = this->values;
	BBInfo *bblocks = this->bblocks;
	LLVMTypeRef i8ptr;
	LLVMBasicBlockRef target_bb;
	MonoInst *exvar;
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

		this->get_mono_personality ();
		i8ptr = llvm::wrap (llvm::PointerType::get (this->llvm_ctx (), 0));

		{
			LLVMTypeRef members [2], ret_type;

			members [0] = i8ptr;
			members [1] = llvm::wrap (llvm::Type::getInt32Ty (this->llvm_ctx ()));
			ret_type = struct_type (llvm_ctx (), members, 2, FALSE);

			landing_pad = llvm::wrap (builder->CreateLandingPad (llvm::unwrap (ret_type), cfg->header->num_clauses, ""));
		}

		if (this_clause->flags == MONO_EXCEPTION_CLAUSE_FINALLY ||
		    this_clause->flags == MONO_EXCEPTION_CLAUSE_FAULT) {
			/*
			 * A finally/fault handler owns its landing pad alone - unlike a catch it
			 * receives no exception object and has no siblings. Control goes to
			 * call_handler_target_bb, which the OP_START_HANDLER / OP_ENDFINALLY
			 * machinery drives on both the exceptional-unwind and the leave
			 * normal-exit paths. No catch-style exception-object store, and no
			 * sibling selector switch.
			 */
			this->add_covering_clauses (landing_pad, clause_index);

			/*
			 * A pad that takes part in nesting grows a selector switch; a standalone
			 * one is a straight branch into the handler body.
			 *
			 * The switch is a fallback rather than the usual route. Once this cleanup
			 * runs, everything the runtime dispatches after it comes in through the
			 * resume pad emit_resume_unwind () builds, not through here - so these
			 * cases only carry a clause whose resume pad never made it into the
			 * published table (build_ex_info ()'s handler chaining falls back to the
			 * inner pad then). Keeping them costs one cold compare and covers that.
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

					auto clause_it = this->clause_to_handler.find (i);
					handler_bb = clause_it != this->clause_to_handler.end () ? clause_it->second : nullptr;
					g_assert (handler_bb);
					g_assert (this->bblocks [handler_bb->block_num].call_handler_target_bb);
					case_target = this->bblocks [handler_bb->block_num].call_handler_target_bb;

					if (enc->flags == MONO_EXCEPTION_CLAUSE_NONE && this->ex_var) {
						/*
						 * Enclosing catch: store the exception object on THIS edge only.
						 * builder == this->builder (both at the pad block), and convert ()
						 * inserts via this->builder, so reposition it into the trampoline,
						 * emit the store, then restore it to the pad block for the rest
						 * of the switch build-out.
						 */
						LLVMBasicBlockRef store_bb = append_basic_block (this->lmethod, "finally_to_catch");
						llvm::BasicBlock *saved_ip = builder->GetInsertBlock ();
						builder->SetInsertPoint (llvm::unwrap (store_bb));
						llvm::Value *ex_obj = builder->CreateExtractValue (llvm::unwrap (landing_pad), {0}, "ex_obj");
						builder->CreateStore (this->convert (ex_obj, llvm::unwrap (ObjRefType ())), this->ex_var);
						llvm::wrap (builder->CreateBr (llvm::unwrap (case_target)));
						builder->SetInsertPoint (saved_ip);
						case_target = store_bb;
					}

					switch_ins->addCase (llvm::cast<llvm::ConstantInt> (llvm::ConstantInt::get (llvm::Type::getInt32Ty (this->llvm_ctx ()), this->clause_id (i), false)), llvm::unwrap (case_target));
				}
			}
		} else {
			/*
			 * The runtime picks among sibling catches by isinst in operand order, so
			 * declaration order (inner/more-derived catch first) has to survive into
			 * the pad - which is what add_covering_clauses () emits.
			 */
			this->add_covering_clauses (landing_pad, clause_index);

			/* Store the exception into the exvar */
			if (this->ex_var)
				builder->CreateStore (this->convert (builder->CreateExtractValue (llvm::unwrap (landing_pad), {0}, "ex_obj"), llvm::unwrap (ObjRefType ())), this->ex_var);

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

				auto clause_it = this->clause_to_handler.find (i);
				handler_bb = clause_it != this->clause_to_handler.end () ? clause_it->second : nullptr;
				g_assert (handler_bb);
				g_assert (this->bblocks [handler_bb->block_num].call_handler_target_bb);
				switch_ins->addCase (llvm::cast<llvm::ConstantInt> (llvm::ConstantInt::get (llvm::Type::getInt32Ty (this->llvm_ctx ()), this->clause_id (i), false)), llvm::unwrap (this->bblocks [handler_bb->block_num].call_handler_target_bb));
			}

			/*
			 * Enclosing-clause selector cases. A catch that does not match runs
			 * nothing, so the runtime keeps dispatching the clauses outside it through
			 * THIS pad rather than moving on to another one - these cases are what
			 * carry those.
			 *
			 * The runtime delivers a fault in a doubly-protected call to the INNERMOST
			 * landing pad (this one), carrying the MATCHED clause's index in the
			 * selector (RDX). For each clause j that
			 * ENCLOSES this catch, add a case routing RDX==j to j's handler body, so
			 * control is re-dispatched to the right enclosing handler. Enclosing
			 * handlers may be catch/finally/fault alike - clause_to_handler and
			 * call_handler_target_bb are populated for every clause (translator.cpp) -
			 * so any encloser is a valid target. The exception-object store above
			 * already ran at pad entry and is correct for an enclosing catch too (it
			 * reads the same ex_var). Enclosers have a different try range than this
			 * clause, so they are never same-range siblings and never collide with the
			 * sibling cases above.
			 */
			for (i = 0; i < cfg->header->num_clauses; ++i) {
				MonoBasicBlock *handler_bb;

				if (!clause_encloses (this_clause, &cfg->header->clauses [i]))
					continue;

				auto clause_it = this->clause_to_handler.find (i);
				handler_bb = clause_it != this->clause_to_handler.end () ? clause_it->second : nullptr;
				g_assert (handler_bb);
				g_assert (this->bblocks [handler_bb->block_num].call_handler_target_bb);
				switch_ins->addCase (llvm::cast<llvm::ConstantInt> (llvm::ConstantInt::get (llvm::Type::getInt32Ty (this->llvm_ctx ()), this->clause_id (i), false)), llvm::unwrap (this->bblocks [handler_bb->block_num].call_handler_target_bb));
			}
		}
	} else {
		/*
		 * No landing pad for this clause, for one of two reasons. Either it is a
		 * secondary sibling, reached only through the group's invoke-target
		 * landing pad selector switch, which branches straight to this clause's
		 * call_handler_target_bb; or nothing in its protected region can raise
		 * into it at all. Every throw the translator emits - implicit exceptions
		 * and explicit throw/rethrow alike - goes through emit_call (), which is
		 * what turns a call inside a try region into an invoke, so a clause with
		 * no invoke has nothing to catch. A finally in that position is still
		 * entered normally through OP_CALL_HANDLER on the leave path, which is
		 * why the handler body below is emitted either way.
		 *
		 * In both cases this clause's own EH entry block is never an unwind
		 * destination, so terminate it as unreachable to keep the IR well-formed.
		 */
		llvm::wrap (builder->CreateUnreachable ());
	}

	/* Start a new bblock which CALL_HANDLER can branch to */
	this->builder = builder = this->create_builder ();
	this->builder->SetInsertPoint (llvm::unwrap (target_bb));

	this->bblocks [bb->block_num].end_bblock = target_bb;

	/* Store the exception into the IL level exvar */
	if (bb->in_scount == 1) {
		g_assert (bb->in_scount == 1);
		exvar = bb->in_stack [0];

		// FIXME: This is shared with filter clauses ?
		g_assert (!values [exvar->dreg]);

		g_assert (this->ex_var);
		values [exvar->dreg] = builder->CreateLoad (llvm::unwrap (ObjRefType ()), this->ex_var, "");
		emit_volatile_store (exvar->dreg);
	}

	/* Make normal branches to the start of the clause branch to the new bblock */
	bblocks [bb->block_num].bblock = target_bb;
}

//Wasm requires us to canonicalize NaNs.
LLVMValueRef
EmitContext::get_double_const (MonoCompile *cfg, double val)
{
#ifdef TARGET_WASM
	if (mono_isnan (val))
		*reinterpret_cast<gint64 *>(&val) = 0x7FF8000000000000ll;
#endif
	return llvm::wrap (llvm::ConstantFP::get (llvm::Type::getDoubleTy (llvm_ctx ()), val));
}

LLVMValueRef
EmitContext::get_float_const (MonoCompile *cfg, float val)
{
#ifdef TARGET_WASM
	if (mono_isnan (val))
		*reinterpret_cast<int *>(&val) = 0x7FC00000;
#endif
	if (cfg->r4fp)
		return llvm::wrap (llvm::ConstantFP::get (llvm::Type::getFloatTy (llvm_ctx ()), val));
	else
		/* LLVM 18 removed the FPExt const-expr; val is already a float, so this
		 * double constant is exactly the extension of the float constant. */
		return llvm::wrap (llvm::ConstantFP::get (llvm::Type::getDoubleTy (llvm_ctx ()), val));
}

LLVMValueRef
EmitContext::call_intrins (int id, LLVMValueRef *args, const char *name)
{
	LLVMValueRef intrins = this->get_intrins (id);
	int nargs = LLVMCountParamTypes (LLVMGlobalGetValueType (intrins));

	for (int i = 0; i < nargs; ++i) {
		LLVMTypeRef t1 = LLVMTypeOf (args [i]);
		LLVMTypeRef t2 = LLVMTypeOf (LLVMGetParam (intrins, i));
		if (t1 != t2)
			args [i] = llvm::wrap (this->convert (llvm::unwrap (args [i]), llvm::unwrap (t2)));
	}

	return llvm::wrap (this->builder->CreateCall (llvm::cast<llvm::FunctionType> (llvm::unwrap (LLVMGlobalGetValueType (intrins))), llvm::unwrap (intrins), gep_index_list (args, nargs), name));
}


#endif /* DISABLE_JIT */

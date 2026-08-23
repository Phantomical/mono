/**
 * \file
 * transform CIL into different opcodes for more
 * efficient interpretation
 *
 * Written by Bernie Solomon (bernard@ugsolutions.com)
 * Copyright (c) 2004.
 */

#include "config.h"
#include <string.h>
#include <mono/metadata/appdomain.h>
#include <mono/metadata/class-internals.h>
#include <mono/metadata/debug-helpers.h>
#include <mono/metadata/exception.h>
#include <mono/metadata/exception-internals.h>
#include <mono/metadata/metadata-update.h>
#include <mono/metadata/mono-endian.h>
#include <mono/metadata/marshal.h>
#include <mono/metadata/profiler-private.h>
#include <mono/metadata/tabledefs.h>
#include <mono/metadata/mono-basic-block.h>
#include <mono/metadata/abi-details.h>
#include <mono/metadata/reflection-internals.h>
#include <mono/utils/unlocked.h>
#include <mono/utils/mono-logger-internals.h>
#include <mono/utils/mono-memory-model.h>

#include <mono/mini/jit-dump.hpp>
#include <mono/mini/mini.h>
#include <mono/mini/mini-runtime.h>

#include "mintops.hpp"
#include "runtime/internals.hpp"
#include "runtime/sharing.hpp"
#include "interp.h"
#include "transform.hpp"
#include "internal.hpp"

#include <mono/llvm/runtime/options.hpp>

#include <algorithm>
#include <memory>
#include <optional>
#include <vector>

#include "mono/llvm/runtime.h"

/* Outside the namespace, because interp-internals.hpp declares it there. */
MonoInterpStats mono_interp_stats;

namespace mono::interp {

#define DEBUG 0

#if SIZEOF_VOID_P == 8
#define MINT_CONV_OVF_U4_P MINT_CONV_OVF_U4_I8
#else
#define MINT_CONV_OVF_U4_P MINT_CONV_OVF_U4_I4
#endif

static const char *stack_type_string[] = {"I4", "I8", "R4", "R8", "O ", "VT", "MP", "F "};

/*
 * op_for_stack_type () indexes an opcode family by the numeric value of a
 * StackType. Every family it is used on has to be laid out in mintops.def in
 * the order StackType names: I4, I8, R4, R8.
 */
#define ASSERT_STACK_TYPE_FAMILY(base, suffix)                                            \
	static_assert (                                                                       \
		base##_I8##suffix == op_for_stack_type (base##_I4##suffix, StackType::I8)         \
			&& base##_R4##suffix == op_for_stack_type (base##_I4##suffix, StackType::R4)  \
			&& base##_R8##suffix == op_for_stack_type (base##_I4##suffix, StackType::R8), \
		#base #suffix " is not laid out in StackType order")

ASSERT_STACK_TYPE_FAMILY (MINT_BEQ, );
ASSERT_STACK_TYPE_FAMILY (MINT_BEQ, _S);
ASSERT_STACK_TYPE_FAMILY (MINT_ADD, );
ASSERT_STACK_TYPE_FAMILY (MINT_SUB, );
ASSERT_STACK_TYPE_FAMILY (MINT_MUL, );
ASSERT_STACK_TYPE_FAMILY (MINT_DIV, );
ASSERT_STACK_TYPE_FAMILY (MINT_REM, );
ASSERT_STACK_TYPE_FAMILY (MINT_CEQ, );
ASSERT_STACK_TYPE_FAMILY (MINT_CGT, );
ASSERT_STACK_TYPE_FAMILY (MINT_CLT, );

/*
 * op_for_mint_type () does the same for the families laid out in MintType
 * order. Some of them stop at O rather than carrying a VT member, so the VT
 * check is its own macro.
 */
#define ASSERT_MINT_TYPE_FAMILY(base)                                             \
	static_assert (base##_U1 == op_for_mint_type (base##_I1, MintType::U1)        \
	                   && base##_I2 == op_for_mint_type (base##_I1, MintType::I2) \
	                   && base##_U2 == op_for_mint_type (base##_I1, MintType::U2) \
	                   && base##_I4 == op_for_mint_type (base##_I1, MintType::I4) \
	                   && base##_I8 == op_for_mint_type (base##_I1, MintType::I8) \
	                   && base##_R4 == op_for_mint_type (base##_I1, MintType::R4) \
	                   && base##_R8 == op_for_mint_type (base##_I1, MintType::R8) \
	                   && base##_O == op_for_mint_type (base##_I1, MintType::O),  \
	               #base " is not laid out in MintType order")

#define ASSERT_MINT_TYPE_FAMILY_VT(base)                                    \
	ASSERT_MINT_TYPE_FAMILY (base);                                         \
	static_assert (base##_VT == op_for_mint_type (base##_I1, MintType::VT), \
	               #base "_VT is not where MintType puts it")

ASSERT_MINT_TYPE_FAMILY_VT (MINT_LDFLD);
ASSERT_MINT_TYPE_FAMILY_VT (MINT_LDFLD_VT);
ASSERT_MINT_TYPE_FAMILY_VT (MINT_STFLD);
ASSERT_MINT_TYPE_FAMILY_VT (MINT_LDSFLD);
ASSERT_MINT_TYPE_FAMILY_VT (MINT_STSFLD);
ASSERT_MINT_TYPE_FAMILY (MINT_LDTSFLD);
ASSERT_MINT_TYPE_FAMILY (MINT_STTSFLD);

/*
 * MINT_MOV leaves the pattern after U2: everything four bytes wide and up
 * shares MINT_MOV_4 and MINT_MOV_8 rather than getting a member each.
 */
static_assert (MINT_MOV_U1 == op_for_mint_type (MINT_MOV_I1, MintType::U1)
                   && MINT_MOV_I2 == op_for_mint_type (MINT_MOV_I1, MintType::I2)
                   && MINT_MOV_U2 == op_for_mint_type (MINT_MOV_I1, MintType::U2),
               "the sign-extending MINT_MOVs are not laid out in MintType order");

/*
 * A token naming a type that will not load resolves to no class at all, and a
 * class that is merely broken resolves to one carrying the reason. Only the
 * second has a failure to report. Asking the first for one dereferences null.
 */
#define CHECK_TYPELOAD(klass)                                                               \
	do {                                                                                    \
		if (!(klass)) {                                                                     \
			mono_error_set_type_load_name (error, NULL, NULL,                               \
			                               "Could not load type from token 0x%08x", token); \
			return FALSE;                                                                   \
		}                                                                                   \
		if (mono_class_has_failure (klass)) {                                               \
			mono_error_set_for_class_failure (error, klass);                                \
			return FALSE;                                                                   \
		}                                                                                   \
	} while (0)

/// Owns the basic block list mono_basic_block_split () builds.
struct BasicBlockListDeleter {
	void operator() (MonoSimpleBasicBlock *bb) const { mono_basic_block_free (bb); }
};

using BasicBlockList = std::unique_ptr<MonoSimpleBasicBlock, BasicBlockListDeleter>;

/// Keeps a method off the inline candidates for as long as the scope lives,
/// which is what stops a method being inlined into itself.
class DontInlineScope {
public:
	DontInlineScope (TransformData *td, MonoMethod *method) : td_ (td)
	{
		td_->dont_inline.push_back (method);
	}

	~DontInlineScope () { td_->dont_inline.pop_back (); }

	DontInlineScope (const DontInlineScope &) = delete;
	DontInlineScope &operator= (const DontInlineScope &) = delete;

private:
	TransformData *td_;
};

gboolean
TransformData::generate_code (MonoMethod *method, MonoMethodHeader *header,
                              MonoGenericContext *generic_context, MonoError *error)
{
	int target;
	int offset, i, i32;
	MintType mt;
	guint32 token;
	int in_offset;
	const unsigned char *end;
	MonoSimpleBasicBlock *bb = NULL;
	gboolean sym_seq_points = FALSE;
	MonoBitSet *seq_point_locs = NULL;
	gboolean readonly = FALSE;
	gboolean volatile_ = FALSE;
	gboolean tailcall = FALSE;
	MonoClass *constrained_class = NULL;
	MonoClass *klass;
	MonoClassField *field;
	MonoImage *image = m_class_get_image (method->klass);
	InterpMethod *rtm = this->rtm;
	MonoDomain *domain = rtm->domain;
	MonoMethodSignature *signature = mono_method_signature_internal (method);
	int num_args = signature->hasthis + signature->param_count;
	int arglist_local = -1;
	gboolean emitted_funccall_seq_point = FALSE;
	std::vector<guint32> arg_locals;
	std::vector<guint32> local_locals;
	InterpInst *last_seq_point = NULL;
	gboolean save_last_error = FALSE;
	gboolean link_bblocks = TRUE;
	gboolean inlining = this->method != method;
	InterpBasicBlock *exit_bb = NULL;

	DontInlineScope dont_inline (this, method);

	BasicBlockList original_bb (mono_basic_block_split (method, error, header));
	return_val_if_nok (error, FALSE);
	bb = original_bb.get ();
	g_assert (bb);

	il_code = header->code;
	in_start = ip = header->code;
	end = ip + header->code_size;

	cbb = entry_bb = arena.create<InterpBasicBlock> ();
	cbb->index = bb_count++;
	cbb->native_offset = -1;
	cbb->stack_height = sp - stack;

	if (inlining) {
		exit_bb = arena.create<InterpBasicBlock> ();
		exit_bb->index = bb_count++;
		exit_bb->native_offset = -1;
		exit_bb->stack_height = -1;
	}

	get_basic_blocks (header, gen_sdb_seq_points);

	if (!inlining)
		initialize_clause_bblocks ();

	if (gen_sdb_seq_points && !inlining) {
		MonoDebugMethodInfo *minfo;

		minfo = mono_debug_lookup_method (method);

		if (minfo) {
			MonoSymSeqPoint *sps;
			int i, n_il_offsets;

			mono_debug_get_seq_points (minfo, NULL, NULL, NULL, &sps, &n_il_offsets);
			seq_point_locs = mono_bitset_mem_new (
				arena.alloc0 (mono_bitset_alloc_size (header->code_size, 0), alignof (gsize)),
				header->code_size, 0);
			sym_seq_points = TRUE;

			for (i = 0; i < n_il_offsets; ++i) {
				if (sps[i].il_offset < header->code_size)
					mono_bitset_set_fast (seq_point_locs, sps[i].il_offset);
			}
			g_free (sps);

			MonoDebugMethodAsyncInfo *asyncMethod =
				mono_debug_lookup_method_async_debug_info (method);
			if (asyncMethod) {
				for (i = 0; asyncMethod != NULL && i < asyncMethod->num_awaits; i++) {
					mono_bitset_set_fast (seq_point_locs, asyncMethod->resume_offsets[i]);
					mono_bitset_set_fast (seq_point_locs, asyncMethod->yield_offsets[i]);
				}
				mono_debug_free_method_async_debug_info (asyncMethod);
			}
		} else if (!method->wrapper_type && !method->dynamic
		           && mono_debug_image_has_debug_info (m_class_get_image (method->klass))) {
			/* Methods without line number info like auto-generated property accessors */
			seq_point_locs = mono_bitset_mem_new (
				arena.alloc0 (mono_bitset_alloc_size (header->code_size, 0), alignof (gsize)),
				header->code_size, 0);
			sym_seq_points = TRUE;
		}
	}

	if (sym_seq_points) {
		last_seq_point = interp_add_ins (MINT_SDB_SEQ_POINT);
		last_seq_point->flags |= INTERP_INST_FLAG_SEQ_POINT_METHOD_ENTRY;
	}

	if (mono_debugger_method_has_breakpoint (method)) {
		interp_add_ins (MINT_BREAKPOINT);
	}

	if (!inlining) {
		if (verbose_level) {
			char *tmp = mono_disasm_code (NULL, method, ip, end);
			char *name = mono_method_full_name (method, TRUE);
			g_print ("Method %s, original code:\n", name);
			g_print ("%s\n", tmp);
			g_free (tmp);
			g_free (name);
		}

		if (rtm->vararg) {
			// vararg calls are identical to normal calls on the call site. However, the
			// first instruction in a vararg method needs to copy the variable arguments
			// into a special region so they can be accessed by MINT_ARGLIST. This region
			// is localloc'ed so we have compile time static offsets for all locals/stack.
			arglist_local = create_interp_local (m_class_get_byval_arg (mono_defaults.int_class));
			interp_add_ins (MINT_INIT_ARGLIST);
			interp_ins_set_dreg (last_ins, arglist_local);
			// This is the offset where the variable args are on stack. After this instruction
			// which copies them to localloc'ed memory, this space will be overwritten by normal
			// locals
			last_ins->data[0] = il_locals_offset;
			has_localloc = TRUE;
		}

		/*
		 * We initialize the locals regardless of the presence of the init_locals
		 * flag. Locals holding references need to be zeroed so we don't risk
		 * crashing the GC if they end up being stored in an object.
		 *
		 * FIXME
		 * Track values of locals over multiple basic blocks. This would enable
		 * us to kill the MINT_INITLOCALS instruction if all locals are initialized
		 * before use. We also don't need this instruction if the init locals flag
		 * is not set and there are no locals holding references.
		 */
		if (header->num_locals) {
			interp_add_ins (MINT_INITLOCALS);
			last_ins->data[0] = il_locals_offset;
			last_ins->data[1] = il_locals_size;
		}

		guint16 enter_profiling = 0;
		if (mono_jit_trace_calls != NULL && mono_trace_eval (method))
			enter_profiling |= TRACING_FLAG;
		if (rtm->prof_flags & MONO_PROFILER_CALL_INSTRUMENTATION_ENTER)
			enter_profiling |= PROFILING_FLAG;
		if (enter_profiling) {
			interp_add_ins (MINT_PROF_ENTER);
			last_ins->data[0] = enter_profiling;
		}

		/*
		 * If safepoints are required by default, always check for polling,
		 * without emitting new instructions. This optimizes method entry in
		 * the common scenario, which is coop.
		 */
#if !defined(ENABLE_HYBRID_SUSPEND) && !defined(ENABLE_COOP_SUSPEND)
		/* safepoint is required on method entry */
		if (mono_threads_are_safepoints_enabled ())
			interp_add_ins (MINT_SAFEPOINT);
#endif

		/*
		 * shared_form () shares an instance method of a reference generic class
		 * and nothing else, so argument 0 is an object whose vtable carries the
		 * instantiation's runtime generic context.
		 *
		 * Entry copies it, so a fetch further down reads the receiver the method
		 * was entered with rather than whatever argument 0 holds by then.
		 * starg.0 and a write through ldarga.0 both reach that argument.
		 */
		if (sharing && signature->hasthis) {
			rgctx_receiver_local = create_interp_local (mono_get_object_type ());
			interp_add_ins (MINT_MOV_P);
			interp_ins_set_dreg (last_ins, rgctx_receiver_local);
			interp_ins_set_sreg (last_ins, 0);
		}
	} else {
		int local;
		arg_locals.resize (!!signature->hasthis + signature->param_count);
		/* Allocate locals to store inlined method args from stack */
		for (i = signature->param_count - 1; i >= 0; i--) {
			local = create_interp_local (signature->params[i]);
			arg_locals[i + !!signature->hasthis] = local;
			store_local (local);
		}

		if (signature->hasthis) {
			/*
			 * If this is value type, it is passed by address and not by value.
			 * Valuetype this local gets integer type MintType::I.
			 */
			MonoType *type;
			if (m_class_is_valuetype (method->klass))
				type = mono_get_int_type ();
			else
				type = mono_get_object_type ();
			local = create_interp_local (type);
			arg_locals[0] = local;
			store_local (local);
		}

		local_locals.resize (header->num_locals);
		/* Allocate locals to store inlined method args from stack */
		for (i = 0; i < header->num_locals; i++)
			local_locals[i] = create_interp_local (header->locals[i]);
	}

	while (ip < end) {
		// Each site that records a refusal returns as well. This catches one
		// that forgets, before the next instruction reaches a shared class.
		if (sharing_refusal != nullptr)
			return TRUE;

		g_assert (sp >= stack);
		in_offset = ip - header->code;
		if (!inlining)
			current_il_offset = in_offset;

		InterpBasicBlock *new_bb = offset_to_bb[in_offset];
		if (new_bb != NULL && cbb != new_bb) {
			/* We are starting a new basic block. Change cbb and link them together */
			if (link_bblocks) {
				/*
				 * By default we link cbb with the new starting bblock, unless the previous
				 * instruction is an unconditional branch (BR, LEAVE, ENDFINALLY)
				 */
				interp_link_bblocks (cbb, new_bb);
				fixup_newbb_stack_locals (new_bb);
			}
			cbb->next_bb = new_bb;
			cbb = new_bb;

			if (new_bb->stack_height >= 0) {
				if (new_bb->stack_height > 0)
					memcpy (stack, new_bb->stack_state, new_bb->stack_height * sizeof (stack[0]));
				sp = stack + new_bb->stack_height;
			} else if (link_bblocks) {
				/* This bblock is not branched to. Initialize its stack state */
				init_bb_stack_state (new_bb);
			}
			link_bblocks = TRUE;
			if (!inlining) {
				int index = clause_indexes[in_offset];
				if (index != -1) {
					MonoExceptionClause *clause = &header->clauses[index];
					if ((clause->flags == MONO_EXCEPTION_CLAUSE_FINALLY
					     || clause->flags == MONO_EXCEPTION_CLAUSE_FAULT)
					    && in_offset == clause->handler_offset)
						interp_add_ins (MINT_START_ABORT_PROT);
				}
			}
		}
		offset_to_bb[in_offset] = cbb;
		in_start = ip;

		if (in_offset == bb->end)
			bb = bb->next;

		if (bb->dead) {
			int op_size = mono_opcode_size (ip, end);
			g_assert (op_size > 0); /* The BB formation pass must catch all bad ops */

			if (verbose_level > 1)
				g_print ("SKIPPING DEAD OP at %x\n", in_offset);
			link_bblocks = FALSE;
			ip += op_size;
			continue;
		}

		if (verbose_level > 1) {
			g_print ("IL_%04lx %-10s, sp %ld, %s %-12s\n", ip - il_code, mono_opcode_name (*ip),
			         sp - stack, sp > stack ? stack_type_string[(int) sp[-1].type] : "  ",
			         (sp > stack && (sp[-1].type == StackType::O || sp[-1].type == StackType::VT))
			             ? (sp[-1].klass == NULL ? "?" : m_class_get_name (sp[-1].klass))
			             : "");
		}

		if (sym_seq_points && mono_bitset_test_fast (seq_point_locs, ip - header->code)) {
			if (in_offset == 0 || (header->num_clauses && !cbb->last_ins))
				interp_add_ins (MINT_SDB_INTR_LOC);
			last_seq_point = interp_add_ins (MINT_SDB_SEQ_POINT);
		}

		if (prof_coverage) {
			guint32 cil_offset = ip - header->code;
			gpointer counter = &coverage_info->data[cil_offset].count;
			coverage_info->data[cil_offset].cil_code = (unsigned char *) ip;

			interp_add_ins (MINT_PROF_COVERAGE_STORE);
			WRITE64_INS (last_ins, 0, &counter);
		}

		switch (*ip) {
		case CEE_NOP:
			/* lose it */
			emitted_funccall_seq_point = FALSE;
			++ip;
			break;
		case CEE_BREAK:
			interp_add_ins (MINT_BREAK);
			++ip;
			break;
		case CEE_LDARG_0:
		case CEE_LDARG_1:
		case CEE_LDARG_2:
		case CEE_LDARG_3: {
			int arg_n = *ip - CEE_LDARG_0;
			if (!inlining)
				load_arg (arg_n);
			else
				load_local (arg_locals[arg_n]);
			++ip;
			break;
		}
		case CEE_LDLOC_0:
		case CEE_LDLOC_1:
		case CEE_LDLOC_2:
		case CEE_LDLOC_3: {
			int loc_n = *ip - CEE_LDLOC_0;
			if (!inlining)
				load_local (num_args + loc_n);
			else
				load_local (local_locals[loc_n]);
			++ip;
			break;
		}
		case CEE_STLOC_0:
		case CEE_STLOC_1:
		case CEE_STLOC_2:
		case CEE_STLOC_3: {
			int loc_n = *ip - CEE_STLOC_0;
			if (!inlining)
				store_local (num_args + loc_n);
			else
				store_local (local_locals[loc_n]);
			++ip;
			break;
		}
		case CEE_LDARG_S: {
			int arg_n = ((guint8 *) ip)[1];
			if (!inlining)
				load_arg (arg_n);
			else
				load_local (arg_locals[arg_n]);
			ip += 2;
			break;
		}
		case CEE_LDARGA_S: {
			/* NOTE: n includes this */
			int n = ((guint8 *) ip)[1];

			if (!inlining) {
				interp_add_ins (MINT_LDLOCA_S);
				interp_ins_set_sreg (last_ins, n);
				locals[n].indirects++;
			} else {
				int loc_n = arg_locals[n];
				interp_add_ins (MINT_LDLOCA_S);
				interp_ins_set_sreg (last_ins, loc_n);
				locals[loc_n].indirects++;
			}
			push_simple_type (StackType::MP);
			interp_ins_set_dreg (last_ins, sp[-1].local);
			ip += 2;
			break;
		}
		case CEE_STARG_S: {
			int arg_n = ((guint8 *) ip)[1];
			if (!inlining)
				store_arg (arg_n);
			else
				store_local (arg_locals[arg_n]);
			ip += 2;
			break;
		}
		case CEE_LDLOC_S: {
			int loc_n = ((guint8 *) ip)[1];
			if (!inlining)
				load_local (num_args + loc_n);
			else
				load_local (local_locals[loc_n]);
			ip += 2;
			break;
		}
		case CEE_LDLOCA_S: {
			int loc_n = ((guint8 *) ip)[1];
			interp_add_ins (MINT_LDLOCA_S);
			if (!inlining)
				loc_n += num_args;
			else
				loc_n = local_locals[loc_n];
			interp_ins_set_sreg (last_ins, loc_n);
			locals[loc_n].indirects++;
			push_simple_type (StackType::MP);
			interp_ins_set_dreg (last_ins, sp[-1].local);
			ip += 2;
			break;
		}
		case CEE_STLOC_S: {
			int loc_n = ((guint8 *) ip)[1];
			if (!inlining)
				store_local (num_args + loc_n);
			else
				store_local (local_locals[loc_n]);
			ip += 2;
			break;
		}
		case CEE_LDNULL:
			interp_add_ins (MINT_LDNULL);
			push_type (StackType::O, NULL);
			interp_ins_set_dreg (last_ins, sp[-1].local);
			++ip;
			break;
		case CEE_LDC_I4_M1:
			interp_add_ins (MINT_LDC_I4_M1);
			push_simple_type (StackType::I4);
			interp_ins_set_dreg (last_ins, sp[-1].local);
			++ip;
			break;
		case CEE_LDC_I4_0:
			if (in_offset + 2 < code_size && interp_ip_in_cbb (in_offset + 1) && ip[1] == 0xfe
			    && ip[2] == CEE_CEQ && sp > stack && sp[-1].type == StackType::I4) {
				interp_add_ins (MINT_CEQ0_I4);
				sp--;
				interp_ins_set_sreg (last_ins, sp[0].local);
				push_simple_type (StackType::I4);
				interp_ins_set_dreg (last_ins, sp[-1].local);
				ip += 3;
			} else {
				interp_add_ins (MINT_LDC_I4_0);
				push_simple_type (StackType::I4);
				interp_ins_set_dreg (last_ins, sp[-1].local);
				++ip;
			}
			break;
		case CEE_LDC_I4_1:
			if (in_offset + 1 < code_size && interp_ip_in_cbb (in_offset + 1)
			    && (ip[1] == CEE_ADD || ip[1] == CEE_SUB) && sp[-1].type == StackType::I4) {
				interp_add_ins (ip[1] == CEE_ADD ? MINT_ADD1_I4 : MINT_SUB1_I4);
				sp--;
				interp_ins_set_sreg (last_ins, sp[0].local);
				push_simple_type (StackType::I4);
				interp_ins_set_dreg (last_ins, sp[-1].local);
				ip += 2;
			} else {
				interp_add_ins (MINT_LDC_I4_1);
				push_simple_type (StackType::I4);
				interp_ins_set_dreg (last_ins, sp[-1].local);
				++ip;
			}
			break;
		case CEE_LDC_I4_2:
		case CEE_LDC_I4_3:
		case CEE_LDC_I4_4:
		case CEE_LDC_I4_5:
		case CEE_LDC_I4_6:
		case CEE_LDC_I4_7:
		case CEE_LDC_I4_8:
			interp_add_ins ((*ip - CEE_LDC_I4_0) + MINT_LDC_I4_0);
			push_simple_type (StackType::I4);
			interp_ins_set_dreg (last_ins, sp[-1].local);
			++ip;
			break;
		case CEE_LDC_I4_S:
			interp_add_ins (MINT_LDC_I4_S);
			last_ins->data[0] = ((gint8 *) ip)[1];
			push_simple_type (StackType::I4);
			interp_ins_set_dreg (last_ins, sp[-1].local);
			ip += 2;
			break;
		case CEE_LDC_I4:
			i32 = read32 (ip + 1);
			interp_add_ins (MINT_LDC_I4);
			WRITE32_INS (last_ins, 0, &i32);
			push_simple_type (StackType::I4);
			interp_ins_set_dreg (last_ins, sp[-1].local);
			ip += 5;
			break;
		case CEE_LDC_I8: {
			gint64 val = read64 (ip + 1);
			interp_add_ins (MINT_LDC_I8);
			WRITE64_INS (last_ins, 0, &val);
			push_simple_type (StackType::I8);
			interp_ins_set_dreg (last_ins, sp[-1].local);
			ip += 9;
			break;
		}
		case CEE_LDC_R4: {
			float val;
			readr4 (ip + 1, &val);
			interp_add_ins (MINT_LDC_R4);
			WRITE32_INS (last_ins, 0, &val);
			push_simple_type (StackType::R4);
			interp_ins_set_dreg (last_ins, sp[-1].local);
			ip += 5;
			break;
		}
		case CEE_LDC_R8: {
			double val;
			readr8 (ip + 1, &val);
			interp_add_ins (MINT_LDC_R8);
			WRITE64_INS (last_ins, 0, &val);
			push_simple_type (StackType::R8);
			interp_ins_set_dreg (last_ins, sp[-1].local);
			ip += 9;
			break;
		}
		case CEE_DUP: {
			StackType type = sp[-1].type;
			MonoClass *klass = sp[-1].klass;
			MintType mt = locals[sp[-1].local].mt;
			if (mt == MintType::VT) {
				gint32 size = mono_class_value_size (klass, NULL);
				g_assert (size < G_MAXUINT16);

				interp_add_ins (MINT_MOV_VT);
				interp_ins_set_sreg (last_ins, sp[-1].local);
				push_type_vt (klass, size);
				interp_ins_set_dreg (last_ins, sp[-1].local);
				last_ins->data[0] = size;
			} else {
				interp_add_ins (get_mov_for_type (mt, FALSE));
				interp_ins_set_sreg (last_ins, sp[-1].local);
				push_type (type, klass);
				interp_ins_set_dreg (last_ins, sp[-1].local);
			}
			ip++;
			break;
		}
		case CEE_POP:
			CHECK_STACK (1);
			interp_add_ins (MINT_NOP);
			--sp;
			++ip;
			break;
		case CEE_JMP: {
			MonoMethod *m;
			INLINE_FAILURE;
			if (sp > stack)
				g_warning ("CEE_JMP: stack must be empty");
			token = read32 (ip + 1);
			m = mono_get_method_checked (image, token, NULL, generic_context, error);
			return_val_if_nok (error, FALSE);
			if (sharing && depends_on_context (m)) {
				cannot_share ("a jump to a method the generic context names");
				return TRUE;
			}
			interp_add_ins (MINT_JMP);
			last_ins->data[0] = get_data_item_index (mono_interp_get_imethod (domain, m, error));
			return_val_if_nok (error, FALSE);
			ip += 5;
			break;
		}
		case CEE_CALLVIRT: /* Fall through */
		case CEE_CALLI:    /* Fall through */
		case CEE_CALL: {
			gboolean need_seq_point = FALSE;

			if (sym_seq_points && !mono_bitset_test_fast (seq_point_locs, ip + 5 - header->code))
				need_seq_point = TRUE;

			if (!interp_transform_call (method, NULL, domain, generic_context, constrained_class,
			                            readonly, error, TRUE, save_last_error, tailcall))
				return FALSE;

			if (need_seq_point) {
				//check is is a nested call and remove the MONO_INST_NONEMPTY_STACK of the last breakpoint, only for non native methods
				if (!(method->flags & METHOD_IMPL_ATTRIBUTE_NATIVE)) {
					if (emitted_funccall_seq_point) {
						if (last_seq_point)
							last_seq_point->flags |= INTERP_INST_FLAG_SEQ_POINT_NESTED_CALL;
					} else
						emitted_funccall_seq_point = TRUE;
				}
				last_seq_point = interp_add_ins (MINT_SDB_SEQ_POINT);
				// This seq point is actually associated with the instruction following the call
				last_seq_point->il_offset = ip - header->code;
				last_seq_point->flags = INTERP_INST_FLAG_SEQ_POINT_NONEMPTY_STACK;
			}

			constrained_class = NULL;
			readonly = FALSE;
			save_last_error = FALSE;
			tailcall = FALSE;
			break;
		}
		case CEE_RET: {
			MonoType *ult = mini_type_get_underlying_type (signature->ret);
			int vt_size = 0;

			link_bblocks = FALSE;

			/* Before the inlined case leaves, since the value is this method's either way. */
			if (ult->type != MONO_TYPE_VOID) {
				CHECK_STACK (1);
				emit_convert (ult);
			}

			/* Return from inlined method, return value is on top of stack */
			if (inlining) {
				ip++;
				fixup_newbb_stack_locals (exit_bb);
				interp_add_ins (MINT_BR_S);
				last_ins->info.target_bb = exit_bb;
				init_bb_stack_state (exit_bb);
				interp_link_bblocks (cbb, exit_bb);
				break;
			}

			if (ult->type != MONO_TYPE_VOID) {
				--sp;
				if (mint_type (ult) == MintType::VT) {
					MonoClass *klass = mono_class_from_mono_type_internal (ult);
					vt_size = mono_class_value_size (klass, NULL);
				}
			}
			if (sp > stack) {
				mono_error_set_generic_error (error, "System", "InvalidProgramException", "");
				return FALSE;
			}

			if (sym_seq_points) {
				last_seq_point = interp_add_ins (MINT_SDB_SEQ_POINT);
				last_ins->flags |= INTERP_INST_FLAG_SEQ_POINT_METHOD_EXIT;
			}

			guint16 exit_profiling = 0;
			if (mono_jit_trace_calls != NULL && mono_trace_eval (method))
				exit_profiling |= TRACING_FLAG;
			if (rtm->prof_flags & MONO_PROFILER_CALL_INSTRUMENTATION_LEAVE)
				exit_profiling |= PROFILING_FLAG;
			if (exit_profiling) {
				/* This does the return as well */
				interp_add_ins (MINT_PROF_EXIT);
				if (ult->type == MONO_TYPE_VOID) {
					vt_size = -1;
					interp_ins_set_sreg (last_ins, -1);
				} else {
					interp_ins_set_sreg (last_ins, sp[0].local);
				}

				last_ins->data[0] = exit_profiling;
				WRITE32_INS (last_ins, 1, &vt_size);
			} else {
				if (vt_size == 0) {
					if (ult->type == MONO_TYPE_VOID) {
						interp_add_ins (MINT_RET_VOID);
					} else {
						interp_add_ins (MINT_RET);
						interp_ins_set_sreg (last_ins, sp[0].local);
					}
				} else {
					interp_add_ins (MINT_RET_VT);
					g_assert (vt_size < G_MAXUINT16);
					interp_ins_set_sreg (last_ins, sp[0].local);
					last_ins->data[0] = vt_size;
				}
			}
			++ip;
			break;
		}
		case CEE_BR: {
			int offset = read32 (ip + 1);
			if (offset) {
				handle_branch (MINT_BR_S, MINT_BR, 5 + offset);
				link_bblocks = FALSE;
			}
			ip += 5;
			break;
		}
		case CEE_BR_S: {
			int offset = (gint8) ip[1];
			if (offset) {
				handle_branch (MINT_BR_S, MINT_BR, 2 + (gint8) ip[1]);
				link_bblocks = FALSE;
			}
			ip += 2;
			break;
		}
		case CEE_BRFALSE:
			one_arg_branch (MINT_BRFALSE_I4, read32 (ip + 1), 5);
			ip += 5;
			break;
		case CEE_BRFALSE_S:
			one_arg_branch (MINT_BRFALSE_I4, (gint8) ip[1], 2);
			ip += 2;
			break;
		case CEE_BRTRUE:
			one_arg_branch (MINT_BRTRUE_I4, read32 (ip + 1), 5);
			ip += 5;
			break;
		case CEE_BRTRUE_S:
			one_arg_branch (MINT_BRTRUE_I4, (gint8) ip[1], 2);
			ip += 2;
			break;
		case CEE_BEQ:
			two_arg_branch (MINT_BEQ_I4, read32 (ip + 1), 5);
			ip += 5;
			break;
		case CEE_BEQ_S:
			two_arg_branch (MINT_BEQ_I4, (gint8) ip[1], 2);
			ip += 2;
			break;
		case CEE_BGE:
			two_arg_branch (MINT_BGE_I4, read32 (ip + 1), 5);
			ip += 5;
			break;
		case CEE_BGE_S:
			two_arg_branch (MINT_BGE_I4, (gint8) ip[1], 2);
			ip += 2;
			break;
		case CEE_BGT:
			two_arg_branch (MINT_BGT_I4, read32 (ip + 1), 5);
			ip += 5;
			break;
		case CEE_BGT_S:
			two_arg_branch (MINT_BGT_I4, (gint8) ip[1], 2);
			ip += 2;
			break;
		case CEE_BLT:
			two_arg_branch (MINT_BLT_I4, read32 (ip + 1), 5);
			ip += 5;
			break;
		case CEE_BLT_S:
			two_arg_branch (MINT_BLT_I4, (gint8) ip[1], 2);
			ip += 2;
			break;
		case CEE_BLE:
			two_arg_branch (MINT_BLE_I4, read32 (ip + 1), 5);
			ip += 5;
			break;
		case CEE_BLE_S:
			two_arg_branch (MINT_BLE_I4, (gint8) ip[1], 2);
			ip += 2;
			break;
		case CEE_BNE_UN:
			two_arg_branch (MINT_BNE_UN_I4, read32 (ip + 1), 5);
			ip += 5;
			break;
		case CEE_BNE_UN_S:
			two_arg_branch (MINT_BNE_UN_I4, (gint8) ip[1], 2);
			ip += 2;
			break;
		case CEE_BGE_UN:
			two_arg_branch (MINT_BGE_UN_I4, read32 (ip + 1), 5);
			ip += 5;
			break;
		case CEE_BGE_UN_S:
			two_arg_branch (MINT_BGE_UN_I4, (gint8) ip[1], 2);
			ip += 2;
			break;
		case CEE_BGT_UN:
			two_arg_branch (MINT_BGT_UN_I4, read32 (ip + 1), 5);
			ip += 5;
			break;
		case CEE_BGT_UN_S:
			two_arg_branch (MINT_BGT_UN_I4, (gint8) ip[1], 2);
			ip += 2;
			break;
		case CEE_BLE_UN:
			two_arg_branch (MINT_BLE_UN_I4, read32 (ip + 1), 5);
			ip += 5;
			break;
		case CEE_BLE_UN_S:
			two_arg_branch (MINT_BLE_UN_I4, (gint8) ip[1], 2);
			ip += 2;
			break;
		case CEE_BLT_UN:
			two_arg_branch (MINT_BLT_UN_I4, read32 (ip + 1), 5);
			ip += 5;
			break;
		case CEE_BLT_UN_S:
			two_arg_branch (MINT_BLT_UN_I4, (gint8) ip[1], 2);
			ip += 2;
			break;
		case CEE_SWITCH: {
			guint32 n;
			const unsigned char *next_ip;
			++ip;
			n = read32 (ip);
			narrow_index (sp - 1);
			interp_add_ins_explicit (MINT_SWITCH, MINT_SWITCH_LEN (n));
			WRITE32_INS (last_ins, 0, &n);
			ip += 4;
			next_ip = ip + n * 4;
			--sp;
			interp_ins_set_sreg (last_ins, sp[0].local);
			InterpBasicBlock **target_bb_table = arena.create_array<InterpBasicBlock *> (n);
			for (i = 0; i < n; i++) {
				offset = read32 (ip);
				target = next_ip - il_code + offset;
				InterpBasicBlock *target_bb = offset_to_bb[target];
				g_assert (target_bb);
				if (offset < 0) {
#if DEBUG_INTERP
					if (stack_height > 0 && stack_height != target_bb->stack_height)
						g_warning ("SWITCH with back branch and non-empty stack");
#endif
				} else {
					init_bb_stack_state (target_bb);
				}
				target_bb_table[i] = target_bb;
				interp_link_bblocks (cbb, target_bb);
				ip += 4;
			}
			last_ins->info.target_bb_table = target_bb_table;
			break;
		}
		case CEE_LDIND_I1:
			handle_ldind (MINT_LDIND_I1_CHECK, StackType::I4, &volatile_);
			break;
		case CEE_LDIND_U1:
			handle_ldind (MINT_LDIND_U1_CHECK, StackType::I4, &volatile_);
			break;
		case CEE_LDIND_I2:
			handle_ldind (MINT_LDIND_I2_CHECK, StackType::I4, &volatile_);
			break;
		case CEE_LDIND_U2:
			handle_ldind (MINT_LDIND_U2_CHECK, StackType::I4, &volatile_);
			break;
		case CEE_LDIND_I4:
			handle_ldind (MINT_LDIND_I4_CHECK, StackType::I4, &volatile_);
			break;
		case CEE_LDIND_U4:
			handle_ldind (MINT_LDIND_U4_CHECK, StackType::I4, &volatile_);
			break;
		case CEE_LDIND_I8:
			handle_ldind (MINT_LDIND_I8_CHECK, StackType::I8, &volatile_);
			break;
		case CEE_LDIND_I:
			handle_ldind (MINT_LDIND_REF_CHECK, StackType::I, &volatile_);
			break;
		case CEE_LDIND_R4:
			handle_ldind (MINT_LDIND_R4_CHECK, StackType::R4, &volatile_);
			break;
		case CEE_LDIND_R8:
			handle_ldind (MINT_LDIND_R8_CHECK, StackType::R8, &volatile_);
			break;
		case CEE_LDIND_REF:
			handle_ldind (MINT_LDIND_REF_CHECK, StackType::O, &volatile_);
			break;
		case CEE_STIND_REF:
			handle_stind (MINT_STIND_REF, &volatile_);
			break;
		case CEE_STIND_I1:
			handle_stind (MINT_STIND_I1, &volatile_);
			break;
		case CEE_STIND_I2:
			handle_stind (MINT_STIND_I2, &volatile_);
			break;
		case CEE_STIND_I4:
			handle_stind (MINT_STIND_I4, &volatile_);
			break;
		case CEE_STIND_I:
			handle_stind (MINT_STIND_I, &volatile_);
			break;
		case CEE_STIND_I8:
			handle_stind (MINT_STIND_I8, &volatile_);
			break;
		case CEE_STIND_R4:
			handle_stind (MINT_STIND_R4, &volatile_);
			break;
		case CEE_STIND_R8:
			handle_stind (MINT_STIND_R8, &volatile_);
			break;
		case CEE_ADD:
			binary_arith_op (MINT_ADD_I4);
			++ip;
			break;
		case CEE_SUB:
			binary_arith_op (MINT_SUB_I4);
			++ip;
			break;
		case CEE_MUL:
			binary_arith_op (MINT_MUL_I4);
			++ip;
			break;
		case CEE_DIV:
			binary_arith_op (MINT_DIV_I4);
			++ip;
			break;
		case CEE_DIV_UN:
			binary_arith_op (MINT_DIV_UN_I4);
			++ip;
			break;
		case CEE_REM:
			binary_arith_op (MINT_REM_I4);
			++ip;
			break;
		case CEE_REM_UN:
			binary_arith_op (MINT_REM_UN_I4);
			++ip;
			break;
		case CEE_AND:
			binary_arith_op (MINT_AND_I4);
			++ip;
			break;
		case CEE_OR:
			binary_arith_op (MINT_OR_I4);
			++ip;
			break;
		case CEE_XOR:
			binary_arith_op (MINT_XOR_I4);
			++ip;
			break;
		case CEE_SHL:
			shift_op (MINT_SHL_I4);
			++ip;
			break;
		case CEE_SHR:
			shift_op (MINT_SHR_I4);
			++ip;
			break;
		case CEE_SHR_UN:
			shift_op (MINT_SHR_UN_I4);
			++ip;
			break;
		case CEE_NEG:
			unary_arith_op (MINT_NEG_I4);
			++ip;
			break;
		case CEE_NOT:
			unary_arith_op (MINT_NOT_I4);
			++ip;
			break;
		case CEE_CONV_U1:
			CHECK_STACK (1);
			switch (sp[-1].type) {
			case StackType::R4:
				interp_add_conv (sp - 1, NULL, StackType::I4, MINT_CONV_U1_R4);
				break;
			case StackType::R8:
				interp_add_conv (sp - 1, NULL, StackType::I4, MINT_CONV_U1_R8);
				break;
			case StackType::I4:
				interp_add_conv (sp - 1, NULL, StackType::I4, MINT_CONV_U1_I4);
				break;
			case StackType::I8:
				interp_add_conv (sp - 1, NULL, StackType::I4, MINT_CONV_U1_I8);
				break;
			default:
				g_assert_not_reached ();
			}
			++ip;
			break;
		case CEE_CONV_I1:
			CHECK_STACK (1);
			switch (sp[-1].type) {
			case StackType::R4:
				interp_add_conv (sp - 1, NULL, StackType::I4, MINT_CONV_I1_R4);
				break;
			case StackType::R8:
				interp_add_conv (sp - 1, NULL, StackType::I4, MINT_CONV_I1_R8);
				break;
			case StackType::I4:
				interp_add_conv (sp - 1, NULL, StackType::I4, MINT_CONV_I1_I4);
				break;
			case StackType::I8:
				interp_add_conv (sp - 1, NULL, StackType::I4, MINT_CONV_I1_I8);
				break;
			default:
				g_assert_not_reached ();
			}
			++ip;
			break;
		case CEE_CONV_U2:
			CHECK_STACK (1);
			switch (sp[-1].type) {
			case StackType::R4:
				interp_add_conv (sp - 1, NULL, StackType::I4, MINT_CONV_U2_R4);
				break;
			case StackType::R8:
				interp_add_conv (sp - 1, NULL, StackType::I4, MINT_CONV_U2_R8);
				break;
			case StackType::I4:
				interp_add_conv (sp - 1, NULL, StackType::I4, MINT_CONV_U2_I4);
				break;
			case StackType::I8:
				interp_add_conv (sp - 1, NULL, StackType::I4, MINT_CONV_U2_I8);
				break;
			default:
				g_assert_not_reached ();
			}
			++ip;
			break;
		case CEE_CONV_I2:
			CHECK_STACK (1);
			switch (sp[-1].type) {
			case StackType::R4:
				interp_add_conv (sp - 1, NULL, StackType::I4, MINT_CONV_I2_R4);
				break;
			case StackType::R8:
				interp_add_conv (sp - 1, NULL, StackType::I4, MINT_CONV_I2_R8);
				break;
			case StackType::I4:
				interp_add_conv (sp - 1, NULL, StackType::I4, MINT_CONV_I2_I4);
				break;
			case StackType::I8:
				interp_add_conv (sp - 1, NULL, StackType::I4, MINT_CONV_I2_I8);
				break;
			default:
				g_assert_not_reached ();
			}
			++ip;
			break;
		case CEE_CONV_U:
			CHECK_STACK (1);
			switch (sp[-1].type) {
			case StackType::R8:
#if SIZEOF_VOID_P == 4
				interp_add_conv (sp - 1, NULL, StackType::I, MINT_CONV_U4_R8);
#else
				interp_add_conv (sp - 1, NULL, StackType::I, MINT_CONV_U8_R8);
#endif
				break;
			case StackType::I4:
#if SIZEOF_VOID_P == 8
				interp_add_conv (sp - 1, NULL, StackType::I, MINT_CONV_I8_U4);
#endif
				break;
			case StackType::I8:
#if SIZEOF_VOID_P == 4
				interp_add_conv (sp - 1, NULL, StackType::I, MINT_CONV_U4_I8);
#endif
				break;
			case StackType::MP:
			case StackType::O:
				SET_SIMPLE_TYPE (sp - 1, StackType::I);
				break;
			default:
				g_assert_not_reached ();
			}
			++ip;
			break;
		case CEE_CONV_I:
			CHECK_STACK (1);
			switch (sp[-1].type) {
			case StackType::R8:
#if SIZEOF_VOID_P == 8
				interp_add_conv (sp - 1, NULL, StackType::I, MINT_CONV_I8_R8);
#else
				interp_add_conv (sp - 1, NULL, StackType::I, MINT_CONV_I4_R8);
#endif
				break;
			case StackType::I4:
#if SIZEOF_VOID_P == 8
				interp_add_conv (sp - 1, NULL, StackType::I, MINT_CONV_I8_I4);
#endif
				break;
			case StackType::O:
			case StackType::MP:
				SET_SIMPLE_TYPE (sp - 1, StackType::I);
				break;
			case StackType::I8:
#if SIZEOF_VOID_P == 4
				interp_add_conv (sp - 1, NULL, StackType::I, MINT_CONV_I4_I8);
#endif
				break;
			default:
				g_assert_not_reached ();
			}
			++ip;
			break;
		case CEE_CONV_U4:
			CHECK_STACK (1);
			switch (sp[-1].type) {
			case StackType::R4:
				interp_add_conv (sp - 1, NULL, StackType::I4, MINT_CONV_U4_R4);
				break;
			case StackType::R8:
				interp_add_conv (sp - 1, NULL, StackType::I4, MINT_CONV_U4_R8);
				break;
			case StackType::I4:
				break;
			case StackType::I8:
				interp_add_conv (sp - 1, NULL, StackType::I4, MINT_CONV_U4_I8);
				break;
			case StackType::MP:
#if SIZEOF_VOID_P == 8
				interp_add_conv (sp - 1, NULL, StackType::I4, MINT_CONV_U4_I8);
#else
				SET_SIMPLE_TYPE (sp - 1, StackType::I4);
#endif
				break;
			default:
				g_assert_not_reached ();
			}
			++ip;
			break;
		case CEE_CONV_I4:
			CHECK_STACK (1);
			switch (sp[-1].type) {
			case StackType::R4:
				interp_add_conv (sp - 1, NULL, StackType::I4, MINT_CONV_I4_R4);
				break;
			case StackType::R8:
				interp_add_conv (sp - 1, NULL, StackType::I4, MINT_CONV_I4_R8);
				break;
			case StackType::I4:
				break;
			case StackType::I8:
				interp_add_conv (sp - 1, NULL, StackType::I4, MINT_CONV_I4_I8);
				break;
			case StackType::MP:
#if SIZEOF_VOID_P == 8
				interp_add_conv (sp - 1, NULL, StackType::I4, MINT_CONV_I4_I8);
#else
				SET_SIMPLE_TYPE (sp - 1, StackType::I4);
#endif
				break;
			default:
				g_assert_not_reached ();
			}
			++ip;
			break;
		case CEE_CONV_I8:
			CHECK_STACK (1);
			switch (sp[-1].type) {
			case StackType::R4:
				interp_add_conv (sp - 1, NULL, StackType::I8, MINT_CONV_I8_R4);
				break;
			case StackType::R8:
				interp_add_conv (sp - 1, NULL, StackType::I8, MINT_CONV_I8_R8);
				break;
			case StackType::I4: {
				if (interp_ins_is_ldc (last_ins) && last_ins == cbb->last_ins) {
					gint64 ct = interp_get_const_from_ldc_i4 (last_ins);
					interp_clear_ins (last_ins);

					interp_add_ins (MINT_LDC_I8);
					sp--;
					push_simple_type (StackType::I8);
					interp_ins_set_dreg (last_ins, sp[-1].local);
					WRITE64_INS (last_ins, 0, &ct);
				} else {
					interp_add_conv (sp - 1, NULL, StackType::I8, MINT_CONV_I8_I4);
				}
				break;
			}
			case StackType::I8:
				break;
			case StackType::MP:
#if SIZEOF_VOID_P == 4
				interp_add_ins (MINT_CONV_I8_I4);
#else
				SET_SIMPLE_TYPE (sp - 1, StackType::I8);
#endif
				break;
			default:
				g_assert_not_reached ();
			}
			++ip;
			break;
		case CEE_CONV_R4:
			CHECK_STACK (1);
			switch (sp[-1].type) {
			case StackType::R8:
				interp_add_conv (sp - 1, NULL, StackType::R4, MINT_CONV_R4_R8);
				break;
			case StackType::I8:
				interp_add_conv (sp - 1, NULL, StackType::R4, MINT_CONV_R4_I8);
				break;
			case StackType::I4:
				interp_add_conv (sp - 1, NULL, StackType::R4, MINT_CONV_R4_I4);
				break;
			case StackType::R4:
				/* no-op */
				break;
			default:
				g_assert_not_reached ();
			}
			++ip;
			break;
		case CEE_CONV_R8:
			CHECK_STACK (1);
			switch (sp[-1].type) {
			case StackType::I4:
				interp_add_conv (sp - 1, NULL, StackType::R8, MINT_CONV_R8_I4);
				break;
			case StackType::I8:
				interp_add_conv (sp - 1, NULL, StackType::R8, MINT_CONV_R8_I8);
				break;
			case StackType::R4:
				interp_add_conv (sp - 1, NULL, StackType::R8, MINT_CONV_R8_R4);
				break;
			case StackType::R8:
				break;
			default:
				g_assert_not_reached ();
			}
			++ip;
			break;
		case CEE_CONV_U8:
			CHECK_STACK (1);
			switch (sp[-1].type) {
			case StackType::I4:
				if (interp_ins_is_ldc (last_ins) && last_ins == cbb->last_ins) {
					gint64 ct = (guint32) interp_get_const_from_ldc_i4 (last_ins);
					interp_clear_ins (last_ins);

					interp_add_ins (MINT_LDC_I8);
					sp--;
					push_simple_type (StackType::I8);
					interp_ins_set_dreg (last_ins, sp[-1].local);
					WRITE64_INS (last_ins, 0, &ct);
				} else {
					interp_add_conv (sp - 1, NULL, StackType::I8, MINT_CONV_I8_U4);
				}
				break;
			case StackType::I8:
				break;
			case StackType::R4:
				interp_add_conv (sp - 1, NULL, StackType::I8, MINT_CONV_U8_R4);
				break;
			case StackType::R8:
				interp_add_conv (sp - 1, NULL, StackType::I8, MINT_CONV_U8_R8);
				break;
			case StackType::MP:
#if SIZEOF_VOID_P == 4
				interp_add_conv (sp - 1, NULL, StackType::I8, MINT_CONV_I8_U4);
#else
				SET_SIMPLE_TYPE (sp - 1, StackType::I8);
#endif
				break;
			default:
				g_assert_not_reached ();
			}
			++ip;
			break;
		case CEE_CPOBJ: {
			CHECK_STACK (2);

			token = read32 (ip + 1);
			klass =
				mono_class_get_and_inflate_typespec_checked (image, token, generic_context, error);
			return_val_if_nok (error, FALSE);

			if (m_class_is_valuetype (klass)) {
				// MINT_CPOBJ copies against the class it is given, and that is
				// the shared form's rather than the instantiation's.
				if (sharing && depends_on_context (klass)) {
					cannot_share ("cpobj of a value type the generic context names");
					return TRUE;
				}

				MintType mt = mint_type (m_class_get_byval_arg (klass));
				sp -= 2;
				interp_add_ins ((mt == MintType::VT) ? MINT_CPOBJ_VT : MINT_CPOBJ);
				interp_ins_set_sregs2 (last_ins, sp[0].local, sp[1].local);
				last_ins->data[0] = get_data_item_index (klass);
			} else {
				sp--;
				interp_add_ins (MINT_LDIND_REF);
				interp_ins_set_sreg (last_ins, sp[0].local);
				push_simple_type (StackType::I);
				interp_ins_set_dreg (last_ins, sp[-1].local);

				sp -= 2;
				interp_add_ins (MINT_STIND_REF);
				interp_ins_set_sregs2 (last_ins, sp[0].local, sp[1].local);
			}
			ip += 5;
			break;
		}
		case CEE_LDOBJ: {
			CHECK_STACK (1);

			token = read32 (ip + 1);

			if (method->wrapper_type != MONO_WRAPPER_NONE)
				klass = (MonoClass *) mono_method_get_wrapper_data (method, token);
			else {
				klass = mono_class_get_and_inflate_typespec_checked (image, token, generic_context,
				                                                     error);
				return_val_if_nok (error, FALSE);
			}
			// interp_emit_ldobj () names the size of the class and the kind of
			// its value, and reference sharing keeps both common.
			interp_emit_ldobj (klass);

			ip += 5;
			BARRIER_IF_VOLATILE (MONO_MEMORY_BARRIER_ACQ);
			break;
		}
		case CEE_LDSTR: {
			token = mono_metadata_token_index (read32 (ip + 1));
			push_type (StackType::O, mono_defaults.string_class);
			if (method->wrapper_type == MONO_WRAPPER_NONE) {
				MonoString *s = mono_ldstr_checked (domain, image, token, error);
				return_val_if_nok (error, FALSE);
				/* GC won't scan code stream, but reference is held by metadata
				 * machinery so we are good here */
				interp_add_ins (MINT_LDSTR);
				interp_ins_set_dreg (last_ins, sp[-1].local);
				last_ins->data[0] = get_data_item_index (s);
			} else {
				/* defer allocation to execution-time */
				interp_add_ins (MINT_LDSTR_TOKEN);
				interp_ins_set_dreg (last_ins, sp[-1].local);
				last_ins->data[0] = get_data_item_index (GUINT_TO_POINTER (token));
			}
			ip += 5;
			break;
		}
		case CEE_NEWOBJ: {
			MonoMethod *m;
			MonoMethodSignature *csignature;

			ip++;
			token = read32 (ip);
			ip += 4;

			m = interp_get_method (method, token, image, generic_context, error);
			return_val_if_nok (error, FALSE);

			/*
			 * MINT_NEWOBJ_DYN takes the class, its vtable and its initializer
			 * off the constructor, so the constructor is the whole of what the
			 * context has to answer. Every arm before that one names the class
			 * itself, so each of them refuses instead.
			 */
			bool ctor_from_context = false;

			if (sharing && depends_on_context (m)) {
				if (inlining) {
					cannot_share ("a newobj inside an inlined callee");
					return TRUE;
				}

				if (m_class_is_valuetype (m->klass)) {
					cannot_share ("a newobj of a value type the generic context names");
					return TRUE;
				}

				if (m_class_get_parent (m->klass) == mono_defaults.array_class) {
					cannot_share ("a newobj of an array the generic context names");
					return TRUE;
				}

				if (m->wrapper_type != MONO_WRAPPER_NONE) {
					cannot_share ("a newobj a wrapper stands in for");
					return TRUE;
				}

				ctor_from_context = true;
			}

			csignature = mono_method_signature_internal (m);
			klass = m->klass;

			// A shared class is left alone. MINT_NEWOBJ_DYN initializes the
			// instantiation's own where the object is made.
			if (!ctor_from_context && !mono_class_init_internal (klass)) {
				mono_error_set_for_class_failure (error, klass);
				return_val_if_nok (error, FALSE);
			}

			if (mono_class_get_flags (klass) & TYPE_ATTRIBUTE_ABSTRACT) {
				char *full_name = mono_type_get_full_name (klass);
				mono_error_set_member_access (error, "Cannot create an abstract class: %s",
				                              full_name);
				g_free (full_name);
				return_val_if_nok (error, FALSE);
			}

			MintType ret_mt = mint_type (m_class_get_byval_arg (klass));
			if (mono_class_is_magic_int (klass) || mono_class_is_magic_float (klass)) {
				g_assert (csignature->param_count == 1);
#if SIZEOF_VOID_P == 8
				if (mono_class_is_magic_int (klass) && sp[-1].type == StackType::I4)
					interp_add_conv (sp - 1, NULL, stack_type_of (ret_mt), MINT_CONV_I8_I4);
				else if (mono_class_is_magic_float (klass) && sp[-1].type == StackType::R4)
					interp_add_conv (sp - 1, NULL, stack_type_of (ret_mt), MINT_CONV_R8_R4);
#endif
			} else if (klass == mono_defaults.int_class && csignature->param_count == 1) {
#if SIZEOF_VOID_P == 8
				if (sp[-1].type == StackType::I4)
					interp_add_conv (sp - 1, NULL, stack_type_of (ret_mt), MINT_CONV_I8_I4);
#else
				if (sp[-1].type == StackType::I8)
					interp_add_conv (sp - 1, NULL, stack_type_of (ret_mt), MINT_CONV_OVF_I4_I8);
#endif
			} else if (m_class_get_parent (klass) == mono_defaults.array_class) {
				sp -= csignature->param_count;
				for (int i = 0; i < csignature->param_count; i++)
					locals[sp[i].local].flags |= INTERP_LOCAL_FLAG_CALL_ARGS;

				interp_add_ins (MINT_NEWOBJ_ARRAY);
				last_ins->data[0] = get_data_item_index (m->klass);
				last_ins->data[1] = csignature->param_count;
				push_type (stack_type_of (ret_mt), klass);
				interp_ins_set_dreg (last_ins, sp[-1].local);
				locals[sp[-1].local].flags |= INTERP_LOCAL_FLAG_CALL_ARGS;
			} else if (klass == mono_defaults.string_class) {
				guint32 tos_offset = get_tos_offset ();
				sp -= csignature->param_count;
				guint32 params_stack_size = tos_offset - get_tos_offset ();

				for (int i = 0; i < csignature->param_count; i++)
					locals[sp[i].local].flags |= INTERP_LOCAL_FLAG_CALL_ARGS;

				interp_add_ins (MINT_NEWOBJ_STRING);
				last_ins->data[0] =
					get_data_item_index (mono_interp_get_imethod (domain, m, error));
				last_ins->data[1] = params_stack_size;
				push_type (stack_type_of (ret_mt), klass);
				interp_ins_set_dreg (last_ins, sp[-1].local);
				locals[sp[-1].local].flags |= INTERP_LOCAL_FLAG_CALL_ARGS;
			} else if (m_class_get_image (klass) == mono_defaults.corlib
			           && !strcmp (m_class_get_name (m->klass), "ByReference`1")
			           && !strcmp (m->name, ".ctor")) {
				/* public ByReference(ref T value) */
				g_assert (csignature->hasthis && csignature->param_count == 1);
				sp--;
				/* We already have the vt on top of the stack. Do a dummy mov that should be optimized out. */
				interp_add_ins (MINT_MOV_P);
				interp_ins_set_sreg (last_ins, sp[0].local);
				push_type_vt (klass, mono_class_value_size (klass, NULL));
				interp_ins_set_dreg (last_ins, sp[-1].local);
			} else if (m_class_get_image (klass) == mono_defaults.corlib
			           && (!strcmp (m_class_get_name (m->klass), "Span`1")
			               || !strcmp (m_class_get_name (m->klass), "ReadOnlySpan`1"))
			           && csignature->param_count == 2
			           && csignature->params[0]->type == MONO_TYPE_PTR
			           && !type_has_references (
						   mono_method_get_context (m)->class_inst->type_argv[0])) {
				/* ctor frequently used with ReadOnlySpan over static arrays */
				interp_add_ins (MINT_INTRINS_SPAN_CTOR);
				sp -= 2;
				interp_ins_set_sregs2 (last_ins, sp[0].local, sp[1].local);
				push_type_vt (klass, mono_class_value_size (klass, NULL));
				interp_ins_set_dreg (last_ins, sp[-1].local);
			} else {
				guint32 tos_offset = get_tos_offset ();
				sp -= csignature->param_count;
				guint32 params_stack_size = tos_offset - get_tos_offset ();

				// Move params types in temporary buffer
				// FIXME stop leaking sp_params
				StackInfo *sp_params =
					(StackInfo *) g_malloc (sizeof (StackInfo) * csignature->param_count);
				memcpy (sp_params, sp, sizeof (StackInfo) * csignature->param_count);

				// We must not optimize out these locals, storing to them is part of the interp call convention
				// FIXME this affects inlining efficiency. We need to first remove the param moving by NEWOBJ
				for (int i = 0; i < csignature->param_count; i++)
					locals[sp_params[i].local].flags |= INTERP_LOCAL_FLAG_CALL_ARGS;

				// Push the return value and `this` argument to the ctor
				gboolean is_vt = m_class_is_valuetype (klass);
				int vtsize = 0;
				if (is_vt) {
					vtsize = mono_class_value_size (klass, NULL);
					if (ret_mt == MintType::VT)
						push_type_vt (klass, vtsize);
					else
						push_type (stack_type_of (ret_mt), klass);
					push_simple_type (StackType::I);
				} else {
					push_type (stack_type_of (ret_mt), klass);
					push_type (stack_type_of (ret_mt), klass);
				}
				int dreg = sp[-2].local;
				locals[dreg].flags |= INTERP_LOCAL_FLAG_CALL_ARGS;

				// Push back the params to top of stack
				push_types (sp_params, csignature->param_count);

				if (ctor_from_context) {
					int callee = emit_rgctx_fetch (MONO_RGCTX_INFO_INTERP_METHOD, m);

					if (sharing_refusal != nullptr)
						return TRUE;

					interp_add_ins (MINT_NEWOBJ_DYN);
					interp_ins_set_dreg (last_ins, dreg);
					interp_ins_set_sreg (last_ins, callee);
					last_ins->data[0] = params_stack_size;
				} else if (!mono_class_is_marshalbyref (klass) && !mono_class_has_finalizer (klass)
				           && !m_class_has_weak_fields (klass)) {
					InterpInst *newobj_fast;

					if (is_vt) {
						newobj_fast = interp_add_ins (MINT_NEWOBJ_VT_FAST);
						interp_ins_set_dreg (newobj_fast, dreg);
						newobj_fast->data[1] = ALIGN_TO (vtsize, MINT_STACK_SLOT_SIZE);
					} else {
						MonoVTable *vtable = mono_class_vtable_checked (domain, klass, error);
						return_val_if_nok (error, FALSE);
						newobj_fast = interp_add_ins (MINT_NEWOBJ_FAST);
						interp_ins_set_dreg (newobj_fast, dreg);
						newobj_fast->data[1] = get_data_item_index (vtable);
					}
					// FIXME remove these once we have our own local offset allocator, even for execution stack locals
					newobj_fast->data[2] = params_stack_size;
					newobj_fast->data[3] = csignature->param_count;

					if ((mono_interp_opt & INTERP_OPT_INLINE)
					    && interp_method_check_inlining (m, csignature)) {
						MonoMethodHeader *mheader = interp_method_get_header (m, error);
						return_val_if_nok (error, FALSE);

						// Add local mapping information for cprop to use, in case we inline
						int param_count = csignature->param_count;
						int *newobj_reg_map = arena.create_array<int> (param_count * 2);
						for (int i = 0; i < param_count; i++) {
							newobj_reg_map[2 * i] = sp_params[i].local;
							newobj_reg_map[2 * i + 1] = sp[-param_count + i].local;
						}

						if (interp_inline_method (m, mheader, error)) {
							newobj_fast->data[0] = INLINED_METHOD_FLAG;
							newobj_fast->info.newobj_reg_map = newobj_reg_map;
							break;
						}
					}
					// Inlining failed. Set the method to be executed as part of newobj instruction
					newobj_fast->data[0] =
						get_data_item_index (mono_interp_get_imethod (domain, m, error));
					/* The constructor was not inlined, abort inlining of current method */
					INLINE_FAILURE;
				} else {
					interp_add_ins (MINT_NEWOBJ);
					g_assert (!m_class_is_valuetype (klass));
					interp_ins_set_dreg (last_ins, dreg);
					last_ins->data[0] =
						get_data_item_index (mono_interp_get_imethod (domain, m, error));
					last_ins->data[1] = params_stack_size;
				}
				return_val_if_nok (error, FALSE);
				// Parameters and this pointer are popped of the stack. The return value remains
				sp -= csignature->param_count + 1;
			}
			break;
		}
		case CEE_CASTCLASS:
		case CEE_ISINST: {
			gboolean isinst_instr = *ip == CEE_ISINST;
			bool from_context = false;
			CHECK_STACK (1);
			token = read32 (ip + 1);
			klass = resolve_class (method, token, generic_context,
			                       inlining ? nullptr : &from_context);
			CHECK_TYPELOAD (klass);
			if (from_context) {
				int klass_local = emit_rgctx_fetch (MONO_RGCTX_INFO_KLASS, klass);

				if (sharing_refusal != nullptr)
					return TRUE;

				interp_handle_isinst_dyn (klass_local, klass, isinst_instr);
				break;
			}
			if (sharing_refusal != nullptr)
				return TRUE;
			interp_handle_isinst (klass, isinst_instr);
			break;
		}
		case CEE_CONV_R_UN:
			switch (sp[-1].type) {
			case StackType::R8:
				break;
			case StackType::I8:
				interp_add_conv (sp - 1, NULL, StackType::R8, MINT_CONV_R_UN_I8);
				break;
			case StackType::I4:
				interp_add_conv (sp - 1, NULL, StackType::R8, MINT_CONV_R_UN_I4);
				break;
			default:
				g_assert_not_reached ();
			}
			++ip;
			break;
		case CEE_UNBOX:
			CHECK_STACK (1);
			token = read32 (ip + 1);

			if (method->wrapper_type != MONO_WRAPPER_NONE)
				klass = (MonoClass *) mono_method_get_wrapper_data (method, token);
			else {
				klass = mono_class_get_and_inflate_typespec_checked (image, token, generic_context,
				                                                     error);
				return_val_if_nok (error, FALSE);
			}
			if (sharing && depends_on_context (klass)) {
				cannot_share ("a class the generic context names");
				return TRUE;
			}

			if (mono_class_is_nullable (klass)) {
				MonoMethod *target_method;
				if (m_class_is_enumtype (mono_class_get_nullable_param_internal (klass)))
					target_method =
						mono_class_get_method_from_name_checked (klass, "UnboxExact", 1, 0, error);
				else
					target_method =
						mono_class_get_method_from_name_checked (klass, "Unbox", 1, 0, error);
				return_val_if_nok (error, FALSE);
				/* ip is incremented by interp_transform_call */
				if (!interp_transform_call (method, target_method, domain, generic_context, NULL,
				                            FALSE, error, FALSE, FALSE, FALSE))
					return FALSE;
				/*
				 * CEE_UNBOX needs to push address of vtype while Nullable.Unbox returns the value type
				 * We create a local variable in the frame so that we can fetch its address.
				 */
				int local = create_interp_local (m_class_get_byval_arg (klass));
				store_local (local);

				interp_add_ins (MINT_LDLOCA_S);
				push_simple_type (StackType::MP);
				interp_ins_set_dreg (last_ins, sp[-1].local);
				interp_ins_set_sreg (last_ins, local);
				locals[local].indirects++;
			} else {
				interp_add_ins (MINT_UNBOX);
				sp--;
				interp_ins_set_sreg (last_ins, sp[0].local);
				push_simple_type (StackType::MP);
				interp_ins_set_dreg (last_ins, sp[-1].local);
				last_ins->data[0] = get_data_item_index (klass);
				ip += 5;
			}
			break;
		case CEE_UNBOX_ANY: {
			bool from_context = false;
			CHECK_STACK (1);
			token = read32 (ip + 1);

			klass = resolve_class (method, token, generic_context,
			                       inlining ? nullptr : &from_context);
			CHECK_TYPELOAD (klass);
			if (sharing_refusal != nullptr)
				return TRUE;

			if (from_context) {
				// The unbox arm below burns the class in as a data item, and
				// MINT_UNBOX compares the boxed object against it. That is the
				// shared form's class, which matches no instantiation.
				if (!mini_type_is_reference (m_class_get_byval_arg (klass))) {
					cannot_share ("unbox.any of a value type the generic context names");
					return TRUE;
				}

				int klass_local = emit_rgctx_fetch (MONO_RGCTX_INFO_KLASS, klass);

				if (sharing_refusal != nullptr)
					return TRUE;

				interp_handle_isinst_dyn (klass_local, klass, FALSE);
				break;
			}

			// Common in generic code:
			// box T + unbox.any T -> nop
			if ((last_ins->opcode == MINT_BOX || last_ins->opcode == MINT_BOX_VT)
			    && (sp - 1)->klass == klass && last_ins == cbb->last_ins) {
				interp_clear_ins (last_ins);
				MintType mt = mint_type (m_class_get_byval_arg (klass));
				sp--;
				// Push back the original value that was boxed. We should handle this in CEE_BOX instead
				if (mt == MintType::VT)
					push_type_vt (klass, mono_class_value_size (klass, NULL));
				else
					push_type (stack_type_of (mt), klass);
				// FIXME do this somewhere else, maybe in super instruction pass, where we would check
				// instruction patterns
				// Restore the local that is on top of the stack
				sp[-1].local = last_ins->sregs[0];
				ip += 5;
				break;
			}

			if (mini_type_is_reference (m_class_get_byval_arg (klass))) {
				interp_handle_isinst (klass, FALSE);
			} else if (mono_class_is_nullable (klass)) {
				MonoMethod *target_method;
				if (m_class_is_enumtype (mono_class_get_nullable_param_internal (klass)))
					target_method =
						mono_class_get_method_from_name_checked (klass, "UnboxExact", 1, 0, error);
				else
					target_method =
						mono_class_get_method_from_name_checked (klass, "Unbox", 1, 0, error);
				return_val_if_nok (error, FALSE);
				/* ip is incremented by interp_transform_call */
				if (!interp_transform_call (method, target_method, domain, generic_context, NULL,
				                            FALSE, error, FALSE, FALSE, FALSE))
					return FALSE;
			} else {
				interp_add_ins (MINT_UNBOX);
				sp--;
				interp_ins_set_sreg (last_ins, sp[0].local);
				push_simple_type (StackType::MP);
				interp_ins_set_dreg (last_ins, sp[-1].local);
				last_ins->data[0] = get_data_item_index (klass);

				interp_emit_ldobj (klass);

				ip += 5;
			}

			break;
		}
		case CEE_THROW:
			INLINE_FAILURE;
			CHECK_STACK (1);
			interp_add_ins (MINT_THROW);
			interp_ins_set_sreg (last_ins, sp[-1].local);
			link_bblocks = FALSE;
			sp = stack;
			++ip;
			break;
		case CEE_LDFLDA: {
			bool from_context = false;
			CHECK_STACK (1);
			token = read32 (ip + 1);
			field = resolve_field (method, token, &klass, generic_context, error, &from_context);
			return_val_if_nok (error, FALSE);
			if (sharing_refusal != nullptr)
				return TRUE;
			MonoType *ftype = mono_field_get_type_internal (field);
			gboolean is_static = !!(ftype->attrs & FIELD_ATTRIBUTE_STATIC);
			mono_class_init_internal (klass);
			if (from_context && !may_share_field_access (klass, is_static, inlining))
				return TRUE;
#ifndef DISABLE_REMOTING
			if (m_class_get_marshalbyref (klass) || mono_class_is_contextbound (klass)
			    || klass == mono_defaults.marshalbyrefobject_class) {
				g_assert (!is_static);
				int offset = m_class_is_valuetype (klass)
				                 ? field->offset - MONO_ABI_SIZEOF (MonoObject)
				                 : field->offset;

				interp_add_ins (MINT_MONO_LDPTR);
				last_ins->data[0] = get_data_item_index (klass);
				push_simple_type (StackType::I);
				interp_ins_set_dreg (last_ins, sp[-1].local);

				interp_add_ins (MINT_MONO_LDPTR);
				last_ins->data[0] = get_data_item_index (field);
				push_simple_type (StackType::I);
				interp_ins_set_dreg (last_ins, sp[-1].local);

				interp_add_ins (MINT_LDC_I4);
				WRITE32_INS (last_ins, 0, &offset);
				push_simple_type (StackType::I4);
				interp_ins_set_dreg (last_ins, sp[-1].local);
#if SIZEOF_VOID_P == 8
				interp_add_conv (sp - 1, NULL, StackType::I8, MINT_CONV_I8_I4);
#endif

				MonoMethod *wrapper = mono_marshal_get_ldflda_wrapper (field->type);
				/* ip is incremented by interp_transform_call */
				if (!interp_transform_call (method, wrapper, domain, generic_context, NULL, FALSE,
				                            error, FALSE, FALSE, FALSE))
					return FALSE;
			} else
#endif
			{
				if (is_static) {
					sp--;
					if (from_context) {
						interp_emit_ldsflda_dyn (field);
						if (sharing_refusal != nullptr)
							return TRUE;
					} else {
						interp_emit_ldsflda (field, error);
						return_val_if_nok (error, FALSE);
					}
				} else {
					sp--;
					if (sp->type == StackType::O) {
						interp_add_ins (MINT_LDFLDA);
					} else {
						StackType sp_type = sp->type;
						g_assert (sp_type == StackType::MP || sp_type == StackType::I);
						interp_add_ins (MINT_LDFLDA_UNSAFE);
					}
					last_ins->data[0] = m_class_is_valuetype (klass)
					                        ? field->offset - MONO_ABI_SIZEOF (MonoObject)
					                        : field->offset;
					interp_ins_set_sreg (last_ins, sp[0].local);
					push_simple_type (StackType::MP);
					interp_ins_set_dreg (last_ins, sp[-1].local);
				}
				ip += 5;
			}
			break;
		}
		case CEE_LDFLD: {
			bool from_context = false;
			CHECK_STACK (1);
			token = read32 (ip + 1);
			field = resolve_field (method, token, &klass, generic_context, error, &from_context);
			return_val_if_nok (error, FALSE);
			if (sharing_refusal != nullptr)
				return TRUE;
			MonoType *ftype = mono_field_get_type_internal (field);
			gboolean is_static = !!(ftype->attrs & FIELD_ATTRIBUTE_STATIC);
			mono_class_init_internal (klass);
			if (from_context && !may_share_field_access (klass, is_static, inlining))
				return TRUE;

			MonoClass *field_klass = mono_class_from_mono_type_internal (ftype);
			mt = mint_type (m_class_get_byval_arg (field_klass));
			int field_size = mono_class_value_size (field_klass, NULL);
			int obj_size = mono_class_value_size (klass, NULL);
			obj_size = ALIGN_TO (obj_size, MINT_VT_ALIGNMENT);

			/*
			 * A magic type rides the stack as the native type it stands for, so
			 * an entry marked with one already holds the field. Reading through
			 * it would take the value for an address. check_magic_int_layout ()
			 * and mono_class_is_magic_float () are what make the two the same
			 * bytes.
			 */
			if (!is_static && sp[-1].klass == klass
			    && mono_class_get_magic_index (klass) >= 0) {
				g_assert (field->offset - MONO_ABI_SIZEOF (MonoObject) == 0);
				SET_TYPE (sp - 1, stack_type_of (mt), field_klass);
				ip += 5;
				BARRIER_IF_VOLATILE (MONO_MEMORY_BARRIER_ACQ);
				break;
			}

#ifndef DISABLE_REMOTING
			if (m_class_get_marshalbyref (klass) || mono_class_is_contextbound (klass)
			    || klass == mono_defaults.marshalbyrefobject_class) {
				g_assert (!is_static);
				interp_add_ins (mt == MintType::VT ? MINT_LDRMFLD_VT : MINT_LDRMFLD);
				sp--;
				interp_ins_set_sreg (last_ins, sp[0].local);
				last_ins->data[0] = get_data_item_index (field);
				if (mt == MintType::VT)
					push_type_vt (field_klass, field_size);
				else
					push_type (stack_type_of (mt), field_klass);
				interp_ins_set_dreg (last_ins, sp[-1].local);
			} else
#endif
			{
				if (is_static) {
					sp--;
					if (from_context) {
						interp_emit_sfld_access_dyn (field, field_klass, mt, TRUE);
						if (sharing_refusal != nullptr)
							return TRUE;
					} else {
						interp_emit_sfld_access (field, field_klass, mt, TRUE, error);
						return_val_if_nok (error, FALSE);
					}
				} else if (sp[-1].type == StackType::VT) {
					/* First we pop the vt object from the stack. Then we push the field */
					int opcode = op_for_mint_type (MINT_LDFLD_VT_I1, mt);
#ifdef NO_UNALIGNED_ACCESS
					if (field->offset % SIZEOF_VOID_P != 0) {
						if (mt == MintType::I8)
							opcode = MINT_LDFLD_VT_I8_UNALIGNED;
						else if (mt == MintType::R8)
							opcode = MINT_LDFLD_VT_R8_UNALIGNED;
					}
#endif
					interp_add_ins (opcode);
					g_assert (m_class_is_valuetype (klass));
					sp--;
					interp_ins_set_sreg (last_ins, sp[0].local);
					last_ins->data[0] = field->offset - MONO_ABI_SIZEOF (MonoObject);
					if (mt == MintType::VT)
						last_ins->data[1] = field_size;
					if (mt == MintType::VT)
						push_type_vt (field_klass, field_size);
					else
						push_type (stack_type_of (mt), field_klass);
					interp_ins_set_dreg (last_ins, sp[-1].local);
				} else {
					int opcode = op_for_mint_type (MINT_LDFLD_I1, mt);
#ifdef NO_UNALIGNED_ACCESS
					if ((mt == MintType::I8 || mt == MintType::R8)
					    && field->offset % SIZEOF_VOID_P != 0)
						opcode = get_unaligned_opcode (opcode);
#endif
					interp_add_ins (opcode);
					sp--;
					interp_ins_set_sreg (last_ins, sp[0].local);
					last_ins->data[0] = m_class_is_valuetype (klass)
					                        ? field->offset - MONO_ABI_SIZEOF (MonoObject)
					                        : field->offset;
					if (mt == MintType::VT) {
						int size = mono_class_value_size (field_klass, NULL);
						g_assert (size < G_MAXUINT16);
						last_ins->data[1] = size;
					}
					if (mt == MintType::VT)
						push_type_vt (field_klass, field_size);
					else
						push_type (stack_type_of (mt), field_klass);
					interp_ins_set_dreg (last_ins, sp[-1].local);
				}
			}
			ip += 5;
			BARRIER_IF_VOLATILE (MONO_MEMORY_BARRIER_ACQ);
			break;
		}
		case CEE_STFLD: {
			bool from_context = false;
			CHECK_STACK (2);
			token = read32 (ip + 1);
			field = resolve_field (method, token, &klass, generic_context, error, &from_context);
			return_val_if_nok (error, FALSE);
			if (sharing_refusal != nullptr)
				return TRUE;
			MonoType *ftype = mono_field_get_type_internal (field);
			gboolean is_static = !!(ftype->attrs & FIELD_ATTRIBUTE_STATIC);
			MonoClass *field_klass = mono_class_from_mono_type_internal (ftype);
			mono_class_init_internal (klass);
			if (from_context && !may_share_field_access (klass, is_static, inlining))
				return TRUE;
			mt = mint_type (ftype);

			emit_convert (ftype);

			BARRIER_IF_VOLATILE (MONO_MEMORY_BARRIER_REL);

#ifndef DISABLE_REMOTING
			if (m_class_get_marshalbyref (klass)) {
				g_assert (!is_static);
				interp_add_ins (mt == MintType::VT ? MINT_STRMFLD_VT : MINT_STRMFLD);
				sp -= 2;
				interp_ins_set_sregs2 (last_ins, sp[0].local, sp[1].local);
				last_ins->data[0] = get_data_item_index (field);
			} else
#endif
			{
				if (is_static) {
					if (from_context) {
						interp_emit_sfld_access_dyn (field, field_klass, mt, FALSE);
						if (sharing_refusal != nullptr)
							return TRUE;
					} else {
						interp_emit_sfld_access (field, field_klass, mt, FALSE, error);
						return_val_if_nok (error, FALSE);
					}

					/* pop the unused object reference */
					sp--;

					/* the vtable of the field might not be initialized at this point */
					mono_class_vtable_checked (domain, field_klass, error);
					return_val_if_nok (error, FALSE);
				} else {
					int opcode = op_for_mint_type (MINT_STFLD_I1, mt);
#ifdef NO_UNALIGNED_ACCESS
					if ((mt == MintType::I8 || mt == MintType::R8)
					    && field->offset % SIZEOF_VOID_P != 0)
						opcode = get_unaligned_opcode (opcode);
#endif
					interp_add_ins (opcode);
					sp -= 2;
					interp_ins_set_sregs2 (last_ins, sp[0].local, sp[1].local);
					last_ins->data[0] = m_class_is_valuetype (klass)
					                        ? field->offset - MONO_ABI_SIZEOF (MonoObject)
					                        : field->offset;
					if (mt == MintType::VT) {
						/* the vtable of the field might not be initialized at this point */
						mono_class_vtable_checked (domain, field_klass, error);
						return_val_if_nok (error, FALSE);
						if (m_class_has_references (field_klass)) {
							last_ins->data[1] = get_data_item_index (field_klass);
						} else {
							last_ins->opcode = MINT_STFLD_VT_NOREF;
							last_ins->data[1] = mono_class_value_size (field_klass, NULL);
						}
					}
				}
			}
			ip += 5;
			break;
		}
		case CEE_LDSFLDA: {
			bool from_context = false;
			token = read32 (ip + 1);
			field = resolve_field (method, token, &klass, generic_context, error,
			                       inlining ? nullptr : &from_context);
			return_val_if_nok (error, FALSE);
			if (sharing_refusal != nullptr)
				return TRUE;
			if (from_context) {
				interp_emit_ldsflda_dyn (field);
				if (sharing_refusal != nullptr)
					return TRUE;
				ip += 5;
				break;
			}
			interp_emit_ldsflda (field, error);
			return_val_if_nok (error, FALSE);
			ip += 5;
			break;
		}
		case CEE_LDSFLD: {
			bool from_context = false;
			token = read32 (ip + 1);
			field = resolve_field (method, token, &klass, generic_context, error,
			                       inlining ? nullptr : &from_context);
			return_val_if_nok (error, FALSE);
			if (sharing_refusal != nullptr)
				return TRUE;
			MonoType *ftype = mono_field_get_type_internal (field);
			mt = mint_type (ftype);
			klass = mono_class_from_mono_type_internal (ftype);
			gboolean in_corlib = m_class_get_image (field->parent) == mono_defaults.corlib;

			if (in_corlib && !strcmp (field->name, "IsLittleEndian")
			    && !strcmp (m_class_get_name (field->parent), "BitConverter")
			    && !strcmp (m_class_get_name_space (field->parent), "System")) {
				interp_add_ins ((TARGET_BYTE_ORDER == G_LITTLE_ENDIAN) ? MINT_LDC_I4_1
				                                                       : MINT_LDC_I4_0);
				push_simple_type (StackType::I4);
				interp_ins_set_dreg (last_ins, sp[-1].local);
				ip += 5;
				break;
			}

			if (from_context) {
				interp_emit_sfld_access_dyn (field, klass, mt, TRUE);
				if (sharing_refusal != nullptr)
					return TRUE;
				ip += 5;
				break;
			}

			interp_emit_sfld_access (field, klass, mt, TRUE, error);
			return_val_if_nok (error, FALSE);

			ip += 5;
			break;
		}
		case CEE_STSFLD: {
			bool from_context = false;
			CHECK_STACK (1);
			token = read32 (ip + 1);
			field = resolve_field (method, token, &klass, generic_context, error,
			                       inlining ? nullptr : &from_context);
			return_val_if_nok (error, FALSE);
			if (sharing_refusal != nullptr)
				return TRUE;
			MonoType *ftype = mono_field_get_type_internal (field);
			mt = mint_type (ftype);

			emit_convert (ftype);

			/* the vtable of the field might not be initialized at this point */
			MonoClass *fld_klass = mono_class_from_mono_type_internal (ftype);
			mono_class_vtable_checked (domain, fld_klass, error);
			return_val_if_nok (error, FALSE);

			if (from_context) {
				interp_emit_sfld_access_dyn (field, fld_klass, mt, FALSE);
				if (sharing_refusal != nullptr)
					return TRUE;
				ip += 5;
				break;
			}

			interp_emit_sfld_access (field, fld_klass, mt, FALSE, error);
			return_val_if_nok (error, FALSE);

			ip += 5;
			break;
		}
		case CEE_STOBJ: {
			bool from_context = false;
			token = read32 (ip + 1);

			if (method->wrapper_type != MONO_WRAPPER_NONE)
				klass = (MonoClass *) mono_method_get_wrapper_data (method, token);
			else
				klass = resolve_class (method, token, generic_context, &from_context);
			CHECK_TYPELOAD (klass);
			if (sharing_refusal != nullptr)
				return TRUE;

			// MINT_STOBJ_VT copies against the class it is given, and that is
			// the shared form's rather than the instantiation's. Every other
			// arm names the kind of the value alone.
			if (from_context && mint_type (m_class_get_byval_arg (klass)) == MintType::VT) {
				cannot_share ("stobj of a value type the generic context names");
				return TRUE;
			}

			BARRIER_IF_VOLATILE (MONO_MEMORY_BARRIER_REL);

			interp_emit_stobj (klass);

			ip += 5;
			break;
		}
		case CEE_CONV_OVF_I_UN:
		case CEE_CONV_OVF_U_UN: {
			CHECK_STACK (1);
			gboolean to_signed = *ip == CEE_CONV_OVF_I_UN;
			int op = -1;

			switch (sp[-1].type) {
			/*
			 * ECMA-335 III.3.29 gives .un no reading on a float source, so these
			 * check the destination's own range, as the form without .un does.
			 */
			case StackType::R4:
#if SIZEOF_VOID_P == 8
				op = to_signed ? MINT_CONV_OVF_I8_R4 : MINT_CONV_OVF_U8_R4;
#else
				op = to_signed ? MINT_CONV_OVF_I4_R4 : MINT_CONV_OVF_U4_R4;
#endif
				break;
			case StackType::R8:
#if SIZEOF_VOID_P == 8
				op = to_signed ? MINT_CONV_OVF_I8_R8 : MINT_CONV_OVF_U8_R8;
#else
				op = to_signed ? MINT_CONV_OVF_I4_R8 : MINT_CONV_OVF_U4_R8;
#endif
				break;
			case StackType::I8:
#if SIZEOF_VOID_P == 8
				/* Read unsigned, an int64 reaches past what a signed native int holds. */
				if (to_signed)
					op = MINT_CONV_OVF_I8_U8;
#else
				op = MINT_CONV_OVF_I4_U8;
#endif
				break;
			case StackType::I4:
#if SIZEOF_VOID_P == 8
				op = MINT_CONV_I8_U4;
#else
				if (to_signed)
					op = MINT_CONV_OVF_I4_U4;
#endif
				break;
			default:
				g_assert_not_reached ();
				break;
			}

			if (op != -1)
				interp_add_conv (sp - 1, NULL, StackType::I, op);
			++ip;
			break;
		}
		case CEE_CONV_OVF_I8_UN:
		case CEE_CONV_OVF_U8_UN: {
			CHECK_STACK (1);
			gboolean to_signed = *ip == CEE_CONV_OVF_I8_UN;
			int op = -1;

			switch (sp[-1].type) {
			/* The float source reads the same way it does for the native int forms. */
			case StackType::R4:
				op = to_signed ? MINT_CONV_OVF_I8_R4 : MINT_CONV_OVF_U8_R4;
				break;
			case StackType::R8:
				op = to_signed ? MINT_CONV_OVF_I8_R8 : MINT_CONV_OVF_U8_R8;
				break;
			case StackType::I8:
				if (to_signed)
					op = MINT_CONV_OVF_I8_U8;
				break;
			case StackType::I4:
				op = MINT_CONV_I8_U4;
				break;
			default:
				g_assert_not_reached ();
				break;
			}

			if (op != -1)
				interp_add_conv (sp - 1, NULL, StackType::I8, op);
			++ip;
			break;
		}
		case CEE_BOX: {
			bool from_context = false;
			CHECK_STACK (1);
			token = read32 (ip + 1);
			if (method->wrapper_type != MONO_WRAPPER_NONE)
				klass = (MonoClass *) mono_method_get_wrapper_data (method, token);
			else
				klass = resolve_class (method, token, generic_context,
				                       inlining ? nullptr : &from_context);
			CHECK_TYPELOAD (klass);
			if (sharing_refusal != nullptr)
				return TRUE;

			if (from_context) {
				// The arms below allocate against the vtable of the class the
				// token resolved to, and each instantiation has its own.
				if (!mini_type_is_reference (m_class_get_byval_arg (klass))) {
					cannot_share ("box of a value type the generic context names");
					return TRUE;
				}

				// Already an object, whatever the context stands for.
				ip += 5;
				break;
			}

			if (mono_class_is_nullable (klass)) {
				MonoMethod *target_method =
					mono_class_get_method_from_name_checked (klass, "Box", 1, 0, error);
				return_val_if_nok (error, FALSE);
				/* ip is incremented by interp_transform_call */
				if (!interp_transform_call (method, target_method, domain, generic_context, NULL,
				                            FALSE, error, FALSE, FALSE, FALSE))
					return FALSE;
			} else if (!m_class_is_valuetype (klass)) {
				/* already boxed, do nothing. */
				ip += 5;
			} else {
				if (G_UNLIKELY (m_class_is_byreflike (klass))) {
					mono_error_set_bad_image (error, image, "Cannot box IsByRefLike type '%s.%s'",
					                          m_class_get_name_space (klass),
					                          m_class_get_name (klass));
					return FALSE;
				}

				const MintType boxed_mt = mint_type (m_class_get_byval_arg (klass));
				const gboolean vt = boxed_mt == MintType::VT;

				coerce_fp (sp - 1, stack_type_of (boxed_mt));
				MonoVTable *vtable = mono_class_vtable_checked (domain, klass, error);
				return_val_if_nok (error, FALSE);

				sp--;
				interp_add_ins (vt ? MINT_BOX_VT : MINT_BOX);
				interp_ins_set_sreg (last_ins, sp[0].local);
				last_ins->data[0] = get_data_item_index (vtable);
				push_type (StackType::O, klass);
				interp_ins_set_dreg (last_ins, sp[-1].local);
				ip += 5;
			}

			break;
		}
		case CEE_NEWARR: {
			bool from_context = false;
			CHECK_STACK (1);
			token = read32 (ip + 1);

			if (method->wrapper_type != MONO_WRAPPER_NONE)
				klass = (MonoClass *) mono_method_get_wrapper_data (method, token);
			else
				klass = resolve_class (method, token, generic_context,
				                       inlining ? nullptr : &from_context);
			CHECK_TYPELOAD (klass);
			if (sharing_refusal != nullptr)
				return TRUE;

			MonoClass *array_class = mono_class_create_array (klass, 1);
			int vtable_local = -1;
			MonoVTable *vtable = nullptr;

			if (from_context) {
				vtable_local = emit_rgctx_fetch (MONO_RGCTX_INFO_VTABLE, array_class);

				if (sharing_refusal != nullptr)
					return TRUE;
			} else {
				vtable = mono_class_vtable_checked (domain, array_class, error);
				return_val_if_nok (error, FALSE);
			}

			StackType lentype = (sp - 1)->type;
			if (lentype == StackType::I8) {
				/* mimic mini behaviour */
				interp_add_conv (sp - 1, NULL, StackType::I4, MINT_CONV_OVF_U4_I8);
			} else {
				g_assert (lentype == StackType::I4);
				interp_add_conv (sp - 1, NULL, StackType::I4, MINT_CONV_OVF_U4_I4);
			}
			sp--;
			if (from_context) {
				interp_add_ins (MINT_NEWARR_DYN);
				interp_ins_set_sregs2 (last_ins, sp[0].local, vtable_local);
			} else {
				interp_add_ins (MINT_NEWARR);
				interp_ins_set_sreg (last_ins, sp[0].local);
			}
			push_type (StackType::O, array_class);
			interp_ins_set_dreg (last_ins, sp[-1].local);
			if (!from_context)
				last_ins->data[0] = get_data_item_index (vtable);
			ip += 5;
			break;
		}
		case CEE_LDLEN:
			CHECK_STACK (1);
			sp--;
			interp_add_ins (MINT_LDLEN);
			interp_ins_set_sreg (last_ins, sp[0].local);
#ifdef MONO_BIG_ARRAYS
			push_simple_type (StackType::I8);
#else
			push_simple_type (StackType::I4);
#endif
			interp_ins_set_dreg (last_ins, sp[-1].local);
			++ip;
			break;
		case CEE_LDELEMA: {
			gint32 size;
			bool from_context = false;
			CHECK_STACK (2);
			narrow_index (sp - 1);
			token = read32 (ip + 1);

			if (method->wrapper_type != MONO_WRAPPER_NONE)
				klass = (MonoClass *) mono_method_get_wrapper_data (method, token);
			else
				klass = resolve_class (method, token, generic_context, &from_context);

			CHECK_TYPELOAD (klass);
			if (sharing_refusal != nullptr)
				return TRUE;

			if (!m_class_is_valuetype (klass) && method->wrapper_type == MONO_WRAPPER_NONE
			    && !readonly) {
				// MINT_LDELEMA_TC tests the element against the class it is
				// given, and that is the shared form's rather than the
				// instantiation's.
				if (from_context) {
					cannot_share ("a checked ldelema of a class the generic context names");
					return TRUE;
				}

				/*
				 * Check the class for failures before the type check, which can
				 * throw other exceptions.
				 */
				mono_class_setup_vtable (klass);
				CHECK_TYPELOAD (klass);
				interp_add_ins (MINT_LDELEMA_TC);
				sp -= 2;
				locals[sp[0].local].flags |= INTERP_LOCAL_FLAG_CALL_ARGS;
				locals[sp[1].local].flags |= INTERP_LOCAL_FLAG_CALL_ARGS;
				push_simple_type (StackType::MP);
				interp_ins_set_dreg (last_ins, sp[-1].local);
				locals[sp[-1].local].flags |= INTERP_LOCAL_FLAG_CALL_ARGS;
				last_ins->data[0] = get_data_item_index (klass);
			} else {
				interp_add_ins (MINT_LDELEMA1);
				sp -= 2;
				interp_ins_set_sregs2 (last_ins, sp[0].local, sp[1].local);
				push_simple_type (StackType::MP);
				interp_ins_set_dreg (last_ins, sp[-1].local);
				mono_class_init_internal (klass);
				size = mono_class_array_element_size (klass);
				last_ins->data[0] = size;
			}

			readonly = FALSE;

			ip += 5;
			break;
		}
		case CEE_LDELEM_I1:
			handle_ldelem (MINT_LDELEM_I1, StackType::I4);
			break;
		case CEE_LDELEM_U1:
			handle_ldelem (MINT_LDELEM_U1, StackType::I4);
			break;
		case CEE_LDELEM_I2:
			handle_ldelem (MINT_LDELEM_I2, StackType::I4);
			break;
		case CEE_LDELEM_U2:
			handle_ldelem (MINT_LDELEM_U2, StackType::I4);
			break;
		case CEE_LDELEM_I4:
			handle_ldelem (MINT_LDELEM_I4, StackType::I4);
			break;
		case CEE_LDELEM_U4:
			handle_ldelem (MINT_LDELEM_U4, StackType::I4);
			break;
		case CEE_LDELEM_I8:
			handle_ldelem (MINT_LDELEM_I8, StackType::I8);
			break;
		case CEE_LDELEM_I:
			handle_ldelem (MINT_LDELEM_I, StackType::I);
			break;
		case CEE_LDELEM_R4:
			handle_ldelem (MINT_LDELEM_R4, StackType::R4);
			break;
		case CEE_LDELEM_R8:
			handle_ldelem (MINT_LDELEM_R8, StackType::R8);
			break;
		case CEE_LDELEM_REF:
			handle_ldelem (MINT_LDELEM_REF, StackType::O);
			break;
		case CEE_LDELEM: {
			bool from_context = false;
			token = read32 (ip + 1);
			// Every arm below names the kind of the element and its size, which
			// reference sharing keeps common.
			klass = resolve_class (method, token, generic_context, &from_context);
			CHECK_TYPELOAD (klass);
			if (sharing_refusal != nullptr)
				return TRUE;
			switch (mint_type (m_class_get_byval_arg (klass))) {
			case MintType::I1:
				handle_ldelem (MINT_LDELEM_I1, StackType::I4);
				break;
			case MintType::U1:
				handle_ldelem (MINT_LDELEM_U1, StackType::I4);
				break;
			case MintType::U2:
				handle_ldelem (MINT_LDELEM_U2, StackType::I4);
				break;
			case MintType::I2:
				handle_ldelem (MINT_LDELEM_I2, StackType::I4);
				break;
			case MintType::I4:
				handle_ldelem (MINT_LDELEM_I4, StackType::I4);
				break;
			case MintType::I8:
				handle_ldelem (MINT_LDELEM_I8, StackType::I8);
				break;
			case MintType::R4:
				handle_ldelem (MINT_LDELEM_R4, StackType::R4);
				break;
			case MintType::R8:
				handle_ldelem (MINT_LDELEM_R8, StackType::R8);
				break;
			case MintType::O:
				handle_ldelem (MINT_LDELEM_REF, StackType::O);
				break;
			case MintType::VT: {
				int size = mono_class_value_size (klass, NULL);
				g_assert (size < G_MAXUINT16);

				CHECK_STACK (2);
				narrow_index (sp - 1);
				interp_add_ins (MINT_LDELEM_VT);
				sp -= 2;
				interp_ins_set_sregs2 (last_ins, sp[0].local, sp[1].local);
				push_type_vt (klass, size);
				interp_ins_set_dreg (last_ins, sp[-1].local);
				last_ins->data[0] = size;
				++ip;
				break;
			}
			default: {
				GString *res = g_string_new ("");
				mono_type_get_desc (res, m_class_get_byval_arg (klass), TRUE);
				g_print ("LDELEM: %s -> %d (%s)\n", m_class_get_name (klass),
				         mint_type (m_class_get_byval_arg (klass)), res->str);
				g_string_free (res, TRUE);
				g_assert (0);
				break;
			}
			}
			ip += 4;
			break;
		}
		case CEE_STELEM_I:
			handle_stelem (MINT_STELEM_I);
			break;
		case CEE_STELEM_I1:
			handle_stelem (MINT_STELEM_I1);
			break;
		case CEE_STELEM_I2:
			handle_stelem (MINT_STELEM_I2);
			break;
		case CEE_STELEM_I4:
			handle_stelem (MINT_STELEM_I4);
			break;
		case CEE_STELEM_I8:
			handle_stelem (MINT_STELEM_I8);
			break;
		case CEE_STELEM_R4:
			handle_stelem (MINT_STELEM_R4);
			break;
		case CEE_STELEM_R8:
			handle_stelem (MINT_STELEM_R8);
			break;
		case CEE_STELEM_REF:
			handle_stelem (MINT_STELEM_REF);
			break;
		case CEE_STELEM: {
			bool from_context = false;
			token = read32 (ip + 1);
			klass = resolve_class (method, token, generic_context, &from_context);
			CHECK_TYPELOAD (klass);
			if (sharing_refusal != nullptr)
				return TRUE;
			// MINT_STELEM_VT copies against the class it is given, and that is
			// the shared form's rather than the instantiation's. Every other
			// arm names the kind of the element alone.
			if (from_context && mint_type (m_class_get_byval_arg (klass)) == MintType::VT) {
				cannot_share ("stelem of a value type the generic context names");
				return TRUE;
			}
			switch (mint_type (m_class_get_byval_arg (klass))) {
			case MintType::I1:
				handle_stelem (MINT_STELEM_I1);
				break;
			case MintType::U1:
				handle_stelem (MINT_STELEM_U1);
				break;
			case MintType::I2:
				handle_stelem (MINT_STELEM_I2);
				break;
			case MintType::U2:
				handle_stelem (MINT_STELEM_U2);
				break;
			case MintType::I4:
				handle_stelem (MINT_STELEM_I4);
				break;
			case MintType::I8:
				handle_stelem (MINT_STELEM_I8);
				break;
			case MintType::R4:
				handle_stelem (MINT_STELEM_R4);
				break;
			case MintType::R8:
				handle_stelem (MINT_STELEM_R8);
				break;
			case MintType::O:
				handle_stelem (MINT_STELEM_REF);
				break;
			case MintType::VT: {
				int size = mono_class_value_size (klass, NULL);
				g_assert (size < G_MAXUINT16);

				handle_stelem (MINT_STELEM_VT);
				last_ins->data[0] = get_data_item_index (klass);
				last_ins->data[1] = size;
				break;
			}
			default: {
				GString *res = g_string_new ("");
				mono_type_get_desc (res, m_class_get_byval_arg (klass), TRUE);
				g_print ("STELEM: %s -> %d (%s)\n", m_class_get_name (klass),
				         mint_type (m_class_get_byval_arg (klass)), res->str);
				g_string_free (res, TRUE);
				g_assert (0);
				break;
			}
			}
			ip += 4;
			break;
		}
		case CEE_CKFINITE: {
			CHECK_STACK (1);
			// ckfinite hands its operand back, so the value keeps the width it
			// arrived with. Reading a single as a double gets four bytes of
			// whatever follows it.
			StackType float_type = sp[-1].type == StackType::R4 ? StackType::R4 : StackType::R8;
			interp_add_ins (float_type == StackType::R4 ? MINT_CKFINITE_R4 : MINT_CKFINITE);
			sp--;
			interp_ins_set_sreg (last_ins, sp[0].local);
			push_simple_type (float_type);
			interp_ins_set_dreg (last_ins, sp[-1].local);
			++ip;
			break;
		}
		case CEE_MKREFANY:
			CHECK_STACK (1);

			token = read32 (ip + 1);
			klass = resolve_class (method, token, generic_context);
			CHECK_TYPELOAD (klass);
			if (sharing_refusal != nullptr)
				return TRUE;

			interp_add_ins (MINT_MKREFANY);
			sp--;
			interp_ins_set_sreg (last_ins, sp[0].local);
			push_type_vt (mono_defaults.typed_reference_class, sizeof (MonoTypedRef));
			interp_ins_set_dreg (last_ins, sp[-1].local);
			last_ins->data[0] = get_data_item_index (klass);

			ip += 5;
			break;
		case CEE_REFANYVAL: {
			CHECK_STACK (1);

			token = read32 (ip + 1);
			klass = resolve_class (method, token, generic_context);
			CHECK_TYPELOAD (klass);
			if (sharing_refusal != nullptr)
				return TRUE;

			interp_add_ins (MINT_REFANYVAL);
			sp--;
			interp_ins_set_sreg (last_ins, sp[0].local);
			push_simple_type (StackType::MP);
			interp_ins_set_dreg (last_ins, sp[-1].local);
			last_ins->data[0] = get_data_item_index (klass);

			ip += 5;
			break;
		}
		case CEE_CONV_OVF_I1:
		case CEE_CONV_OVF_I1_UN: {
			gboolean is_un = *ip == CEE_CONV_OVF_I1_UN;
			CHECK_STACK (1);
			switch (sp[-1].type) {
			case StackType::R4:
				interp_add_conv (sp - 1, NULL, StackType::I4,
				                 is_un ? MINT_CONV_OVF_I1_UN_R4 : MINT_CONV_OVF_I1_R4);
				break;
			case StackType::R8:
				interp_add_conv (sp - 1, NULL, StackType::I4,
				                 is_un ? MINT_CONV_OVF_I1_UN_R8 : MINT_CONV_OVF_I1_R8);
				break;
			case StackType::I4:
				interp_add_conv (sp - 1, NULL, StackType::I4,
				                 is_un ? MINT_CONV_OVF_I1_U4 : MINT_CONV_OVF_I1_I4);
				break;
			case StackType::I8:
				interp_add_conv (sp - 1, NULL, StackType::I4,
				                 is_un ? MINT_CONV_OVF_I1_U8 : MINT_CONV_OVF_I1_I8);
				break;
			default:
				g_assert_not_reached ();
			}
			++ip;
			break;
		}
		case CEE_CONV_OVF_U1:
		case CEE_CONV_OVF_U1_UN:
			CHECK_STACK (1);
			switch (sp[-1].type) {
			case StackType::R4:
				interp_add_conv (sp - 1, NULL, StackType::I4, MINT_CONV_OVF_U1_R4);
				break;
			case StackType::R8:
				interp_add_conv (sp - 1, NULL, StackType::I4, MINT_CONV_OVF_U1_R8);
				break;
			case StackType::I4:
				interp_add_conv (sp - 1, NULL, StackType::I4, MINT_CONV_OVF_U1_I4);
				break;
			case StackType::I8:
				interp_add_conv (sp - 1, NULL, StackType::I4, MINT_CONV_OVF_U1_I8);
				break;
			default:
				g_assert_not_reached ();
			}
			++ip;
			break;
		case CEE_CONV_OVF_I2:
		case CEE_CONV_OVF_I2_UN: {
			gboolean is_un = *ip == CEE_CONV_OVF_I2_UN;
			CHECK_STACK (1);
			switch (sp[-1].type) {
			case StackType::R4:
				interp_add_conv (sp - 1, NULL, StackType::I4,
				                 is_un ? MINT_CONV_OVF_I2_UN_R4 : MINT_CONV_OVF_I2_R4);
				break;
			case StackType::R8:
				interp_add_conv (sp - 1, NULL, StackType::I4,
				                 is_un ? MINT_CONV_OVF_I2_UN_R8 : MINT_CONV_OVF_I2_R8);
				break;
			case StackType::I4:
				interp_add_conv (sp - 1, NULL, StackType::I4,
				                 is_un ? MINT_CONV_OVF_I2_U4 : MINT_CONV_OVF_I2_I4);
				break;
			case StackType::I8:
				interp_add_conv (sp - 1, NULL, StackType::I4,
				                 is_un ? MINT_CONV_OVF_I2_U8 : MINT_CONV_OVF_I2_I8);
				break;
			default:
				g_assert_not_reached ();
			}
			++ip;
			break;
		}
		case CEE_CONV_OVF_U2_UN:
		case CEE_CONV_OVF_U2:
			CHECK_STACK (1);
			switch (sp[-1].type) {
			case StackType::R4:
				interp_add_conv (sp - 1, NULL, StackType::I4, MINT_CONV_OVF_U2_R4);
				break;
			case StackType::R8:
				interp_add_conv (sp - 1, NULL, StackType::I4, MINT_CONV_OVF_U2_R8);
				break;
			case StackType::I4:
				interp_add_conv (sp - 1, NULL, StackType::I4, MINT_CONV_OVF_U2_I4);
				break;
			case StackType::I8:
				interp_add_conv (sp - 1, NULL, StackType::I4, MINT_CONV_OVF_U2_I8);
				break;
			default:
				g_assert_not_reached ();
			}
			++ip;
			break;
#if SIZEOF_VOID_P == 4
		case CEE_CONV_OVF_I:
#endif
		case CEE_CONV_OVF_I4:
		case CEE_CONV_OVF_I4_UN:
			CHECK_STACK (1);
			switch (sp[-1].type) {
			case StackType::R4:
				interp_add_conv (sp - 1, NULL, StackType::I4, MINT_CONV_OVF_I4_R4);
				break;
			case StackType::R8:
				interp_add_conv (sp - 1, NULL, StackType::I4, MINT_CONV_OVF_I4_R8);
				break;
			case StackType::I4:
				if (*ip == CEE_CONV_OVF_I4_UN)
					interp_add_conv (sp - 1, NULL, StackType::I4, MINT_CONV_OVF_I4_U4);
				break;
			case StackType::I8:
				if (*ip == CEE_CONV_OVF_I4_UN)
					interp_add_conv (sp - 1, NULL, StackType::I4, MINT_CONV_OVF_I4_U8);
				else
					interp_add_conv (sp - 1, NULL, StackType::I4, MINT_CONV_OVF_I4_I8);
				break;
			default:
				g_assert_not_reached ();
			}
			++ip;
			break;
#if SIZEOF_VOID_P == 4
		case CEE_CONV_OVF_U:
#endif
		case CEE_CONV_OVF_U4:
		case CEE_CONV_OVF_U4_UN:
			CHECK_STACK (1);
			switch (sp[-1].type) {
			case StackType::R4:
				interp_add_conv (sp - 1, NULL, StackType::I4, MINT_CONV_OVF_U4_R4);
				break;
			case StackType::R8:
				interp_add_conv (sp - 1, NULL, StackType::I4, MINT_CONV_OVF_U4_R8);
				break;
			case StackType::I4:
				if (*ip != CEE_CONV_OVF_U4_UN)
					interp_add_conv (sp - 1, NULL, StackType::I4, MINT_CONV_OVF_U4_I4);
				break;
			case StackType::I8:
				interp_add_conv (sp - 1, NULL, StackType::I4, MINT_CONV_OVF_U4_I8);
				break;
			case StackType::MP:
				interp_add_conv (sp - 1, NULL, StackType::I4, MINT_CONV_OVF_U4_P);
				break;
			default:
				g_assert_not_reached ();
			}
			++ip;
			break;
#if SIZEOF_VOID_P == 8
		case CEE_CONV_OVF_I:
#endif
		case CEE_CONV_OVF_I8:
			CHECK_STACK (1);
			switch (sp[-1].type) {
			case StackType::R4:
				interp_add_conv (sp - 1, NULL, StackType::I8, MINT_CONV_OVF_I8_R4);
				break;
			case StackType::R8:
				interp_add_conv (sp - 1, NULL, StackType::I8, MINT_CONV_OVF_I8_R8);
				break;
			case StackType::I4:
				interp_add_conv (sp - 1, NULL, StackType::I8, MINT_CONV_I8_I4);
				break;
			case StackType::I8:
				break;
			default:
				g_assert_not_reached ();
			}
			++ip;
			break;
#if SIZEOF_VOID_P == 8
		case CEE_CONV_OVF_U:
#endif
		case CEE_CONV_OVF_U8:
			CHECK_STACK (1);
			switch (sp[-1].type) {
			case StackType::R4:
				interp_add_conv (sp - 1, NULL, StackType::I8, MINT_CONV_OVF_U8_R4);
				break;
			case StackType::R8:
				interp_add_conv (sp - 1, NULL, StackType::I8, MINT_CONV_OVF_U8_R8);
				break;
			case StackType::I4:
				interp_add_conv (sp - 1, NULL, StackType::I8, MINT_CONV_OVF_U8_I4);
				break;
			case StackType::I8:
				interp_add_conv (sp - 1, NULL, StackType::I8, MINT_CONV_OVF_U8_I8);
				break;
			default:
				g_assert_not_reached ();
			}
			++ip;
			break;
		case CEE_LDTOKEN: {
			int size;
			gpointer handle;
			token = read32 (ip + 1);
			/*
			 * Every token, rather than only the ones the context names. The
			 * handle below is burned into a data item, and typeof (T) in a
			 * body shared between string and object needs two of them.
			 * Classifying the token instead means resolving it first, which is
			 * what the code below does for each shape it accepts.
			 */
			if (sharing) {
				cannot_share ("ldtoken in a shared body");
				return TRUE;
			}
			if (method->wrapper_type == MONO_WRAPPER_DYNAMIC_METHOD
			    || method->wrapper_type == MONO_WRAPPER_SYNCHRONIZED) {
				handle = mono_method_get_wrapper_data (method, token);
				klass = (MonoClass *) mono_method_get_wrapper_data (method, token + 1);
				if (klass == mono_defaults.typehandle_class)
					handle = m_class_get_byval_arg ((MonoClass *) handle);

				if (generic_context) {
					handle = mono_class_inflate_generic_type_checked ((MonoType *) handle,
					                                                  generic_context, error);
					return_val_if_nok (error, FALSE);
				}
			} else {
				handle = mono_ldtoken_checked (image, token, &klass, generic_context, error);
				return_val_if_nok (error, FALSE);
			}
			mono_class_init_internal (klass);
			mt = mint_type (m_class_get_byval_arg (klass));
			g_assert (mt == MintType::VT);
			size = mono_class_value_size (klass, NULL);
			g_assert (size == sizeof (gpointer));

			const unsigned char *next_ip = ip + 5;
			MonoMethod *cmethod;
			if (next_ip < end && interp_ip_in_cbb (next_ip - il_code)
			    && (*next_ip == CEE_CALL || *next_ip == CEE_CALLVIRT)
			    && (cmethod = interp_get_method (method, read32 (next_ip + 1), image,
			                                     generic_context, error))
			    && (cmethod->klass == mono_defaults.systemtype_class)
			    && (strcmp (cmethod->name, "GetTypeFromHandle") == 0)) {
				const unsigned char *next_next_ip = next_ip + 5;
				MonoMethod *next_cmethod;
				MonoClass *tclass = mono_class_from_mono_type_internal ((MonoType *) handle);
				// Optimize to true/false if next instruction is `call instance bool Type::get_IsValueType()`
				if (next_next_ip < end && interp_ip_in_cbb (next_next_ip - il_code)
				    && (*next_next_ip == CEE_CALL || *next_next_ip == CEE_CALLVIRT)
				    && (next_cmethod = interp_get_method (method, read32 (next_next_ip + 1), image,
				                                          generic_context, error))
				    && (next_cmethod->klass == mono_defaults.systemtype_class)
				    && !strcmp (next_cmethod->name, "get_IsValueType")) {
					g_assert (
						!mono_class_is_open_constructed_type (m_class_get_byval_arg (tclass)));
					if (m_class_is_valuetype (tclass))
						interp_add_ins (MINT_LDC_I4_1);
					else
						interp_add_ins (MINT_LDC_I4_0);
					push_simple_type (StackType::I4);
					interp_ins_set_dreg (last_ins, sp[-1].local);
					ip = next_next_ip + 5;
					break;
				}

				interp_add_ins (MINT_MONO_LDPTR);
				gpointer systype =
					mono_type_get_object_checked (domain, (MonoType *) handle, error);
				return_val_if_nok (error, FALSE);
				push_simple_type (StackType::MP);
				interp_ins_set_dreg (last_ins, sp[-1].local);
				last_ins->data[0] = get_data_item_index (systype);
				ip = next_ip + 5;
			} else {
				interp_add_ins (MINT_LDTOKEN);
				push_type_vt (klass, sizeof (gpointer));
				interp_ins_set_dreg (last_ins, sp[-1].local);
				last_ins->data[0] = get_data_item_index (handle);
				ip += 5;
			}

			break;
		}
		case CEE_ADD_OVF:
			binary_arith_op (MINT_ADD_OVF_I4);
			++ip;
			break;
		case CEE_ADD_OVF_UN:
			binary_arith_op (MINT_ADD_OVF_UN_I4);
			++ip;
			break;
		case CEE_MUL_OVF:
			binary_arith_op (MINT_MUL_OVF_I4);
			++ip;
			break;
		case CEE_MUL_OVF_UN:
			binary_arith_op (MINT_MUL_OVF_UN_I4);
			++ip;
			break;
		case CEE_SUB_OVF:
			binary_arith_op (MINT_SUB_OVF_I4);
			++ip;
			break;
		case CEE_SUB_OVF_UN:
			binary_arith_op (MINT_SUB_OVF_UN_I4);
			++ip;
			break;
		case CEE_ENDFINALLY: {
			g_assert (clause_indexes[in_offset] != -1);
			sp = stack;
			interp_add_ins (MINT_ENDFINALLY);
			last_ins->data[0] = clause_indexes[in_offset];
			link_bblocks = FALSE;
			++ip;
			break;
		}
		case CEE_LEAVE:
		case CEE_LEAVE_S: {
			int target_offset;

			if (*ip == CEE_LEAVE)
				target_offset = 5 + read32 (ip + 1);
			else
				target_offset = 2 + (gint8) ip[1];

			sp = stack;

			for (i = 0; i < header->num_clauses; ++i) {
				MonoExceptionClause *clause = &header->clauses[i];
				if (clause->flags != MONO_EXCEPTION_CLAUSE_FINALLY)
					continue;
				if (MONO_OFFSET_IN_CLAUSE (clause, (ip - header->code))
				    && (!MONO_OFFSET_IN_CLAUSE (clause, (target_offset + in_offset)))) {
					handle_branch (MINT_CALL_HANDLER_S, MINT_CALL_HANDLER,
					               clause->handler_offset - in_offset);
					// FIXME We need new IR to get rid of _S ugliness
					if (last_ins->opcode == MINT_CALL_HANDLER_S)
						last_ins->data[1] = i;
					else
						last_ins->data[2] = i;
				}
			}

			if (clause_indexes[in_offset] != -1) {
				/* LEAVE instructions in catch clauses need to check for abort exceptions */
				handle_branch (MINT_LEAVE_S_CHECK, MINT_LEAVE_CHECK, target_offset);
			} else {
				handle_branch (MINT_LEAVE_S, MINT_LEAVE, target_offset);
			}

			if (*ip == CEE_LEAVE)
				ip += 5;
			else
				ip += 2;
			link_bblocks = FALSE;
			break;
		}
		case MONO_CUSTOM_PREFIX:
			++ip;
			switch (*ip) {
			case CEE_MONO_RETHROW:
				CHECK_STACK (1);
				interp_add_ins (MINT_MONO_RETHROW);
				sp--;
				interp_ins_set_sreg (last_ins, sp[0].local);
				sp = stack;
				++ip;
				break;

			case CEE_MONO_LD_DELEGATE_METHOD_PTR:
				--sp;
				ip += 1;
				interp_add_ins (MINT_LD_DELEGATE_METHOD_PTR);
				interp_ins_set_sreg (last_ins, sp[0].local);
				push_simple_type (StackType::I);
				interp_ins_set_dreg (last_ins, sp[-1].local);
				break;
			case CEE_MONO_CALLI_EXTRA_ARG: {
				int saved_local = sp[-1].local;
				/* Same as CEE_CALLI, except that we drop the extra arg required for llvm specific behaviour */
				sp -= 2;
				StackInfo tos = sp[1];

				// Push back to top of stack and fixup the local offset
				push_types (&tos, 1);
				sp[-1].local = saved_local;
				locals[saved_local].stack_offset = sp[-1].offset;

				if (!interp_transform_call (method, NULL, domain, generic_context, NULL, FALSE,
				                            error, FALSE, FALSE, FALSE))
					return FALSE;
				break;
			}
			case CEE_MONO_JIT_ICALL_ADDR: {
				const guint32 token = read32 (ip + 1);
				ip += 5;
				const gconstpointer func = mono_find_jit_icall_info ((MonoJitICallId) token)->func;

				interp_add_ins (MINT_MONO_LDPTR);
				push_simple_type (StackType::I);
				interp_ins_set_dreg (last_ins, sp[-1].local);
				last_ins->data[0] = get_data_item_index ((gpointer) func);
				break;
			}
			case CEE_MONO_ICALL: {
				int dreg;
				MonoJitICallId const jit_icall_id = (MonoJitICallId) read32 (ip + 1);
				MonoJitICallInfo const *const info = mono_find_jit_icall_info (jit_icall_id);
				ip += 5;

				CHECK_STACK (info->sig->param_count);
				sp -= info->sig->param_count;
				for (int i = 0; i < info->sig->param_count; i++)
					locals[sp[i].local].flags |= INTERP_LOCAL_FLAG_CALL_ARGS;
				if (!MONO_TYPE_IS_VOID (info->sig->ret)) {
					MintType mt = mint_type (info->sig->ret);
					push_simple_type (stack_type_of (mt));
					dreg = sp[-1].local;
					locals[dreg].flags |= INTERP_LOCAL_FLAG_CALL_ARGS;
				} else {
					// Create a new dummy local to serve as the dreg of the call
					// This dreg is only used to resolve the call args offset
					push_simple_type (StackType::I4);
					sp--;
					dreg = sp[0].local;
				}
				if (jit_icall_id == MONO_JIT_ICALL_mono_threads_attach_coop) {
					rtm->needs_thread_attach = 1;
				} else if (jit_icall_id == MONO_JIT_ICALL_mono_threads_detach_coop) {
					g_assert (rtm->needs_thread_attach);
				} else {
					int const icall_op = interp_icall_op_for_sig (info->sig);
					g_assert (icall_op != -1);

					interp_add_ins (icall_op);
					// hash here is overkill
					interp_ins_set_dreg (last_ins, dreg);
					last_ins->data[0] = get_data_item_index ((gpointer) info->func);
				}
				break;
			}
			case CEE_MONO_VTADDR: {
				int size;
				CHECK_STACK (1);
				MonoClass *klass = sp[-1].klass;
				if (method->wrapper_type == MONO_WRAPPER_MANAGED_TO_NATIVE)
					size = mono_class_native_size (klass, NULL);
				else
					size = mono_class_value_size (klass, NULL);

				int local = create_interp_local_explicit (m_class_get_byval_arg (klass), size);
				interp_add_ins (MINT_MOV_VT);
				sp--;
				interp_ins_set_sreg (last_ins, sp[0].local);
				interp_ins_set_dreg (last_ins, local);
				last_ins->data[0] = size;

				interp_add_ins (MINT_LDLOCA_S);
				push_simple_type (StackType::MP);
				interp_ins_set_dreg (last_ins, sp[-1].local);
				interp_ins_set_sreg (last_ins, local);
				locals[local].indirects++;

				++ip;
				break;
			}
			case CEE_MONO_LDPTR:
			case CEE_MONO_CLASSCONST:
			case CEE_MONO_METHODCONST:
				token = read32 (ip + 1);
				ip += 5;
				interp_add_ins (MINT_MONO_LDPTR);
				push_simple_type (StackType::I);
				interp_ins_set_dreg (last_ins, sp[-1].local);
				last_ins->data[0] =
					get_data_item_index (mono_method_get_wrapper_data (method, token));
				break;
			case CEE_MONO_PINVOKE_ADDR_CACHE: {
				token = read32 (ip + 1);
				ip += 5;
				interp_add_ins (MINT_MONO_LDPTR);
				g_assert (method->wrapper_type != MONO_WRAPPER_NONE);
				push_simple_type (StackType::I);
				interp_ins_set_dreg (last_ins, sp[-1].local);
				/* This is a memory slot used by the wrapper */
				gpointer addr = mono_mem_manager_alloc0 (mem_manager, sizeof (gpointer));
				last_ins->data[0] = get_data_item_index (addr);
				break;
			}
			case CEE_MONO_OBJADDR:
				CHECK_STACK (1);
				++ip;
				sp[-1].type = StackType::MP;
				/* do nothing? */
				break;
			case CEE_MONO_NEWOBJ:
				token = read32 (ip + 1);
				ip += 5;
				interp_add_ins (MINT_MONO_NEWOBJ);
				push_simple_type (StackType::O);
				interp_ins_set_dreg (last_ins, sp[-1].local);
				last_ins->data[0] =
					get_data_item_index (mono_method_get_wrapper_data (method, token));
				break;
			case CEE_MONO_RETOBJ:
				CHECK_STACK (1);
				token = read32 (ip + 1);
				ip += 5;
				interp_add_ins (MINT_MONO_RETOBJ);
				sp--;
				interp_ins_set_sreg (last_ins, sp[0].local);
				klass = (MonoClass *) mono_method_get_wrapper_data (method, token);

				/*stackval_from_data (signature->ret, frame->retval, sp->data.vt, signature->pinvoke);*/

				if (sp > stack)
					g_warning ("CEE_MONO_RETOBJ: more values on stack: %d", sp - stack);
				break;
			case CEE_MONO_LDNATIVEOBJ: {
				token = read32 (ip + 1);
				ip += 5;
				klass = (MonoClass *) mono_method_get_wrapper_data (method, token);
				g_assert (m_class_is_valuetype (klass));
				sp--;

				int size = mono_class_native_size (klass, NULL);
				interp_add_ins (MINT_LDOBJ_VT);
				interp_ins_set_sreg (last_ins, sp[0].local);
				push_type_vt (klass, size);
				interp_ins_set_dreg (last_ins, sp[-1].local);
				last_ins->data[0] = size;
				break;
			}
			case CEE_MONO_TLS: {
				gint32 key = read32 (ip + 1);
				ip += 5;
				g_assertf (key == TLS_KEY_SGEN_THREAD_INFO, "%d", key);
				interp_add_ins (MINT_MONO_SGEN_THREAD_INFO);
				push_simple_type (StackType::MP);
				interp_ins_set_dreg (last_ins, sp[-1].local);
				break;
			}
			case CEE_MONO_ATOMIC_STORE_I4:
				CHECK_STACK (2);
				interp_add_ins (MINT_MONO_ATOMIC_STORE_I4);
				sp -= 2;
				interp_ins_set_sregs2 (last_ins, sp[0].local, sp[1].local);
				ip += 2;
				break;
			case CEE_MONO_SAVE_LMF:
			case CEE_MONO_RESTORE_LMF:
			case CEE_MONO_NOT_TAKEN:
				++ip;
				break;
			case CEE_MONO_LDPTR_INT_REQ_FLAG:
				interp_add_ins (MINT_MONO_LDPTR);
				push_type (StackType::MP, NULL);
				interp_ins_set_dreg (last_ins, sp[-1].local);
				last_ins->data[0] = get_data_item_index (&mono_thread_interruption_request_flag);
				++ip;
				break;
			case CEE_MONO_MEMORY_BARRIER:
				interp_add_ins (MINT_MONO_MEMORY_BARRIER);
				++ip;
				break;
			case CEE_MONO_LDDOMAIN:
				interp_add_ins (MINT_MONO_LDDOMAIN);
				push_simple_type (StackType::I);
				interp_ins_set_dreg (last_ins, sp[-1].local);
				++ip;
				break;
			case CEE_MONO_SAVE_LAST_ERROR:
				save_last_error = TRUE;
				++ip;
				break;
			case CEE_MONO_GET_SP:
				interp_add_ins (MINT_MONO_GET_SP);
				push_simple_type (StackType::I);
				interp_ins_set_dreg (last_ins, sp[-1].local);
				++ip;
				break;
			default:
				g_error ("transform.c: Unimplemented opcode: 0xF0 %02x at 0x%x\n", *ip,
				         ip - header->code);
			}
			break;
#if 0
		case CEE_PREFIX7:
		case CEE_PREFIX6:
		case CEE_PREFIX5:
		case CEE_PREFIX4:
		case CEE_PREFIX3:
		case CEE_PREFIX2:
		case CEE_PREFIXREF: ves_abort(); break;
#endif
		/*
		 * Note: Exceptions thrown when executing a prefixed opcode need
		 * to take into account the number of prefix bytes (usually the
		 * throw point is (ip - n_prefix_bytes).
		 */
		case CEE_PREFIX1:
			++ip;
			switch (*ip) {
			case CEE_ARGLIST:
				load_local (arglist_local);
				++ip;
				break;
			case CEE_CEQ:
				CHECK_STACK (2);
				if (sp[-1].type == StackType::O || sp[-1].type == StackType::MP) {
					interp_add_ins (op_for_stack_type (MINT_CEQ_I4, StackType::I));
				} else {
					if (sp[-1].type == StackType::R4 && sp[-2].type == StackType::R8)
						interp_add_conv (sp - 1, NULL, StackType::R8, MINT_CONV_R8_R4);
					if (sp[-1].type == StackType::R8 && sp[-2].type == StackType::R4)
						interp_add_conv (sp - 2, NULL, StackType::R8, MINT_CONV_R8_R4);
					interp_add_ins (op_for_stack_type (MINT_CEQ_I4, sp[-1].type));
				}
				sp -= 2;
				interp_ins_set_sregs2 (last_ins, sp[0].local, sp[1].local);
				push_simple_type (StackType::I4);
				interp_ins_set_dreg (last_ins, sp[-1].local);
				++ip;
				break;
			case CEE_CGT:
				CHECK_STACK (2);
				if (sp[-1].type == StackType::O || sp[-1].type == StackType::MP)
					interp_add_ins (op_for_stack_type (MINT_CGT_I4, StackType::I));
				else
					interp_add_ins (op_for_stack_type (MINT_CGT_I4, sp[-1].type));
				sp -= 2;
				interp_ins_set_sregs2 (last_ins, sp[0].local, sp[1].local);
				push_simple_type (StackType::I4);
				interp_ins_set_dreg (last_ins, sp[-1].local);
				++ip;
				break;
			case CEE_CGT_UN:
				CHECK_STACK (2);
				if (sp[-1].type == StackType::O || sp[-1].type == StackType::MP)
					interp_add_ins (op_for_stack_type (MINT_CGT_UN_I4, StackType::I));
				else
					interp_add_ins (op_for_stack_type (MINT_CGT_UN_I4, sp[-1].type));
				sp -= 2;
				interp_ins_set_sregs2 (last_ins, sp[0].local, sp[1].local);
				push_simple_type (StackType::I4);
				interp_ins_set_dreg (last_ins, sp[-1].local);
				++ip;
				break;
			case CEE_CLT:
				CHECK_STACK (2);
				if (sp[-1].type == StackType::O || sp[-1].type == StackType::MP)
					interp_add_ins (op_for_stack_type (MINT_CLT_I4, StackType::I));
				else
					interp_add_ins (op_for_stack_type (MINT_CLT_I4, sp[-1].type));
				sp -= 2;
				interp_ins_set_sregs2 (last_ins, sp[0].local, sp[1].local);
				push_simple_type (StackType::I4);
				interp_ins_set_dreg (last_ins, sp[-1].local);
				++ip;
				break;
			case CEE_CLT_UN:
				CHECK_STACK (2);
				if (sp[-1].type == StackType::O || sp[-1].type == StackType::MP)
					interp_add_ins (op_for_stack_type (MINT_CLT_UN_I4, StackType::I));
				else
					interp_add_ins (op_for_stack_type (MINT_CLT_UN_I4, sp[-1].type));
				sp -= 2;
				interp_ins_set_sregs2 (last_ins, sp[0].local, sp[1].local);
				push_simple_type (StackType::I4);
				interp_ins_set_dreg (last_ins, sp[-1].local);
				++ip;
				break;
			case CEE_LDVIRTFTN: /* fallthrough */
			case CEE_LDFTN: {
				MonoMethod *m;
				bool from_context = false;
				token = read32 (ip + 1);
				m = interp_get_method (method, token, image, generic_context, error);
				return_val_if_nok (error, FALSE);
				if (sharing && depends_on_context (m)) {
					/*
					 * ldvirtftn settles the method off the receiver, exactly as
					 * a dispatched call does, so it needs nothing fetched. ldftn
					 * names the method itself and does.
					 */
					if (*ip == CEE_LDVIRTFTN) {
						if (!may_dispatch_through_receiver (m))
							return TRUE;
					} else if (inlining) {
						cannot_share ("a method pointer inside an inlined callee");
						return TRUE;
					} else {
						from_context = true;
					}
				}

				if (!mono_method_can_access_method (method, m))
					interp_generate_mae_throw (method, m);

				/*
				 * Only ldftn takes the wrapper here. ldvirtftn resolves the
				 * override at run time and get_virtual_method () wraps what it
				 * found. Handing it the wrapper instead gives it a method that is
				 * not virtual and not in any vtable, so the receiver selects
				 * nothing and the base body is what comes back.
				 */
				if (*ip == CEE_LDFTN && method->wrapper_type == MONO_WRAPPER_NONE
				    && m->iflags & METHOD_IMPL_ATTRIBUTE_SYNCHRONIZED)
					m = mono_marshal_get_synchronized_wrapper (m);

				if (G_UNLIKELY (*ip == CEE_LDFTN && m->wrapper_type == MONO_WRAPPER_NONE
				                && mono_method_has_unmanaged_callers_only_attribute (m))) {
					if (m->flags & METHOD_ATTRIBUTE_PINVOKE_IMPL) {
						interp_generate_not_supported_throw ();
						interp_add_ins (MINT_LDNULL);
						push_simple_type (StackType::MP);
						interp_ins_set_dreg (last_ins, sp[-1].local);
						ip += 5;
						break;
					}

					MonoMethod *ctor_method;

					const unsigned char *next_ip = ip + 5;
					/* check for
					 *    ldftn method_sig
					 *    newobj Delegate::.ctor
					 */
					if (next_ip < end && *next_ip == CEE_NEWOBJ
					    && ((ctor_method = interp_get_method (method, read32 (next_ip + 1), image,
					                                          generic_context, error)))
					    && is_ok (error)
					    && m_class_get_parent (ctor_method->klass)
					           == mono_defaults.multicastdelegate_class
					    && !strcmp (ctor_method->name, ".ctor")) {
						mono_error_set_not_supported (
							error,
							"Cannot create delegate from method with UnmanagedCallersOnlyAttribute");
						return FALSE;
					}

					MonoClass *delegate_klass = NULL;
					MonoGCHandle target_handle = 0;
					ERROR_DECL (wrapper_error);
					m = mono_marshal_get_managed_wrapper (m, delegate_klass, target_handle,
					                                      wrapper_error);
					if (!is_ok (wrapper_error)) {
						/* Generate a call that will throw an exception if the
						 * UnmanagedCallersOnly attribute is used incorrectly */
						interp_generate_ipe_throw_with_msg (wrapper_error);
						mono_interp_error_cleanup (wrapper_error);
						interp_add_ins (MINT_LDNULL);
						push_simple_type (StackType::MP);
						interp_ins_set_dreg (last_ins, sp[-1].local);
					} else {
						/* push a pointer to a trampoline that calls m */
						gpointer entry =
							mini_get_interp_callbacks ()->create_method_pointer (m, TRUE, error);
#if SIZEOF_VOID_P == 8
						interp_add_ins (MINT_LDC_I8);
						WRITE64_INS (last_ins, 0, &entry);
#else
						interp_add_ins (MINT_LDC_I4);
						WRITE32_INS (last_ins, 0, &entry);
#endif
						push_simple_type (StackType::MP);
						interp_ins_set_dreg (last_ins, sp[-1].local);
					}
					ip += 5;
					break;
				}

				/*
				 * A method pointer is the method's entry point in both engines, so
				 * that a delegate built from one - or a calli through it - means the
				 * same thing wherever the frame that produced it ran. Both opcodes
				 * carry the InterpMethod and ask for its entry when they run: the
				 * entry is a trampoline, and creating one for every ldftn site the
				 * program never reaches would be worse than the load.
				 */
				if (from_context) {
					// An rgctx entry holds no wrapper, and the arm above hands
					// a synchronized method one.
					if (m->wrapper_type != MONO_WRAPPER_NONE) {
						cannot_share ("a method pointer a wrapper stands in for");
						return TRUE;
					}

					int callee = emit_rgctx_fetch (MONO_RGCTX_INFO_INTERP_METHOD, m);

					if (sharing_refusal != nullptr)
						return TRUE;

					interp_add_ins (MINT_LDFTN_DYN);
					interp_ins_set_sreg (last_ins, callee);
					push_simple_type (StackType::F);
					interp_ins_set_dreg (last_ins, sp[-1].local);

					ip += 5;
					break;
				}

				int index = get_data_item_index (mono_interp_get_imethod (domain, m, error));
				return_val_if_nok (error, FALSE);
				if (*ip == CEE_LDVIRTFTN) {
					CHECK_STACK (1);
					--sp;
					interp_add_ins (MINT_LDVIRTFTN);
					interp_ins_set_sreg (last_ins, sp[0].local);
					last_ins->data[0] = index;
				} else {
					interp_add_ins (MINT_LDFTN);
					last_ins->data[0] = index;
				}
				push_simple_type (StackType::F);
				interp_ins_set_dreg (last_ins, sp[-1].local);

				ip += 5;
				break;
			}
			case CEE_LDARG: {
				int arg_n = read16 (ip + 1);
				if (!inlining)
					load_arg (arg_n);
				else
					load_local (arg_locals[arg_n]);
				ip += 3;
				break;
			}
			case CEE_LDARGA: {
				int n = read16 (ip + 1);

				if (!inlining) {
					interp_add_ins (MINT_LDLOCA_S);
					interp_ins_set_sreg (last_ins, n);
					locals[n].indirects++;
				} else {
					int loc_n = arg_locals[n];
					interp_add_ins (MINT_LDLOCA_S);
					interp_ins_set_sreg (last_ins, loc_n);
					locals[loc_n].indirects++;
				}
				push_simple_type (StackType::MP);
				interp_ins_set_dreg (last_ins, sp[-1].local);
				ip += 3;
				break;
			}
			case CEE_STARG: {
				int arg_n = read16 (ip + 1);
				if (!inlining)
					store_arg (arg_n);
				else
					store_local (arg_locals[arg_n]);
				ip += 3;
				break;
			}
			case CEE_LDLOC: {
				int loc_n = read16 (ip + 1);
				if (!inlining)
					load_local (num_args + loc_n);
				else
					load_local (local_locals[loc_n]);
				ip += 3;
				break;
			}
			case CEE_LDLOCA: {
				int loc_n = read16 (ip + 1);
				interp_add_ins (MINT_LDLOCA_S);
				if (!inlining)
					loc_n += num_args;
				else
					loc_n = local_locals[loc_n];
				interp_ins_set_sreg (last_ins, loc_n);
				locals[loc_n].indirects++;
				push_simple_type (StackType::MP);
				interp_ins_set_dreg (last_ins, sp[-1].local);
				ip += 3;
				break;
			}
			case CEE_STLOC: {
				int loc_n = read16 (ip + 1);
				if (!inlining)
					store_local (num_args + loc_n);
				else
					store_local (local_locals[loc_n]);
				ip += 3;
				break;
			}
			case CEE_LOCALLOC:
				INLINE_FAILURE;
				CHECK_STACK (1);
#if SIZEOF_VOID_P == 8
				if (sp[-1].type == StackType::I8)
					interp_add_conv (sp - 1, NULL, StackType::I4, MINT_CONV_I4_I8);
#endif
				interp_add_ins (MINT_LOCALLOC);
				if (sp != stack + 1)
					g_warning ("CEE_LOCALLOC: stack not empty");
				sp--;
				interp_ins_set_sreg (last_ins, sp[0].local);
				push_simple_type (StackType::MP);
				interp_ins_set_dreg (last_ins, sp[-1].local);
				has_localloc = TRUE;
				++ip;
				break;
#if 0
			case CEE_UNUSED57: ves_abort(); break;
#endif
			case CEE_ENDFILTER:
				interp_add_ins (MINT_ENDFILTER);
				interp_ins_set_sreg (last_ins, sp[-1].local);
				++ip;
				link_bblocks = FALSE;
				break;
			case CEE_UNALIGNED_:
				ip += 2;
				break;
			case CEE_VOLATILE_:
				++ip;
				volatile_ = TRUE;
				break;
			case CEE_TAIL_:
				++ip;
				tailcall = TRUE;
				// TODO: This should raise a method_tail_call profiler event.
				break;
			case CEE_INITOBJ: {
				bool from_context = false;
				CHECK_STACK (1);
				token = read32 (ip + 1);
				// Both arms below are right for every reference instantiation.
				// One writes a null and names no class, and the other zeroes a
				// value type whose size reference sharing keeps common.
				klass = resolve_class (method, token, generic_context,
				                       inlining ? nullptr : &from_context);
				CHECK_TYPELOAD (klass);
				if (sharing_refusal != nullptr)
					return TRUE;
				if (m_class_is_valuetype (klass)) {
					--sp;
					interp_add_ins (MINT_INITOBJ);
					interp_ins_set_sreg (last_ins, sp[0].local);
					i32 = mono_class_value_size (klass, NULL);
					g_assert (i32 < G_MAXUINT16);
					last_ins->data[0] = i32;
				} else {
					interp_add_ins (MINT_LDNULL);
					push_type (StackType::O, NULL);
					interp_ins_set_dreg (last_ins, sp[-1].local);

					interp_add_ins (MINT_STIND_REF);
					sp -= 2;
					interp_ins_set_sregs2 (last_ins, sp[0].local, sp[1].local);
				}
				ip += 5;
				break;
			}
			case CEE_CPBLK:
				CHECK_STACK (3);
				/* FIX? convert length to I8? */
				if (volatile_)
					interp_add_ins (MINT_MONO_MEMORY_BARRIER);
				interp_add_ins (MINT_CPBLK);
				sp -= 3;
				interp_ins_set_sregs3 (last_ins, sp[0].local, sp[1].local, sp[2].local);
				BARRIER_IF_VOLATILE (MONO_MEMORY_BARRIER_SEQ);
				++ip;
				break;
			case CEE_READONLY_:
				readonly = TRUE;
				ip += 1;
				break;
			case CEE_CONSTRAINED_:
				token = read32 (ip + 1);
				constrained_class = mini_get_class (method, token, generic_context);
				CHECK_TYPELOAD (constrained_class);
				ip += 5;
				break;
			case CEE_INITBLK:
				CHECK_STACK (3);
				BARRIER_IF_VOLATILE (MONO_MEMORY_BARRIER_REL);
				interp_add_ins (MINT_INITBLK);
				sp -= 3;
				interp_ins_set_sregs3 (last_ins, sp[0].local, sp[1].local, sp[2].local);
				ip += 1;
				break;
			case CEE_NO_:
				/* FIXME: implement */
				ip += 2;
				break;
			case CEE_RETHROW: {
				int clause_index = clause_indexes[in_offset];
				g_assert (clause_index != -1);
				interp_add_ins (MINT_RETHROW);
				last_ins->data[0] = rtm->clause_data_offsets[clause_index];
				sp = stack;
				link_bblocks = FALSE;
				++ip;
				break;
			}
			case CEE_SIZEOF: {
				gint32 size;
				token = read32 (ip + 1);
				ip += 5;
				if (mono_metadata_token_table (token) == MONO_TABLE_TYPESPEC
				    && !image_is_dynamic (m_class_get_image (method->klass)) && !generic_context) {
					int align;
					MonoType *type = mono_type_create_from_typespec_checked (image, token, error);
					return_val_if_nok (error, FALSE);
					size = mono_type_size (type, &align);
				} else {
					int align;
					MonoClass *szclass = mini_get_class (method, token, generic_context);
					CHECK_TYPELOAD (szclass);
#if 0
					if (!szclass->valuetype)
						THROW_EX (mono_exception_from_name (mono_defaults.corlib, "System", "InvalidProgramException"), ip - 5);
#endif
					size = mono_type_size (m_class_get_byval_arg (szclass), &align);
				}
				interp_add_ins (MINT_LDC_I4);
				WRITE32_INS (last_ins, 0, &size);
				push_simple_type (StackType::I4);
				interp_ins_set_dreg (last_ins, sp[-1].local);
				break;
			}
			case CEE_REFANYTYPE:
				interp_add_ins (MINT_REFANYTYPE);
				sp--;
				interp_ins_set_sreg (last_ins, sp[0].local);
				push_simple_type (StackType::I);
				interp_ins_set_dreg (last_ins, sp[-1].local);
				++ip;
				break;
			default:
				g_error ("transform.c: Unimplemented opcode: 0xFE %02x (%s) at 0x%x\n", *ip,
				         mono_opcode_name (256 + *ip), ip - header->code);
			}
			break;
		default:
			g_error ("transform.c: Unimplemented opcode: %02x at 0x%x\n", *ip, ip - header->code);
		}
		// No IR instructions were added as part of a bb_start IL instruction. Add a MINT_NOP
		// so we always have an instruction associated with a bb_start. This is simple and avoids
		// any complications associated with il_offset tracking.
		if (!cbb->last_ins)
			interp_add_ins (MINT_NOP);
	}

	g_assert (ip == end);

	if (inlining) {
		// When inlining, all return points branch to this bblock. Code generation inside the caller
		// method continues in this bblock. exit_bb is not necessarily an out bb for cbb. We need to
		// restore stack state so future codegen can work.
		cbb->next_bb = exit_bb;
		cbb = exit_bb;
		if (exit_bb->stack_height >= 0) {
			if (exit_bb->stack_height > 0)
				memcpy (stack, exit_bb->stack_state, exit_bb->stack_height * sizeof (stack[0]));
			sp = stack + exit_bb->stack_height;
		}
	}

	if (sym_seq_points) {
		for (InterpBasicBlock *bb : blocks_from (entry_bb->next_bb)) {
			if (bb->first_ins && bb->in_count > 1 && bb->first_ins->opcode == MINT_SDB_SEQ_POINT)
				interp_insert_ins_bb (bb, NULL, MINT_SDB_INTR_LOC);
		}
	}

	return TRUE;
}

int
TransformData::get_native_offset (int il_offset)
{
	// We can't access offset_to_bb for header->code_size IL offset. Also, offset_to_bb
	// is not set for dead bblocks at method end.
	if (il_offset < header->code_size && offset_to_bb[il_offset]) {
		InterpBasicBlock *bb = offset_to_bb[il_offset];
		g_assert (!bb->dead);
		return bb->native_offset;
	} else {
		return new_code_end - new_code;
	}
}

TransformData::TransformData (MonoMethod *method, MonoMethodHeader *header, InterpMethod *rtm)
	: method (method), header (header), rtm (rtm), has_localloc (false)
{
	code_size = header->code_size;
	max_code_size = code_size;

	in_offsets = g_new0 (int, header->code_size + 1);
	clause_indexes = g_new (int, header->code_size);
	mem_manager = m_method_get_mem_manager (rtm->domain, method);
	gen_sdb_seq_points = mini_debug_options.gen_sdb_seq_points;
	verbose_level = mono_interp_traceopt;
	prof_coverage = mono_profiler_coverage_instrumentation_enabled (method);

	if (prof_coverage)
		coverage_info = mono_profiler_coverage_alloc (rtm->domain, method, header->code_size);

	stack = g_new0 (StackInfo, header->max_stack + 1);
	stack_capacity = header->max_stack + 1;
	sp = stack;
}

TransformData::~TransformData ()
{
	g_free (in_offsets);
	g_free (clause_indexes);
	g_free (stack);
}

/// Records that the body under transform names its generic context, so it
/// cannot serve every reference instantiation.
///
/// The reason is only printed, and what the transform acts on is that a reason
/// exists. It is kept rather than copied, so it must outlive the transform.
/// Every caller passes a literal.
void
TransformData::cannot_share (const char *what)
{
	if (sharing_refusal == nullptr)
		sharing_refusal = what;
}

/*
 * A shared body burns what it resolves into its data items, so a class or a
 * field the generic context names is the point where one instantiation's body
 * stops serving the rest. These two wrap the resolution rather than each site,
 * so a new site is classified by using them.
 */

MonoClass *
TransformData::resolve_class (MonoMethod *method, guint32 token,
                              MonoGenericContext *generic_context, bool *from_context)
{
	MonoClass *resolved = mini_get_class (method, token, generic_context);

	if (!sharing || resolved == nullptr || !depends_on_context (resolved))
		return resolved;

	/*
	 * A caller that can read the class out of the context asks for it here
	 * instead of a refusal. The class resolved above is then the shared form's,
	 * so it says what the site means and never what the instantiation runs
	 * against.
	 */
	if (from_context != nullptr)
		*from_context = true;
	else
		cannot_share ("a class the generic context names");

	return resolved;
}

MonoClassField *
TransformData::resolve_field (MonoMethod *method, guint32 token, MonoClass **klass,
                              MonoGenericContext *generic_context, MonoError *error,
                              bool *from_context)
{
	MonoClassField *field = interp_field_from_token (method, token, klass, generic_context, error);

	if (!sharing || field == nullptr || !depends_on_context (field))
		return field;

	if (from_context != nullptr)
		*from_context = true;
	else
		cannot_share ("a field of a class the generic context names");

	return field;
}

bool
TransformData::may_share_field_access (MonoClass *klass, gboolean is_static, bool inlining)
{
	if (is_static) {
		// The storage is reached through a fetch, and a fetch inside an inlined
		// callee reads the caller's receiver rather than the callee's own.
		if (inlining) {
			cannot_share ("a static field inside an inlined callee");
			return false;
		}

		return true;
	}

#ifndef DISABLE_REMOTING
	if (m_class_get_marshalbyref (klass) || mono_class_is_contextbound (klass)
	    || klass == mono_defaults.marshalbyrefobject_class) {
		cannot_share ("a remoted field of a class the generic context names");
		return false;
	}
#endif

	return true;
}

static void
generate (MonoMethod *method, MonoMethodHeader *header, InterpMethod *rtm,
          MonoGenericContext *generic_context, MonoError *error)
{
	int i;
	TransformData transform_data (method, header, rtm);
	TransformData *td = &transform_data;
	static gboolean verbose_method_inited;
	static char *verbose_method_name;

	if (!verbose_method_inited) {
		verbose_method_name = g_getenv ("MONO_VERBOSE_METHOD");
		verbose_method_inited = TRUE;
	}

	rtm->data_items = NULL;

	td->sharing = mono_method_check_context_used (method) != 0;

	/*
	 * Before the walk, because the clauses are copied from the header rather
	 * than resolved, so no site the walk visits classifies a catch type.
	 * adopt_body () then gives each instantiation that same copy, so a catch
	 * type the context names stays the type variable and matches no exception.
	 *
	 * A catch is the only clause shape that names a type. A filter, a finally
	 * and a fault carry code offsets, and every instantiation runs the same
	 * code. The walk visits the IL of their bodies with the rest of the method.
	 */
	if (td->sharing) {
		for (i = 0; i < header->num_clauses; i++) {
			MonoExceptionClause *clause = &header->clauses[i];

			if (clause->flags == MONO_EXCEPTION_CLAUSE_NONE
			    && clause->data.catch_class != nullptr
			    && depends_on_context (clause->data.catch_class)) {
				td->cannot_share ("a catch of a class the generic context names");
				break;
			}
		}
	}

	if (td->sharing_refusal == nullptr) {
		td->interp_method_compute_offsets (rtm, mono_method_signature_internal (method),
		                                   header, error);
		return_if_nok (error);
	}

	if (verbose_method_name) {
		const char *name = verbose_method_name;

		if ((strchr (name, '.') > name) || strchr (name, ':')) {
			MonoMethodDesc *desc;

			desc = mono_method_desc_new (name, TRUE);
			if (mono_method_desc_full_match (desc, method)) {
				td->verbose_level = 4;
			}
			mono_method_desc_free (desc);
		} else {
			if (strcmp (method->name, name) == 0)
				td->verbose_level = 4;
		}
	}

	if (td->sharing_refusal == nullptr) {
		td->generate_code (method, header, generic_context, error);
		return_if_nok (error);
	}

	if (td->sharing_refusal != nullptr) {
		rtm->sharing_refused = 1;

		if (mono::is_jit_trace_enabled ()) {
			char *name = mono_method_full_name (method, TRUE);

			fprintf (stderr, "[interp] not sharing %s: %s\n", name, td->sharing_refusal);
			g_free (name);
		}

		return;
	}

	g_assert (td->inline_depth == 0);

	if (td->has_localloc)
		td->interp_fix_localloc_ret ();

	td->interp_optimize_code ();

	td->generate_compacted_code ();

	if (td->total_locals_size >= G_MAXUINT16) {
		char *name = mono_method_get_full_name (method);
		char *msg = g_strdup_printf ("Unable to run method '%s': locals size too big.", name);
		g_free (name);
		mono_error_set_generic_error (error, "System", "InvalidProgramException", "%s", msg);
		g_free (msg);
		return;
	}

	if (td->verbose_level) {
		g_print ("Runtime method: %s %p\n", mono_method_full_name (method, TRUE), rtm);
		g_print ("Locals size %d, stack size: %d\n", td->total_locals_size, td->max_stack_size);
		g_print ("Calculated stack height: %d, stated height: %d\n", td->max_stack_height,
		         header->max_stack);
		dump_interp_code (stdout, td->new_code, td->new_code_end);
	}

	if (any_dump_point_enabled ()) {
		std::string dumped = dump_name (method);

		if (dumping (DumpPoint::il, dumped.c_str ())) {
			DumpDestination destination (DumpPoint::il, dumped.c_str ());

			if (destination.stream () != nullptr)
				dump_il (destination.stream (), method, header);
		}

		if (dumping (DumpPoint::mint, dumped.c_str ())) {
			DumpDestination destination (DumpPoint::mint, dumped.c_str ());

			if (destination.stream () != nullptr) {
				fprintf (destination.stream (),
				         "// %s\n// locals %d, stack %d\n", dumped.c_str (),
				         td->total_locals_size, td->max_stack_size);
				dump_interp_code (destination.stream (), td->new_code,
				                  td->new_code_end);
			}
		}
	}

	/* Check if we use excessive stack space */
	if (td->max_stack_height > header->max_stack * 3 && header->max_stack > 16)
		g_warning ("Excessive stack space usage for method %s, %d/%d", method->name,
		           td->max_stack_height, header->max_stack);

	int code_len_u8, code_len_u16;
	code_len_u8 = (guint8 *) td->new_code_end - (guint8 *) td->new_code;
	code_len_u16 = td->new_code_end - td->new_code;

	rtm->clauses = (MonoExceptionClause *) mono_mem_manager_alloc0 (
		td->mem_manager, header->num_clauses * sizeof (MonoExceptionClause));
	memcpy (rtm->clauses, header->clauses, header->num_clauses * sizeof (MonoExceptionClause));
	rtm->code = (gushort *) td->new_code;
	rtm->init_locals = header->init_locals;
	rtm->num_clauses = header->num_clauses;
	for (i = 0; i < header->num_clauses; i++) {
		MonoExceptionClause *c = rtm->clauses + i;
		int end_off = c->try_offset + c->try_len;
		c->try_offset = td->get_native_offset (c->try_offset);
		c->try_len = td->get_native_offset (end_off) - c->try_offset;
		g_assert ((c->try_offset + c->try_len) <= code_len_u16);
		end_off = c->handler_offset + c->handler_len;
		c->handler_offset = td->get_native_offset (c->handler_offset);
		c->handler_len = td->get_native_offset (end_off) - c->handler_offset;
		g_assert (c->handler_len >= 0 && (c->handler_offset + c->handler_len) <= code_len_u16);
		if (c->flags & MONO_EXCEPTION_CLAUSE_FILTER)
			c->data.filter_offset = td->get_native_offset (c->data.filter_offset);
	}
	rtm->stack_size = td->max_stack_size;
	// FIXME revisit whether we actually need this
	rtm->stack_size +=
		2 * MINT_STACK_SLOT_SIZE; /* + 1 for returns of called functions  + 1 for 0-ing in trace*/
	rtm->total_locals_size = ALIGN_TO (td->total_locals_size, MINT_VT_ALIGNMENT);
	rtm->alloca_size = ALIGN_TO (rtm->total_locals_size + rtm->stack_size, 8);
	size_t data_items_size = td->data_items.size () * sizeof (td->data_items[0]);
	rtm->data_items = (gpointer *) mono_mem_manager_alloc0 (td->mem_manager, data_items_size);
	memcpy (rtm->data_items, td->data_items.data (), data_items_size);

	td->interp_save_line_numbers (rtm, td->line_numbers);

	/* Save debug info */
	td->interp_save_debug_info (rtm, header, td->line_numbers);

	/* Create a MonoJitInfo for the interpreted method by creating the interpreter IR as the native code. */
	int jinfo_len;
	jinfo_len = mono_jit_info_size ((MonoJitInfoFlags) 0, header->num_clauses, 0);
	MonoJitInfo *jinfo;
	jinfo = (MonoJitInfo *) mono_mem_manager_alloc0 (td->mem_manager, jinfo_len);
	jinfo->is_interp = 1;
	rtm->jinfo = jinfo;
	mono_jit_info_init (jinfo, method, (guint8 *) rtm->code, code_len_u8, (MonoJitInfoFlags) 0,
	                    header->num_clauses, 0);
	for (i = 0; i < jinfo->num_clauses; ++i) {
		MonoJitExceptionInfo *ei = &jinfo->clauses[i];
		MonoExceptionClause *c = rtm->clauses + i;

		ei->flags = c->flags;
		ei->try_start = (guint8 *) (rtm->code + c->try_offset);
		ei->try_end = (guint8 *) (rtm->code + c->try_offset + c->try_len);
		ei->handler_start = (guint8 *) (rtm->code + c->handler_offset);
		/* Into frame_locals (), not a machine frame, so exvar_base_reg is unused here. */
		ei->exvar_offset = rtm->clause_data_offsets[i];
		if (ei->flags == MONO_EXCEPTION_CLAUSE_FILTER) {
			ei->data.filter = (guint8 *) (rtm->code + c->data.filter_offset);
		} else if (ei->flags == MONO_EXCEPTION_CLAUSE_FINALLY) {
			ei->data.handler_end = (guint8 *) (rtm->code + c->handler_offset + c->handler_len);
		} else {
			ei->data.catch_class = c->data.catch_class;
		}
	}

	td->save_seq_points (jinfo);
}

static mono_mutex_t calc_section;

/*
 * A shared body and its instantiations differ only in metadata. Every reference
 * argument is one pointer, and shared_type () and mint_type () answer a type
 * variable with the constraint the shared form recorded, so the argument
 * offsets, the local offsets and the stack size the transform computed hold for
 * each instantiation.
 *
 * So an instantiation takes the code rather than writing its own. It keeps the
 * fields that name it - its method, its domain, its counter and its own jit
 * info - which is what lets a stack trace, a detour and a promotion request go
 * on naming the instantiation the caller asked for.
 */
static void
adopt_body (InterpMethod *imethod, const InterpMethod *body)
{
	MonoJitInfo *shared_jinfo = body->jinfo;
	int jinfo_len = mono_jit_info_size ((MonoJitInfoFlags) 0, shared_jinfo->num_clauses, 0);
	MonoMemoryManager *mem_manager = m_method_get_mem_manager (imethod->domain, imethod->method);
	MonoJitInfo *jinfo = (MonoJitInfo *) mono_mem_manager_alloc0 (mem_manager, jinfo_len);

	memcpy (jinfo, shared_jinfo, jinfo_len);
	jinfo->d.method = imethod->method;

	imethod->code = body->code;
	imethod->clauses = body->clauses;
	imethod->data_items = body->data_items;
	imethod->local_offsets = body->local_offsets;
	imethod->arg_offsets = body->arg_offsets;
	imethod->clause_data_offsets = body->clause_data_offsets;
	imethod->line_numbers = body->line_numbers;
	imethod->line_numbers_size = body->line_numbers_size;
	imethod->total_locals_size = body->total_locals_size;
	imethod->stack_size = body->stack_size;
	imethod->alloca_size = body->alloca_size;
	imethod->num_clauses = body->num_clauses;
	imethod->init_locals = body->init_locals;
	imethod->needs_thread_attach = body->needs_thread_attach;
	imethod->jinfo = jinfo;
}

/// Returns the transformed body every reference instantiation of method runs,
/// or NULL when this method has to write its own.
///
/// The first instantiation to ask transforms the shared form. The verdict,
/// refusal included, is recorded on the shared form's own record, so the shared
/// IL is read once however many instantiations follow.
static InterpMethod *
shared_body (MonoMethod *method, MonoDomain *domain, MonoError *error)
{
	MonoMethod *shared = shared_form (method);

	error_init (error);

	if (shared == nullptr)
		return nullptr;

	InterpMethod *body = mono_interp_imethod_named (domain, shared, error);

	if (!is_ok (error) || body->sharing_refused)
		return nullptr;

	if (body->transformed)
		return body;

	/*
	 * A shared form that will not transform is a refusal rather than a
	 * failure, because the instantiation writes its own body and runs. Only
	 * the shared form was asked about, so its error says nothing about the
	 * method the caller wants.
	 */
	MonoMethodHeader *header = mono_method_get_header_checked (shared, error);

	if (!is_ok (error)) {
		mono_error_cleanup (error);
		error_init (error);
		body->sharing_refused = 1;
		return nullptr;
	}

	/*
	 * Into a copy, the way mono_interp_transform_method () does, so a thread
	 * that reads transformed finds every field already filled in. The open
	 * check and the class initializer that function runs first are the
	 * instantiation's, and the shared form answers neither.
	 */
	InterpMethod tmp = *body;

	generate (shared, header, &tmp, mono_method_get_context (shared), error);
	mono_metadata_free_mh (header);

	if (!is_ok (error)) {
		mono_error_cleanup (error);
		error_init (error);
		body->sharing_refused = 1;
		return nullptr;
	}

	mono_os_mutex_lock (&calc_section);
	if (!body->transformed && !body->sharing_refused) {
		// The method is the same one, so the copy starts after it.
		const int start_offset = sizeof (gpointer);

		memcpy ((char *) body + start_offset, (char *) &tmp + start_offset,
		        sizeof (InterpMethod) - start_offset);
		mono_memory_barrier ();
		body->transformed = tmp.sharing_refused ? FALSE : TRUE;
	}
	mono_os_mutex_unlock (&calc_section);

	return body->transformed ? body : nullptr;
}

} // namespace mono::interp

/* Outside the namespace, because interp-internals.hpp and transform.hpp
 * declare them there. */

using namespace mono::interp;

/*
 * Why do_jit_call () cannot marshal a call to method, or NULL when it can.
 *
 * These are limits of that marshalling - what the gsharedvt_out wrapper can be
 * built for and what the interpreter's stack can be handed across - rather than
 * a policy about when calling natively is worth it. A method that is refused
 * here stays interpreted however thoroughly it has been compiled.
 */
static const char *
interp_jit_call_refusal (MonoMethod *method, MonoMethodSignature *sig)
{
	/* jit_call_cb () spells out the call for each argument count and stops at
	 * eight. The receiver and the return address take two of those places. */
	if (sig->param_count > 6)
		return "the callee takes too many arguments";
	/* The callee's own signature says nothing about the arguments a vararg
	 * call site actually pushed, so the wrapper cannot be built for one. */
	if (sig->call_convention == MONO_CALL_VARARG)
		return "the callee is vararg";
	if (sig->pinvoke)
		return "the signature is a p/invoke one";
	if (method->flags & METHOD_ATTRIBUTE_PINVOKE_IMPL)
		return "the callee is a p/invoke";
	if (method->iflags & METHOD_IMPL_ATTRIBUTE_INTERNAL_CALL)
		return "the callee is an internal call";
	/*
	 * An instantiation is called like any other method: its entry supplies
	 * whatever context the body behind it reads, because the backend puts a
	 * context stub in front of a shared body rather than asking the caller
	 * for one. A method that still names a type parameter has no entry of
	 * its own to call, and its signature names the parameter too, so the
	 * wrapper would have to be built for a type with no layout yet.
	 */
	if (mono_method_check_context_used (method) != 0)
		return "the callee still depends on type parameters";
	if (method->string_ctor)
		return "the callee is a string constructor";
	if (method->wrapper_type != MONO_WRAPPER_NONE)
		return "the callee is a wrapper";

	return NULL;
}

gboolean
mono_interp_jit_call_marshallable (MonoMethod *method, MonoMethodSignature *sig)
{
	const char *refusal = interp_jit_call_refusal (method, sig);

	/*
	 * A refused method is interpreted, which is what it was doing anyway, so
	 * this trace is the only record that the decision was taken. The tiered
	 * mask rather than the transform's own verbose level: --interp=verbose=
	 * also makes the interpreter the whole engine, and a refusal only matters
	 * while there is a compiled tier to refuse.
	 */
	if (refusal != NULL && MONO_TRACE_IS_TRACED (G_LOG_LEVEL_DEBUG, MONO_TRACE_TIERED)) {
		char *name = mono_method_full_name (method, TRUE);

		mono_trace (G_LOG_LEVEL_DEBUG, MONO_TRACE_TIERED, "Decline jit call to %s: %s",
		            name, refusal);
		g_free (name);
	}

	return refusal == NULL;
}

gboolean
mono_interp_jit_call_supported (MonoMethod *method, MonoMethodSignature *sig)
{
	if (!mono_interp_jit_call_marshallable (method, sig))
		return FALSE;

	if (mono_aot_only && m_class_get_image (method->klass)->aot_module
	    && !(method->iflags & METHOD_IMPL_ATTRIBUTE_SYNCHRONIZED)) {
		ERROR_DECL (error);
		gpointer addr = mono_jit_compile_method_jit_only (method, error);
		if (addr && is_ok (error))
			return TRUE;
	}

	// FIXME: Namespaces
	for (const std::string &class_name : mono_interp_jit_classes) {
		if (class_name == m_class_get_name (method->klass))
			return TRUE;
	}

	//return TRUE;
	return FALSE;
}

void
mono_interp_dis_mintop (FILE *out, const guint16 *ip, const guint16 *start)
{
	int opcode = *ip;
	int ins_offset = ip - start;

	fprintf (out, "IR_%04x: %-14s", ins_offset, opname (opcode));
	ip++;

	if (num_dregs (opcode) == MINT_CALL_ARGS)
		fprintf (out, " [call_args %d <-", *ip++);
	else if (num_dregs (opcode) > 0)
		fprintf (out, " [%d <-", *ip++);
	else
		fprintf (out, " [nil <-");

	if (num_sregs (opcode) > 0) {
		for (int i = 0; i < num_sregs (opcode); i++)
			fprintf (out, " %d", *ip++);
		fprintf (out, "],");
	} else {
		fprintf (out, " nil],");
	}
	char *ins = dump_interp_ins_data (NULL, ins_offset, ip, opcode);
	fprintf (out, "%s\n", ins);
	g_free (ins);
}

/* For debug use */
void
mono_interp_print_code (InterpMethod *imethod)
{
	MonoJitInfo *jinfo = imethod->jinfo;
	const guint8 *start;

	if (!jinfo)
		return;

	char *name = mono_method_full_name (imethod->method, 1);
	g_print ("Method : %s\n", name);
	g_free (name);

	start = (guint8 *) jinfo->code_start;
	dump_interp_code (stdout, (const guint16 *) start,
	                  (const guint16 *) (start + jinfo->code_size));
}

/* For debug use */
void
mono_interp_print_td_code (TransformData *td)
{
	char *name = mono_method_full_name (td->method, TRUE);
	g_print ("IR for \"%s\"\n", name);
	g_free (name);

	for (InterpBasicBlock *bb : blocks_from (td->entry_bb))
		for (InterpInst *ins : *bb)
			dump_interp_inst (ins);
}

void
mono_test_interp_method_compute_offsets (TransformData *td, InterpMethod *imethod,
                                         MonoMethodSignature *signature, MonoMethodHeader *header)
{
	ERROR_DECL (error);
	td->interp_method_compute_offsets (imethod, signature, header, error);
}

void
mono_test_interp_cprop (TransformData *td)
{
	td->interp_cprop ();
}

gboolean
mono_test_interp_generate_code (TransformData *td, MonoMethod *method, MonoMethodHeader *header,
                                MonoGenericContext *generic_context, MonoError *error)
{
	return td->generate_code (method, header, generic_context, error);
}

void
mono_interp_transform_init (void)
{
	mono_os_mutex_init_recursive (&calc_section);
}

void
mono_interp_transform_method (InterpMethod *imethod, ThreadContext *context, MonoError *error)
{
	MonoMethod *method = imethod->method;
	MonoMethodHeader *header = NULL;
	MonoMethodSignature *signature = mono_method_signature_internal (method);
	MonoVTable *method_class_vt;
	MonoGenericContext *generic_context = NULL;
	MonoDomain *domain = imethod->domain;
	InterpMethod tmp_imethod;
	InterpMethod *real_imethod;

	error_init (error);

#ifdef ENABLE_METADATA_UPDATE
	mono_metadata_update_thread_expose_published ();
#endif

	if (mono_class_is_open_constructed_type (m_class_get_byval_arg (method->klass))) {
		mono_error_set_invalid_operation (
			error, "%s",
			"Could not execute the method because the containing type is not fully instantiated.");
		return;
	}

	/*
	 * Here rather than where the backend is asked for a method, because the
	 * interpreter reaches a callee without asking. Transform runs once per
	 * method, so a body gets its verdict before its first instruction either
	 * way, and before the class initializer below runs on its behalf.
	 */
	if (!mono_llvm_jit_verify_method (method, error))
		return;

	// g_printerr ("TRANSFORM(0x%016lx): begin %s::%s\n", mono_thread_current (), method->klass->name, method->name);
	method_class_vt = mono_class_vtable_checked (domain, imethod->method->klass, error);
	return_if_nok (error);

	if (!method_class_vt->initialized) {
		mono_runtime_class_init_full (method_class_vt, error);
		return_if_nok (error);
	}

	MONO_PROFILER_RAISE (jit_begin, (method));

	if (InterpMethod *body = shared_body (method, domain, error)) {
		mono_os_mutex_lock (&calc_section);
		if (!imethod->transformed) {
			adopt_body (imethod, body);
			mono_memory_barrier ();
			imethod->transformed = TRUE;
			mono_interp_stats.methods_transformed++;
		}
		mono_os_mutex_unlock (&calc_section);

		MONO_PROFILER_RAISE (jit_done, (method, imethod->jinfo));
		return;
	}

	return_if_nok (error);

	if (mono_method_signature_internal (method)->is_inflated)
		generic_context = mono_method_get_context (method);
	else {
		MonoGenericContainer *generic_container = mono_method_get_generic_container (method);
		if (generic_container)
			generic_context = &generic_container->context;
	}

	if (method->iflags & (METHOD_IMPL_ATTRIBUTE_INTERNAL_CALL | METHOD_IMPL_ATTRIBUTE_RUNTIME)) {
		MonoMethod *nm = NULL;
		if (imethod->transformed) {
			MONO_PROFILER_RAISE (jit_done, (method, imethod->jinfo));
			return;
		}

		/* assumes all internal calls with an array this are built in... */
		if (method->iflags & METHOD_IMPL_ATTRIBUTE_INTERNAL_CALL
		    && (!mono_method_signature_internal (method)->hasthis
		        || m_class_get_rank (method->klass) == 0)) {
			nm = mono_marshal_get_native_wrapper (method, FALSE, FALSE);
			signature = mono_method_signature_internal (nm);
		} else {
			const char *name = method->name;
			if (m_class_get_parent (method->klass) == mono_defaults.multicastdelegate_class) {
				if (*name == '.' && (strcmp (name, ".ctor") == 0)) {
					MonoJitICallInfo *mi =
						&mono_get_jit_icall_info ()->ves_icall_mono_delegate_ctor;
					nm = mono_marshal_get_icall_wrapper (mi, TRUE);
				} else if (*name == 'I' && (strcmp (name, "Invoke") == 0)) {
					/*
					 * Usually handled during transformation of the caller, but
					 * when the caller is handled by another execution engine
					 * (for example fullAOT) we need to handle it here. That's
					 * known to be wrong in cases where the reference to
					 * `MonoDelegate` would be needed (FIXME).
					 */
					nm = mono_marshal_get_delegate_invoke (method, NULL);
				} else if (*name == 'B' && (strcmp (name, "BeginInvoke") == 0)) {
					nm = mono_marshal_get_delegate_begin_invoke (method);
				} else if (*name == 'E' && (strcmp (name, "EndInvoke") == 0)) {
					nm = mono_marshal_get_delegate_end_invoke (method);
				}
			}
			if (nm == NULL)
				g_assert_not_reached ();
		}
		if (nm == NULL) {
			mono_os_mutex_lock (&calc_section);
			imethod->stack_size = sizeof (stackval); /* for tracing */
			imethod->alloca_size = imethod->stack_size;
			mono_memory_barrier ();
			imethod->transformed = TRUE;
			mono_interp_stats.methods_transformed++;
			mono_os_mutex_unlock (&calc_section);
			MONO_PROFILER_RAISE (jit_done, (method, NULL));
			return;
		}
		method = nm;
		header = interp_method_get_header (nm, error);
		return_if_nok (error);
	}

	if (!header) {
		header = mono_method_get_header_checked (method, error);
		return_if_nok (error);
	}

	g_assert ((signature->param_count + signature->hasthis) < 1000);
	// g_printerr ("TRANSFORM(0x%016lx): end %s::%s\n", mono_thread_current (), method->klass->name, method->name);

	/* Make modifications to a copy of imethod, copy them back inside the lock */
	real_imethod = imethod;
	memcpy (&tmp_imethod, imethod, sizeof (InterpMethod));
	imethod = &tmp_imethod;

	MONO_TIME_TRACK (mono_interp_stats.transform_time,
	                 generate (method, header, imethod, generic_context, error));

	mono_metadata_free_mh (header);

	return_if_nok (error);

	/* Copy changes back */
	imethod = real_imethod;
	/*
	 * A thread that reads transformed runs the bytecode immediately, so any
	 * breakpoint the debugger already asked for must be in that bytecode before
	 * the flag goes up. The jit_done event below is too late: the method is
	 * reachable by then, and a thread that enters in between runs the method to
	 * the end without stopping.
	 *
	 * The loader lock is outside calc_section because installing a breakpoint
	 * takes it. It is recursive, so a caller that holds it already is safe.
	 *
	 * Only a body that carries its own sequence points is offered. Without them
	 * the installer looks for the method's table, which takes the domain lock -
	 * and the domain lock is outer to calc_section, so asking for it here
	 * inverts the order. That table also describes some other body than this
	 * one, so the installer has nothing to install from.
	 */
	mono_loader_lock ();
	mono_os_mutex_lock (&calc_section);
	if (!imethod->transformed) {
		// The method is the same one, so the copy starts after it.
		const int start_offset = sizeof (gpointer);
		memcpy ((char *) imethod + start_offset, (char *) &tmp_imethod + start_offset,
		        sizeof (InterpMethod) - start_offset);
		if (imethod->jinfo->seq_points)
			mini_install_pending_breakpoints (domain, method, imethod->jinfo);
		mono_memory_barrier ();
		imethod->transformed = TRUE;
		mono_interp_stats.methods_transformed++;
		mono_atomic_fetch_add_i32 (&mono_jit_stats.methods_with_interp, 1);
	}
	mono_os_mutex_unlock (&calc_section);
	mono_loader_unlock ();

	mono_domain_lock (domain);
	if (mono_stats_method_desc
	    && mono_method_desc_full_match (mono_stats_method_desc, imethod->method)) {
		g_printf ("Printing runtime stats at method: %s\n",
		          mono_method_get_full_name (imethod->method));
		mono_runtime_print_stats ();
	}
	/*
	 * A method's entry here is the list of tables its bodies published, and the
	 * domain frees it as a list. So this publishes a node, not the table. The
	 * same method appends its compiled body's table to this list when it is
	 * promoted out of the interpreter.
	 */
	if (imethod->jinfo->seq_points
	    && !g_hash_table_lookup (domain_jit_info (domain)->seq_points, imethod->method))
		g_hash_table_insert (domain_jit_info (domain)->seq_points, imethod->method,
		                     g_slist_append (NULL, imethod->jinfo->seq_points));
	mono_domain_unlock (domain);

	// FIXME: Add a different callback ?
	MONO_PROFILER_RAISE (jit_done, (method, imethod->jinfo));
}

/**
 * \file
 * \brief Publishing a compiled method's MonoJitInfo from its side tables.
 *
 * This file reads back three of the sections the compiler wrote next to the
 * code.
 *
 * What comes out is the MonoJitInfo mono_handle_exception and the stack walks
 * search for: from_llvm, so dispatch re-enters frames through the landing pads
 * with the exception and clause index in the registers a pad reads.
 */

#include "jinfo.hpp"

#include "arch/arch.hpp"
#include "sidetables.hpp"

#include "mono_lsda.hpp"

#include "mini.h"
#include "mini-unwind.h"
#include "mini-runtime.h"

#include "mono/metadata/class-inlines.h"
#include "mono/metadata/class-internals.h"
#include "mono/metadata/debug-internals.h"
#include "mono/metadata/domain-internals.h"
#include "mono/metadata/mono-debug.h"

#include "seq-point-marker.hpp"

/* A C header with no linkage guard of its own. */
extern "C" {
#include "mono/metadata/seq-points-data.h"
}

#include <llvm/Support/Error.h>

#include <algorithm>
#include <cstring>
#include <map>
#include <vector>

using namespace llvm;

namespace mono {
namespace {

/// The finally guard records, or an error if the section is malformed or names a
/// slot a stack walk cannot reach.
///
/// A null section is not a failure: a method with no finally, or one whose bodies
/// all optimized away, has nothing for a thread to be stopped inside. Neither is
/// a section that holds only other functions' blocks.
Expected<std::vector<MonoFinallyGuard>>
parse_guards (const uint8_t *section, size_t size, const uint8_t *code,
              uint32_t code_size)
{
	std::vector<MonoFinallyGuard> guards;

	if (section == nullptr || size == 0)
		return guards;

	const uint8_t *end = section + size;
	const uint8_t *block = section;
	uint32_t count = 0;

	for (;;) {
		if ((size_t) (end - block) < guards_header_size
		    || read_le<uint32_t> (block) != guards_section_magic
		    || read_le<uint16_t> (block + 4) != guards_section_version)
			return createStringError (inconvertibleErrorCode (),
			                          "the compiled object's finally-guard table "
			                          "is not one of ours");

		count = read_le<uint16_t> (block + 6);

		const uint8_t *records = block + guards_header_size;
		size_t bytes = (size_t) count * guards_record_size;

		if ((size_t) (end - records) < bytes)
			return createStringError (inconvertibleErrorCode (),
			                          "the finally-guard table's length does not "
			                          "match its %u records", count);

		if ((const uint8_t *) (uintptr_t) read_le<uint64_t> (block + 8) == code)
			break;

		block = records + bytes;
		if (block == end)
			return guards;
	}

	const uint8_t *p = block + guards_header_size;

	for (uint32_t i = 0; i < count; ++i, p += guards_record_size) {
		MonoFinallyGuard g;

		g.clause_index = read_le<uint32_t> (p);
		g.handler_start_off = read_le<uint32_t> (p + 4);
		g.handler_end_off = read_le<uint32_t> (p + 8);
		g.exvar_offset = read_le<int32_t> (p + 12);

		int dwarf_reg = read_le<int32_t> (p + 16);

		/*
		 * A run can bracket no code at all - the two markers end up in the same
		 * place once what was between them has folded away - and there is then
		 * no PC for a thread to be stopped at. The writer cannot tell, since it
		 * emits label differences the assembler folds later.
		 */
		if (g.handler_start_off == g.handler_end_off)
			continue;

		if (g.handler_start_off > g.handler_end_off || g.handler_end_off > code_size)
			return createStringError (inconvertibleErrorCode (),
			                          "finally body range [%u, %u) is not inside "
			                          "the method's %u bytes of code",
			                          g.handler_start_off, g.handler_end_off,
			                          code_size);

		if (!mono_dwarf_reg_is_valid (dwarf_reg))
			return createStringError (inconvertibleErrorCode (),
			                          "finally guard register %d has no mono "
			                          "mapping", dwarf_reg);

		int hw_reg = mono_dwarf_reg_to_hw_reg (dwarf_reg);

		if (!arch::reg_is_recoverable (hw_reg))
			return createStringError (inconvertibleErrorCode (),
			                          "the finally guard slot is homed against a "
			                          "register a stack walk cannot rebuild");

		g.exvar_base_reg = (uint8_t) hw_reg;
		guards.push_back (g);
	}

	return guards;
}

} // namespace

Expected<GSList *>
transcode_unwind (const std::vector<UnwindRecord> &records)
{
	GSList *ops = nullptr;
	int remember_depth = 0;

	auto fail = [&ops] (Error err) -> Error {
		mono_free_unwind_info (ops);
		return err;
	};

	auto emit = [&ops] (uint32_t when, uint8_t op, uint16_t hwreg, int32_t val) {
		MonoUnwindOp *out = g_new0 (MonoUnwindOp, 1);

		out->op = op;
		out->reg = hwreg;
		out->val = val;
		out->when = when;
		ops = g_slist_append (ops, out);
	};

	auto hw_reg = [] (int32_t dwarf_reg) -> int {
		if (!mono_dwarf_reg_is_valid (dwarf_reg))
			return -1;
		return mono_dwarf_reg_to_hw_reg (dwarf_reg);
	};

	std::vector<std::pair<int32_t, int64_t>> entry_offsets;
	bool in_entry_state = true;

	/*
	 * The CFA offset in effect, shadowed so that a relative adjustment can be
	 * turned into the absolute one mono needs. The entry records open with a
	 * def_cfa, so this is set before any adjustment can read it.
	 */
	int64_t cfa_offset = 0;
	int64_t remembered_cfa_offset = 0;

	for (const UnwindRecord &r : records) {
		if (r.offset != 0)
			in_entry_state = false;

		switch (r.op) {
		case MONO_UNWIND_OP_DEF_CFA: {
			int reg = hw_reg (r.reg);

			if (reg < 0)
				return fail (createStringError (
					inconvertibleErrorCode (),
					"cfa register %d has no mono mapping", r.reg));
			cfa_offset = r.value;
			emit (r.offset, DW_CFA_def_cfa, (uint16_t) reg,
			      (int32_t) r.value);
			break;
		}
		case MONO_UNWIND_OP_DEF_CFA_OFFSET:
			cfa_offset = r.value;
			emit (r.offset, DW_CFA_def_cfa_offset, 0, (int32_t) r.value);
			break;
		case MONO_UNWIND_OP_ADJUST_CFA_OFFSET:
			/*
			 * ADJUST_CFA_OFFSET moves the CFA offset by a relative amount.
			 * Mono only takes an absolute one, so this replays it as the
			 * offset it arrives at.
			 */
			cfa_offset += r.value;
			emit (r.offset, DW_CFA_def_cfa_offset, 0, (int32_t) cfa_offset);
			break;
		/*
		 * DWARF carries the argument bytes for a personality routine that
		 * adjusts the stack as it unwinds. Mono's unwinder has no opcode
		 * for it and never calls the personality routine. It sets no CFA
		 * and no register rule, and the CFA movement around the call
		 * arrives as its own record, so the description stays complete
		 * without it.
		 */
		case MONO_UNWIND_OP_ARGS_SIZE:
			break;
		case MONO_UNWIND_OP_DEF_CFA_REGISTER: {
			int reg = hw_reg (r.reg);

			if (reg < 0)
				return fail (createStringError (
					inconvertibleErrorCode (),
					"cfa register %d has no mono mapping", r.reg));
			emit (r.offset, DW_CFA_def_cfa_register, (uint16_t) reg, 0);
			break;
		}
		case MONO_UNWIND_OP_OFFSET: {
			int reg = hw_reg (r.reg);

			if (reg < 0)
				return fail (createStringError (
					inconvertibleErrorCode (),
					"saved register %d has no mono mapping", r.reg));
			emit (r.offset, DW_CFA_offset, (uint16_t) reg,
			      (int32_t) r.value);
			if (in_entry_state)
				entry_offsets.emplace_back (r.reg, r.value);
			break;
		}
		case MONO_UNWIND_OP_REMEMBER_STATE:
			/*
			 * mono's unwinder holds exactly one remembered state and
			 * fails the frame when a second arrives. We refuse a deeper
			 * nest here and decline the method instead.
			 */
			if (++remember_depth > 1)
				return fail (createStringError (
					inconvertibleErrorCode (),
					"remember_state nested deeper than mono supports"));
			remembered_cfa_offset = cfa_offset;
			emit (r.offset, DW_CFA_remember_state, 0, 0);
			break;
		case MONO_UNWIND_OP_RESTORE_STATE:
			if (--remember_depth < 0)
				return fail (createStringError (
					inconvertibleErrorCode (),
					"restore_state without a remembered state"));
			cfa_offset = remembered_cfa_offset;
			emit (r.offset, DW_CFA_restore_state, 0, 0);
			break;
		/*
		 * RESTORE reverts a register to its rule at function entry, which
		 * mono has no opcode for. It is replayed here as that entry rule
		 * instead: an offset where the entry saved the register, and
		 * same_value where it did not. The entry state is the leading run of
		 * offset-zero records.
		 */
		case MONO_UNWIND_OP_RESTORE:
		case MONO_UNWIND_OP_SAME_VALUE: {
			int reg = hw_reg (r.reg);

			if (reg < 0)
				return fail (createStringError (
					inconvertibleErrorCode (),
					"restored register %d has no mono mapping", r.reg));

			const std::pair<int32_t, int64_t> *entry_rule = nullptr;

			if (r.op == MONO_UNWIND_OP_RESTORE)
				for (const auto &rule : entry_offsets)
					if (rule.first == r.reg)
						entry_rule = &rule;

			if (entry_rule != nullptr)
				emit (r.offset, DW_CFA_offset, (uint16_t) reg,
				      (int32_t) entry_rule->second);
			else
				emit (r.offset, DW_CFA_same_value, (uint16_t) reg, 0);
			break;
		}
		default:
			return fail (createStringError (
				inconvertibleErrorCode (),
				"frame description op %d (llvm %d) is not expressible",
				r.op, (int) r.value));
		}
	}

	if (remember_depth != 0)
		return fail (createStringError (inconvertibleErrorCode (),
		                                "unbalanced remember_state"));

	return ops;
}

/*
 * Publish method's sequence points: the table the soft debugger looks an IL
 * offset up in to place a breakpoint, and looks a native offset up in to say
 * where a stopped thread is.
 *
 * The rows come out of the line table in the encoding seq-point-marker.hpp
 * describes and are already ascending by native offset, which is what
 * mono_seq_point_find_prev_by_native_offset () walks. Native offsets are a
 * property of the body, so every body gets a table of its own. The domain
 * keeps them in a list under the method, which is both who frees them and
 * where the per-method lookup - which can only ever answer for one body -
 * reads from.
 *
 * graph says which sequence points can follow which, as IL offsets. What goes
 * into the table are indices into the table itself, so it is joined against
 * the rows on the way past.
 */
static void
publish_seq_points (MonoDomain *domain, MonoMethod *method, MonoJitInfo *jinfo,
                    const std::vector<IlLineRow> &rows,
                    const SeqPointGraph &graph)
{
	/*
	 * With debug data every row is published, so a row's position here is the
	 * index mono_seq_point_init_next () will resolve a successor by. The two
	 * markers are left out: they are not IL offsets and nothing names them.
	 */
	std::map<uint32_t, unsigned> index_of;

	for (size_t i = 0; i < rows.size (); ++i)
		if (rows[i].il_offset < SEQ_POINT_ENCODED_ENTRY)
			index_of.emplace (rows[i].il_offset, (unsigned) i);

	GByteArray *array = g_byte_array_new ();
	SeqPoint last = { 0, 0, 0, 0, 0 };
	int len = 0;

	for (const IlLineRow &row : rows) {
		SeqPoint sp;
		GSList *next = NULL;

		memset (&sp, 0, sizeof (sp));
		sp.native_offset = (int) row.native_offset;
		sp.flags = row.flags;

		if (row.il_offset == SEQ_POINT_ENCODED_ENTRY)
			sp.il_offset = METHOD_ENTRY_IL_OFFSET;
		else if (row.il_offset == SEQ_POINT_ENCODED_EXIT)
			sp.il_offset = METHOD_EXIT_IL_OFFSET;
		else
			sp.il_offset = (int) row.il_offset;

		auto successors = graph.find (row.il_offset);

		if (successors != graph.end ())
			for (uint32_t offset : successors->second) {
				auto index = index_of.find (offset);

				if (index != index_of.end ())
					next = g_slist_prepend (
						next, GUINT_TO_POINTER (index->second));
			}

		if (mono_seq_point_info_add_seq_point (array, &sp, &last, next, TRUE)) {
			last = sp;
			len ++;
		}

		g_slist_free (next);
	}

	int size = 0;
	MonoSeqPointInfo *info =
		mono_seq_point_info_new (array->len, TRUE, array->data, TRUE, &size);

	g_byte_array_free (array, TRUE);

	if (len == 0) {
		mono_seq_point_info_free (info);
		return;
	}

	/*
	 * Appended rather than prepended: the list head is what the per-method
	 * lookup returns, and that has always been the method's first body.
	 * Appending in place leaves the head where it is, so the hash entry - and
	 * with it the domain's ownership of every table on the list - does not
	 * have to be replaced.
	 */
	mono_domain_lock (domain);

	GSList *published = (GSList *) g_hash_table_lookup (
		domain_jit_info (domain)->seq_points, method);

	if (published == NULL)
		g_hash_table_insert (domain_jit_info (domain)->seq_points, method,
		                     g_slist_append (NULL, info));
	else
		g_slist_append (published, info);
	mono_domain_unlock (domain);

	jinfo->seq_points = info;
}

/*
 * Fill in var from slot, or say the slot is not one a stack walk can address.
 *
 * The translator pinned the variable to a frame slot for exactly this, so
 * there is one home for the whole method and REGOFFSET can say where it is.
 * Which register the slot is addressed off is LLVM's choice, and a
 * caller-saved one is already gone by the time a walk reaches this frame.
 */
static bool
fill_var_info (MonoDebugVarInfo &var, const VarSlot &slot, MonoType *type)
{
	if (!mono_dwarf_reg_is_valid (slot.dwarf_reg))
		return false;

	int hw_reg = mono_dwarf_reg_to_hw_reg (slot.dwarf_reg);

	if (!arch::reg_is_recoverable (hw_reg))
		return false;

	memset (&var, 0, sizeof (var));
	var.index = (uint32_t) hw_reg | MONO_DEBUG_VAR_ADDRESS_MODE_REGOFFSET;
	var.offset = (uint32_t) slot.offset;
	var.type = type;
	return true;
}

/*
 * Publish method's code extent, line table and variable homes where the
 * mono_debug_* API can find them.
 *
 * The line table is how an address is turned into an IL offset for anything that
 * is not the frame a debugger stopped in: the soft debugger reads the sequence
 * point table for its top frame and this for every frame below it, so without it
 * a caller's line number is unknown.
 *
 * The variable half is only filled in when the translator pinned this method's
 * arguments and locals to frame slots. That happens when a debugger is
 * attached or a profiler asked for call contexts. Without that there is no
 * home to report for any variable, so has_var_info stays off and the
 * debugger answers ERR_ABSENT_INFORMATION - which is the truth.
 */
static void
publish_debug_info (MonoDomain *domain, MonoMethod *method,
                    MonoMethodHeader *header, const CompiledMethod &compiled)
{
	if (!mono_debug_enabled () || compiled.il_lines.empty ())
		return;

	std::vector<MonoDebugLineNumberEntry> lines;

	lines.reserve (compiled.il_lines.size ());
	for (const IlLineRow &row : compiled.il_lines)
		lines.push_back ({ row.il_offset, row.native_offset });

	MonoDebugMethodJitInfo jit;

	memset (&jit, 0, sizeof (jit));
	jit.code_start = (const mono_byte *) compiled.code;
	jit.code_size = (uint32_t) compiled.code_size;
	jit.num_line_numbers = (uint32_t) lines.size ();
	jit.line_numbers = lines.data ();

	MonoMethodSignature *sig = mono_method_signature_internal (method);
	unsigned num_params = sig->param_count;
	unsigned num_locals = header->num_locals;
	unsigned nargs = num_params + (sig->hasthis ? 1u : 0u);
	MonoDebugVarInfo this_var = {};
	std::vector<MonoDebugVarInfo> params (num_params);
	std::vector<MonoDebugVarInfo> locals (num_locals);

	/*
	 * The marker named every argument and then every local, so a list of any
	 * other length is not describing this method and nothing in it can be
	 * trusted position by position.
	 */
	if (compiled.var_slots.size () == nargs + num_locals) {
		unsigned first_param = nargs - num_params;
		bool complete = true;

		if (sig->hasthis)
			complete = fill_var_info (
				this_var, compiled.var_slots[0],
				m_class_is_valuetype (method->klass)
					? m_class_get_this_arg (method->klass)
					: m_class_get_byval_arg (method->klass));

		for (unsigned i = 0; complete && i < num_params; ++i)
			complete = fill_var_info (params[i],
			                          compiled.var_slots[first_param + i],
			                          sig->params[i]);
		for (unsigned i = 0; complete && i < num_locals; ++i)
			complete = fill_var_info (locals[i], compiled.var_slots[nargs + i],
			                          header->locals[i]);

		if (complete) {
			jit.has_var_info = 1;
			jit.num_params = num_params;
			jit.params = params.data ();
			jit.num_locals = num_locals;
			jit.locals = locals.data ();
			if (sig->hasthis)
				jit.this_var = &this_var;
		}
	}

	mono_debug_add_method (method, &jit, domain);
}

Expected<MonoJitInfo *>
register_jit_info (MonoDomain *domain, MonoMethod *method,
                   MonoMethodHeader *header, const CompiledMethod &compiled,
                   CodeKind kind,
                   const std::vector<std::pair<uint32_t, void *>> &filters,
                   MonoLLVMBreakpointSwitch *bp_switch,
                   const SeqPointGraph &seq_points)
{
	guint8 *code = (guint8 *) compiled.code;
	guint32 code_size = (guint32) compiled.code_size;

	std::vector<UnwindRecord> records;
	if (!parse_unwind_records (compiled.unwind_table, compiled.unwind_table_size,
	                           compiled.code, records))
		return createStringError (inconvertibleErrorCode (),
		                          "the compiled object carries no frame "
		                          "description for the function at %p", code);

	Expected<GSList *> ops = transcode_unwind (records);
	if (!ops)
		return ops.takeError ();

	guint32 encoded_len = 0;
	guint8 *encoded = mono_unwind_ops_encode (*ops, &encoded_len);

	mono_free_unwind_info (*ops);

	/*
	 * The clause table, joined against the IL clauses. An absent section for a
	 * clause-bearing method means the gather declined it, so nothing is
	 * published: a partial table dispatches wrongly rather than failing. A
	 * null header registers clauseless code (an interop thunk).
	 *
	 * A method whose IL declared no clause can still carry a block, because
	 * TierCounterPass (passes/tier-counter.cpp) gives a body with a loop and an
	 * unwinding call a fault clause of its own. An absent block is what most
	 * methods have, and it says the body has no such pad rather than that the
	 * gather refused.
	 */
	std::vector<MonoJitExceptionInfo> clauses;

	std::vector<MonoLsdaEntry> entries;
	bool described = header != nullptr
	                 && parse_mono_lsda (compiled.clause_table,
	                                     compiled.clause_table_size, compiled.code,
	                                     entries);

	if (header != nullptr && !described && header->num_clauses > 0) {
		g_free (encoded);
		return createStringError (inconvertibleErrorCode (),
		                          "the compiled object carries no usable "
		                          "clause table");
	}

	if (described) {
		Expected<std::vector<MonoFinallyGuard>> guards = parse_guards (
			compiled.guard_table, compiled.guard_table_size, compiled.code,
			code_size);

		if (!guards) {
			g_free (encoded);
			return guards.takeError ();
		}

		if (!build_ex_info (entries, header->clauses,
		                    (int) header->num_clauses, code, code_size,
		                    clauses, *guards)) {
			g_free (encoded);
			return createStringError (inconvertibleErrorCode (),
			                          "the clause table does not join "
			                          "against the method's IL clauses");
		}

		/*
		 * The filter body's entry only exists once the object is linked, so
		 * it is joined in here rather than travelling through the section.
		 */
		for (MonoJitExceptionInfo &ei : clauses) {
			if (ei.flags != MONO_EXCEPTION_CLAUSE_FILTER)
				continue;

			void *body = nullptr;

			for (const auto &filter : filters)
				if (filter.first == (uint32_t) ei.clause_index)
					body = filter.second;
			if (body == nullptr) {
				g_free (encoded);
				return createStringError (inconvertibleErrorCode (),
				                          "no compiled filter body for "
				                          "clause %d", ei.clause_index);
			}
			ei.data.filter = body;
		}
	}

	int num_clauses = (int) clauses.size ();
	/*
	 * A shared body's record names the shared method, so a stack walk asking
	 * which instantiation a frame is running as has to read the receiver out of
	 * that frame. The generic jit info is where the runtime looks for it. Only
	 * the method's own body carries one: a filter body or an entry thunk is a
	 * frame of its own, and neither holds the receiver at a recorded slot.
	 */
	bool has_generic = header != nullptr && method->is_inflated
	                   && mono_dwarf_reg_is_valid (compiled.rgctx_slot.dwarf_reg)
	                   && arch::reg_is_recoverable (
		                   mono_dwarf_reg_to_hw_reg (compiled.rgctx_slot.dwarf_reg));
	MonoJitInfoFlags flags =
		has_generic ? JIT_INFO_HAS_GENERIC_JIT_INFO : JIT_INFO_NONE;
	size_t jinfo_size = mono_jit_info_size (flags, num_clauses, 0);

	/*
	 * The IL-offset map rides in the same allocation, past everything
	 * mono_jit_info_size () accounts for, so it is reclaimed exactly when the
	 * record is. A dynamic method's record is g_free ()d wholesale, with no
	 * separate free for a second pointer hung off it.
	 */
	size_t n_seq_points = compiled.il_lines.size ();
	size_t map_offset = (size_t) ALIGN_TO (jinfo_size, sizeof (guint32));
	size_t map_size = n_seq_points * sizeof (MonoLLVMSeqPoint);
	/*
	 * The folded bodies ride along behind the map, for the same reason and with
	 * the same lifetime. A row whose offset names no row of the map is left out:
	 * the runtime looks a chain up by that offset, and finds this one at no
	 * address at all.
	 */
	std::vector<MonoLLVMInlineFrame> inlined;

	inlined.reserve (compiled.inline_frames.size ());
	for (const IlInlineRow &row : compiled.inline_frames) {
		auto anchored = std::lower_bound (
			compiled.il_lines.begin (), compiled.il_lines.end (),
			row.native_offset, [] (const IlLineRow &line, uint32_t offset) {
				return line.native_offset < offset;
			});

		if (anchored == compiled.il_lines.end ()
		    || anchored->native_offset != row.native_offset)
			continue;

		MonoLLVMInlineFrame frame;

		frame.native_offset = row.native_offset;
		frame.il_offset = row.il_offset;
		frame.depth = row.depth;
		frame.method = (MonoMethod *) (uintptr_t) row.callee;
		inlined.push_back (frame);
	}

	size_t inline_offset =
		(size_t) ALIGN_TO (map_offset + map_size, sizeof (gpointer));
	size_t total_size = inline_offset + inlined.size () * sizeof (MonoLLVMInlineFrame);

	/*
	 * A dynamic method's record is unregistered again when the method is freed,
	 * and mono_jit_info_table_remove () frees what it unregisters, so it has to
	 * come from the allocator that call uses. Everything else lives exactly as
	 * long as its domain, so it comes out of the domain's mempool.
	 */
	MonoJitInfo *jinfo =
		method->dynamic
			? (MonoJitInfo *) g_malloc0 (total_size)
			: (MonoJitInfo *) mono_domain_alloc0 (domain, total_size);

	mono_jit_info_init (jinfo, method, code, code_size, flags, num_clauses, 0);
	jinfo->from_llvm = true;

	if (has_generic) {
		MonoGenericJitInfo *gi = mono_jit_info_get_generic_jit_info (jinfo);

		/*
		 * Shared over reference instantiations only, so never gsharedvt. The
		 * runtime dereferences this without a null check
		 * (mini_add_method_trampoline), and it holds nothing per compile, so
		 * one const record serves every body.
		 */
		static MonoGenericSharingContext shared_context = { FALSE };

		gi->generic_sharing_context = &shared_context;
		gi->has_this = 1;
		gi->this_in_reg = 0;
		gi->this_reg =
			(guint8) mono_dwarf_reg_to_hw_reg (compiled.rgctx_slot.dwarf_reg);
		gi->this_offset = compiled.rgctx_slot.offset;
	}

	jinfo->llvm_side_body = header == nullptr;
	jinfo->llvm_abi_thunk = kind == CodeKind::AbiThunk;
	/*
	 * Reading the classic JIT's mapping instead is not an option: it is keyed by
	 * MonoMethod, not by body, so it cannot tell this body's offsets from
	 * another body's. This one is recovered from this body's own line table, so
	 * it says "unknown" only when there was nothing to recover - a method
	 * compiled without debug info, e.g. an interop thunk.
	 */
	jinfo->no_il_offsets = n_seq_points == 0;

	if (n_seq_points > 0) {
		MonoLLVMSeqPoint *map =
			(MonoLLVMSeqPoint *) ((char *) jinfo + map_offset);

		for (size_t i = 0; i < n_seq_points; ++i) {
			map[i].native_offset = compiled.il_lines[i].native_offset;
			map[i].il_offset = compiled.il_lines[i].il_offset;
		}

		jinfo->llvm_seq_points = map;
		jinfo->n_llvm_seq_points = (guint32) n_seq_points;
	}

	if (!inlined.empty ()) {
		MonoLLVMInlineFrame *frames =
			(MonoLLVMInlineFrame *) ((char *) jinfo + inline_offset);

		memcpy (frames, inlined.data (),
		        inlined.size () * sizeof (MonoLLVMInlineFrame));
		jinfo->llvm_inline_frames = frames;
		jinfo->n_llvm_inline_frames = (guint32) inlined.size ();
	}

	if (num_clauses > 0)
		memcpy (&jinfo->clauses[0], clauses.data (),
		        num_clauses * sizeof (MonoJitExceptionInfo));

	jinfo->unwind_info = mono_cache_unwind_info (encoded, encoded_len);
	g_free (encoded);

	/*
	 * The debugger has to be able to find both halves before the code can be
	 * reached, and it looks for them through the jit info - so they go in
	 * before the record is published rather than after.
	 */
	jinfo->llvm_bp_switch = bp_switch;
	if (!compiled.seq_points.empty ())
		publish_seq_points (domain, method, jinfo, compiled.seq_points,
		                    seq_points);
	/*
	 * Only the method's own body. An entry thunk is registered against the
	 * same MonoMethod, and mono_debug_add_method () keys by method, so a
	 * second call for it replaces the body's table - and it has no IL of its
	 * own to put there anyway.
	 */
	if (header != nullptr)
		publish_debug_info (domain, method, header, compiled);

	mono_jit_info_table_add (domain, jinfo);
	return jinfo;
}

} // namespace mono

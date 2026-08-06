/**
 * \file
 * \brief Publishing a compiled method's MonoJitInfo from its side tables.
 *
 * The compiler wrote two sections next to the code: the clause table, read back
 * with the tiered backend's own reader (parse_mono_lsda / build_ex_info), and
 * the frame description, transcoded here into the unwind ops mono's unwinder
 * executes. What comes out is the MonoJitInfo mono_handle_exception and the
 * stack walks search for: from_llvm, so dispatch re-enters frames through the
 * landing pads with the exception and clause index in the registers a pad
 * reads.
 */

#include "jinfo.hpp"

#include "arch/arch.hpp"
#include "sidetables.hpp"

#include "../mini/llvm/mono_lsda.hpp"

#include "mini.h"
#include "mini-unwind.h"
#include "mini-runtime.h"

#include "mono/metadata/domain-internals.h"

#include "seq-point-marker.hpp"

/* A C header with no linkage guard of its own. */
extern "C" {
#include "mono/metadata/seq-points-data.h"
}

#include <llvm/Support/Error.h>

#include <cstring>
#include <map>
#include <vector>

using namespace llvm;

namespace mono {
namespace {

struct WireRecord {
	uint32_t offset;
	uint8_t op;
	int32_t reg;
	int64_t value;
};

template <typename T>
T
read_le (const uint8_t *p)
{
	T value;

	memcpy (&value, p, sizeof (T));
	return value;
}

/// The records of the block describing the function linked at CODE, or false
/// when the section holds no such block.
bool
parse_unwind_records (const uint8_t *section, size_t size, const uint8_t *code,
                      std::vector<WireRecord> &out)
{
	if (section == nullptr)
		return false;

	const uint8_t *end = section + size;

	for (const uint8_t *block = section; (size_t) (end - block) >= unwind_header_size;) {
		if (read_le<uint32_t> (block) != unwind_section_magic)
			return false;
		if (read_le<uint16_t> (block + 4) != unwind_section_version)
			return false;

		uint32_t count = read_le<uint32_t> (block + 8);
		const uint8_t *function =
			(const uint8_t *) (uintptr_t) read_le<uint64_t> (block + 12);
		const uint8_t *p = block + unwind_header_size;
		size_t records = (size_t) count * unwind_record_size;

		if ((size_t) (end - p) < records)
			return false;
		if (function != code) {
			block = p + records;
			continue;
		}

		out.reserve (count);
		for (uint32_t i = 0; i < count; ++i, p += unwind_record_size) {
			WireRecord r;

			r.offset = read_le<uint32_t> (p);
			r.op = p[4];
			r.reg = read_le<int32_t> (p + 5);
			r.value = read_le<int64_t> (p + 9);
			out.push_back (r);
		}

		return true;
	}

	return false;
}

/// The finally guard records, or an error if the section is malformed or names a
/// slot a stack walk could not reach.
///
/// A null section is not a failure: a method with no finally, or one whose bodies
/// all optimized away, has nothing for a thread to be stopped inside.
Expected<std::vector<MonoFinallyGuard>>
parse_guards (const uint8_t *section, size_t size, uint32_t code_size)
{
	std::vector<MonoFinallyGuard> guards;

	if (section == nullptr || size == 0)
		return guards;

	if (size < guards_header_size || read_le<uint32_t> (section) != guards_section_magic
	    || read_le<uint16_t> (section + 4) != guards_section_version)
		return createStringError (inconvertibleErrorCode (),
		                          "the compiled object's finally-guard table is "
		                          "not one of ours");

	uint32_t count = read_le<uint16_t> (section + 6);

	if (size != guards_header_size + (size_t) count * guards_record_size)
		return createStringError (inconvertibleErrorCode (),
		                          "the finally-guard table's length does not match "
		                          "its %u records", count);

	const uint8_t *p = section + guards_header_size;

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

/// The frame description as mono unwind ops, or an error naming what could not
/// be expressed.
///
/// Almost everything maps one to one - mono's unwinder executes def_cfa,
/// offset, same_value and one level of remember/restore state natively. The one
/// normalization is RESTORE, which mono cannot execute: it reverts a register
/// to its rule at function entry, so it is replayed here as that entry rule -
/// the entry state is the leading run of offset-zero records - which is an
/// offset where the entry saved the register and same_value where it did not.
Expected<GSList *>
transcode_unwind (const std::vector<WireRecord> &records)
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

	/* DWARF reg -> its rule at entry; absent means not saved. */
	std::vector<std::pair<int32_t, int64_t>> entry_offsets;
	bool in_entry_state = true;

	for (const WireRecord &r : records) {
		if (r.offset != 0)
			in_entry_state = false;

		switch (r.op) {
		case MONO_UNWIND_OP_DEF_CFA: {
			int reg = hw_reg (r.reg);

			if (reg < 0)
				return fail (createStringError (
					inconvertibleErrorCode (),
					"cfa register %d has no mono mapping", r.reg));
			emit (r.offset, DW_CFA_def_cfa, (uint16_t) reg,
			      (int32_t) r.value);
			break;
		}
		case MONO_UNWIND_OP_DEF_CFA_OFFSET:
			emit (r.offset, DW_CFA_def_cfa_offset, 0, (int32_t) r.value);
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
			 * mono's unwinder holds exactly one remembered state; a deeper
			 * nest would mis-unwind, so it is refused here instead.
			 */
			if (++remember_depth > 1)
				return fail (createStringError (
					inconvertibleErrorCode (),
					"remember_state nested deeper than mono supports"));
			emit (r.offset, DW_CFA_remember_state, 0, 0);
			break;
		case MONO_UNWIND_OP_RESTORE_STATE:
			if (--remember_depth < 0)
				return fail (createStringError (
					inconvertibleErrorCode (),
					"restore_state without a remembered state"));
			emit (r.offset, DW_CFA_restore_state, 0, 0);
			break;
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

} // namespace

/*
 * Publish METHOD's sequence points: the table the soft debugger looks an IL
 * offset up in to place a breakpoint, and looks a native offset up in to say
 * where a stopped thread is.
 *
 * The rows come out of the line table in the encoding seq-point-marker.hpp
 * describes and are already ascending by native offset, which is what
 * mono_seq_point_find_prev_by_native_offset () walks. Registration mirrors
 * mono_save_seq_point_info (): the table is keyed per MonoMethod rather than
 * per body, so a second body for the same method leaves the first one's
 * registration alone and publishes it, rather than pointing the new jit info at
 * a table that is about to be freed.
 *
 * GRAPH says which sequence points can follow which, as IL offsets; what goes
 * into the table are indices into the table itself, so it is joined against the
 * rows on the way past.
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

	mono_domain_lock (domain);

	MonoSeqPointInfo *live = (MonoSeqPointInfo *) g_hash_table_lookup (
		domain_jit_info (domain)->seq_points, method);

	if (live == NULL) {
		g_hash_table_insert (domain_jit_info (domain)->seq_points, method, info);
		live = info;
	} else {
		mono_seq_point_info_free (info);
	}
	mono_domain_unlock (domain);

	jinfo->seq_points = live;
}

Expected<MonoJitInfo *>
register_jit_info (MonoDomain *domain, MonoMethod *method,
                   MonoMethodHeader *header, const CompiledMethod &compiled,
                   const std::vector<std::pair<uint32_t, void *>> &filters,
                   MonoLLVMBreakpointSwitch *bp_switch,
                   const SeqPointGraph &seq_points)
{
	guint8 *code = (guint8 *) compiled.code;
	guint32 code_size = (guint32) compiled.code_size;

	std::vector<WireRecord> records;
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
	 * clause-bearing method means the gather declined it - nothing may be
	 * published, since a partial table dispatches wrongly rather than failing.
	 * A null HEADER registers clauseless code (an interop thunk).
	 */
	std::vector<MonoJitExceptionInfo> clauses;

	if (header != nullptr && header->num_clauses > 0) {
		std::vector<MonoLsdaEntry> entries;

		if (!parse_mono_lsda (compiled.clause_table, compiled.clause_table_size,
		                      entries)) {
			g_free (encoded);
			return createStringError (inconvertibleErrorCode (),
			                          "the compiled object carries no usable "
			                          "clause table");
		}

		Expected<std::vector<MonoFinallyGuard>> guards = parse_guards (
			compiled.guard_table, compiled.guard_table_size, code_size);

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
	size_t jinfo_size = mono_jit_info_size (JIT_INFO_NONE, num_clauses, 0);

	/*
	 * The IL-offset map rides in the same allocation, past everything
	 * mono_jit_info_size () accounts for, so it is reclaimed exactly when the
	 * record is. A dynamic method's record is g_free ()d wholesale and there is
	 * nowhere to hang a second pointer that anyone would know to free.
	 */
	size_t n_seq_points = compiled.il_lines.size ();
	size_t map_offset = (size_t) ALIGN_TO (jinfo_size, sizeof (guint32));
	size_t total_size = map_offset + n_seq_points * sizeof (MonoLLVMSeqPoint);

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

	mono_jit_info_init (jinfo, method, code, code_size, JIT_INFO_NONE,
	                    num_clauses, 0);
	jinfo->from_llvm = true;
	/*
	 * Reading the classic JIT's mapping instead is not an option: it is keyed by
	 * MonoMethod, so it would attribute this body's offsets to another body's
	 * table. This one is recovered from this body's own line table, so it says
	 * "unknown" only when there was nothing to recover - a method compiled
	 * without debug info, e.g. an interop thunk.
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

	mono_jit_info_table_add (domain, jinfo);
	return jinfo;
}

} // namespace mono

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

#include "sidetables.hpp"

#include "../mini/llvm/mono_lsda.hpp"

#include "mini.h"
#include "mini-unwind.h"
#include "mini-runtime.h"

#include "mono/metadata/domain-internals.h"

#include <llvm/Support/Error.h>

#include <cstring>
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

bool
parse_unwind_records (const uint8_t *section, size_t size,
                      std::vector<WireRecord> &out)
{
	if (section == nullptr || size < unwind_header_size)
		return false;
	if (read_le<uint32_t> (section) != unwind_section_magic)
		return false;
	if (read_le<uint16_t> (section + 4) != unwind_section_version)
		return false;

	uint32_t count = read_le<uint32_t> (section + 8);

	if (size != unwind_header_size + (size_t) count * unwind_record_size)
		return false;

	const uint8_t *p = section + unwind_header_size;

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

Error
register_jit_info (MonoMethod *method, MonoMethodHeader *header,
                   const CompiledMethod &compiled)
{
	guint8 *code = (guint8 *) compiled.code;
	guint32 code_size = (guint32) compiled.code_size;

	std::vector<WireRecord> records;
	if (!parse_unwind_records (compiled.unwind_table, compiled.unwind_table_size,
	                           records))
		return createStringError (inconvertibleErrorCode (),
		                          "the compiled object carries no frame "
		                          "description");

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

		if (!build_ex_info (entries, header->clauses,
		                    (int) header->num_clauses, code, code_size,
		                    clauses)) {
			g_free (encoded);
			return createStringError (inconvertibleErrorCode (),
			                          "the clause table does not join "
			                          "against the method's IL clauses");
		}
	}

	MonoDomain *domain = mono_domain_get ();
	int num_clauses = (int) clauses.size ();
	MonoJitInfo *jinfo = (MonoJitInfo *) mono_domain_alloc0 (
		domain, mono_jit_info_size (JIT_INFO_NONE, num_clauses, 0));

	mono_jit_info_init (jinfo, method, code, code_size, JIT_INFO_NONE,
	                    num_clauses, 0);
	jinfo->from_llvm = TRUE;
	/*
	 * No native-offset -> IL-offset mapping is produced yet, and reading the
	 * classic JIT's by MonoMethod would attribute this body's offsets to
	 * another body's table.
	 */
	jinfo->no_il_offsets = TRUE;

	if (num_clauses > 0)
		memcpy (&jinfo->clauses[0], clauses.data (),
		        num_clauses * sizeof (MonoJitExceptionInfo));

	jinfo->unwind_info = mono_cache_unwind_info (encoded, encoded_len);
	g_free (encoded);

	mono_jit_info_table_add (domain, jinfo);
	return Error::success ();
}

} // namespace mono

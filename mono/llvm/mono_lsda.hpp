/**
 * \file
 * mono_lsda.hpp declares the load-time reader for the `.mono_lsda` clause
 * table the JIT writes next to a compiled method's code. The section layout is
 * in mono_lsda.cpp, beside the code that decodes it.
 *
 * parse_mono_lsda () turns the section bytes into MonoLsdaEntry tuples.
 * build_ex_info () joins those tuples against the method's IL clause table
 * to build the MonoJitExceptionInfo array the runtime's unwinder reads. The
 * section carries each clause's IL flags in its kind column, and
 * build_ex_info () cross-checks that column. catch_class lives only in the
 * clause table, so build_ex_info () joins it from there.
 */

#ifndef MONO_LLVM_MONO_LSDA_HPP
#define MONO_LLVM_MONO_LSDA_HPP

#include <cstddef>
#include <cstdint>
#include <vector>

/*
 * mini.h pulls in MonoExceptionClause and MONO_EXCEPTION_CLAUSE_NONE from
 * metadata.h, MonoJitExceptionInfo from domain-internals.h, and
 * MINI_ADDR_TO_FTNPTR from ftnptr.h. metadata.h is included here too,
 * because callers of build_ex_info name its types directly.
 */
#include <mono/metadata/metadata.h>
#include <mono/mini/mini.h>

/* The non-ECMA `kind` marker values. */
#include "mono_lsda_format.hpp"

namespace mono {

/// One entry decoded from a `.mono_lsda` section, in host byte order.
struct MonoLsdaEntry {
	std::uint32_t try_start_off = 0;  ///< invoke range start, code-relative.
	std::uint32_t try_len = 0;        ///< invoke range length.
	std::uint32_t handler_off = 0;    ///< landing pad, code-relative.
	std::uint32_t clause_index = 0;   ///< IL clause index, the join key.

	/// Depends on the entry:
	///   - a protected region holds its clause's IL flags. NONE=0 for catch,
	///     FILTER=1, FINALLY=2, FAULT=4, matching MonoExceptionEnum.
	///   - a marker entry holds a kind from mono_lsda_format.hpp.
	std::uint32_t kind = 0;
};

/// One thread-abort guard for a finally clause. A clause gets one guard per
/// recorded range of its handler body, and every one of them names the same
/// exvar.
struct MonoFinallyGuard {
	std::uint32_t clause_index = 0;       ///< the FINALLY clause this guards.
	std::uint32_t handler_start_off = 0;  ///< handler body start, code-relative.
	std::uint32_t handler_end_off = 0;    ///< handler body end, code-relative.
	std::int32_t exvar_offset = 0;        ///< frame offset of the guard's exvar.
	std::uint8_t exvar_base_reg = 0;      ///< base register for exvar_offset.
};

/**
 * Decodes a `.mono_lsda` section body into header-checked entries.
 *
 * \param sec   the section body.
 * \param size  length of \p sec in bytes.
 * \param out   one entry per section row. Cleared first.
 *
 * \returns whether the section parsed.
 */
bool parse_mono_lsda (const std::uint8_t *sec, std::size_t size,
                      std::vector<MonoLsdaEntry> &out);

/**
 * Joins decoded section entries against a method's IL clause table to build
 * the MonoJitExceptionInfo array the runtime's unwinder reads.
 *
 * \param entries      decoded rows from parse_mono_lsda ().
 * \param clauses      the method's IL clause table, from its header.
 * \param num_clauses  how many clauses \p clauses holds.
 * \param native_code  base address the entry offsets are relative to.
 * \param code_len     length of the loaded code.
 * \param out          the published array. Cleared first. A FILTER entry
 *                     carries a null data.filter, so the caller must join the
 *                     compiled filter body itself.
 * \param guards       thread-abort guards for this method's FINALLY clauses.
 *                     Each becomes one guard-only entry, appended last.
 *
 * \returns whether the join succeeded.
 *
 * \p entries, \p clauses and \p guards must all come from one compile.
 * build_ex_info () asserts on a disagreement between them instead of
 * declining.
 *
 * An empty \p entries, or one holding only marker entries, publishes no
 * protected-region entry and succeeds. The guard entries are still appended.
 */
bool build_ex_info (const std::vector<MonoLsdaEntry> &entries,
                    const MonoExceptionClause *clauses, int num_clauses,
                    const std::uint8_t *native_code, std::uint32_t code_len,
                    std::vector<MonoJitExceptionInfo> &out,
                    const std::vector<MonoFinallyGuard> &guards = {});

} // namespace mono

#endif /* MONO_LLVM_MONO_LSDA_HPP */

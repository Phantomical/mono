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
#include <functional>
#include <vector>

/*
 * mini.h pulls in MonoExceptionClause and MONO_EXCEPTION_CLAUSE_NONE from
 * metadata.h, MonoJitExceptionInfo from domain-internals.h, and
 * MINI_ADDR_TO_FTNPTR from ftnptr.h. metadata.h is included here too,
 * because callers of build_ex_info name its types directly.
 */
#include <mono/metadata/metadata.h>
#include <mono/mini/mini.h>

// The non-ECMA `kind` marker values.
#include "mono_lsda_format.hpp"

namespace mono {

/// One entry decoded from a `.mono_lsda` section, in host byte order.
struct MonoLsdaEntry {
	std::uint32_t try_start_off = 0;  ///< protected range start, code-relative.
	std::uint32_t try_len = 0;        ///< protected range length.
	std::uint32_t handler_off = 0;    ///< landing pad, code-relative.
	std::uint32_t clause_index = 0;   ///< IL clause index, the join key.

	/// Depends on the entry:
	///   - a protected region holds its clause's IL flags. NONE=0 for catch,
	///     FILTER=1, FINALLY=2, FAULT=4, matching MonoExceptionEnum.
	///   - a marker entry holds a kind from mono_lsda_format.hpp.
	std::uint32_t kind = 0;

	/// Which method clause_index indexes into: 0 for the method this section's
	/// block was linked for, the shape every entry had before a fold could
	/// merge a live clause in and what every entry still has today. Otherwise
	/// (uint64_t)(uintptr_t) of a folded body's own MonoMethod*, same
	/// convention as jit.hpp's IlInlineRow::callee. build_ex_info () resolves a
	/// non-zero owner through the OwnerHeader a caller opts in with. Left
	/// unset, any entry naming one is declined.
	std::uint64_t owner = 0;
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
	std::uint64_t owner = 0;              ///< same convention as MonoLsdaEntry::owner.
};

/// Resolves a non-zero MonoLsdaEntry/MonoFinallyGuard owner to that method's
/// own IL clause table. Called at most once per distinct owner build_ex_info ()
/// finds, so a caller may cache and free through it freely. Returning false
/// declines the whole join - a clause build_ex_info () cannot place safely is
/// not one it guesses about.
using OwnerHeader = std::function<bool (std::uint64_t owner,
                                        const MonoExceptionClause *&clauses,
                                        int &num_clauses)>;

/**
 * Decodes the `.mono_lsda` block that describes the function linked at \p code.
 *
 * \param sec   the section body, which holds one block per clause-bearing
 *              function in the object.
 * \param size  length of \p sec in bytes.
 * \param code  where the function was linked, the key a block carries.
 * \param out   one entry per row of that block. Cleared first.
 *
 * \returns whether a block for \p code parsed.
 */
bool parse_mono_lsda (const std::uint8_t *sec, std::size_t size, const void *code,
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
 *                     Each becomes one guard-only entry, appended behind the
 *                     dispatch entries.
 * \param owner_header resolves a non-zero owner (a fold merged a live clause
 *                     in from another method) to that method's own IL clause
 *                     table. Left empty, any entry naming one is declined -
 *                     the caller has not opted in to joining against more
 *                     than the one table every entry named before merging
 *                     existed.
 *
 * \returns whether the join succeeded.
 *
 * \p entries, \p clauses and \p guards must all come from one compile.
 * build_ex_info () asserts on a disagreement between them instead of
 * declining - except a disagreement \p owner_header itself reports by
 * returning false, which declines rather than asserts, since that failure
 * is reachable from a class this backend cannot load rather than only from
 * our own round-trip.
 *
 * An empty \p entries, or one holding only marker entries, publishes no
 * protected-region entry of the method's own and succeeds. A tier-unwind marker
 * publishes one fault clause over the whole method. The guard entries are still
 * appended.
 */
bool build_ex_info (const std::vector<MonoLsdaEntry> &entries,
                    const MonoExceptionClause *clauses, int num_clauses,
                    const std::uint8_t *native_code, std::uint32_t code_len,
                    std::vector<MonoJitExceptionInfo> &out,
                    const std::vector<MonoFinallyGuard> &guards = {},
                    const OwnerHeader &owner_header = {});

} // namespace mono

#endif /* MONO_LLVM_MONO_LSDA_HPP */

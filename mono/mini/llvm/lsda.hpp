/**
 * \file
 * lsda.hpp - C++-only interface for the stock Itanium .gcc_except_table (LSDA)
 * decoder.
 *
 * mono's forked LLVM emitted a mono-format LSDA that unwind.c:decode_lsda()
 * consumed - a parser that opens with g_assert (mono_magic == 0x4d4fef4f) and
 * aborts on the first byte of anything else. Unmodified LLVM 18 emits a standard
 * Itanium .gcc_except_table instead, so the per-method exception description has
 * to be recovered from that. This header exposes an OFFLINE decoder for that
 * section body: given the raw bytes, it yields the call-site table and, per call
 * site, the catch clause's ttype (clause) index.
 *
 * This is the DATA half of milestone M1. It performs no runtime wiring: it does
 * not locate the section, resolve relocations, read ttype table entries (they
 * are relocations the JIT loader resolves - meaningless in an offline buffer),
 * or build a MonoJitExceptionInfo[]. Those are M2+. The decoder only surfaces
 * the ttype INDEX per call site; M2 follows that index into the ttype table to a
 * global whose i32 value is the IL clause index (the "smuggling" trick, doc
 * 07-exception-handling.md 2.4).
 *
 * Consumed ONLY by lsda.cpp and by mono/unit-tests/test-llvm-ehtable.cpp; like
 * engine.hpp it must never be included by mono's C sources.
 */

#ifndef __MONO_MINI_LLVM_LSDA_HPP__
#define __MONO_MINI_LLVM_LSDA_HPP__

#include <cstddef>
#include <cstdint>
#include <vector>

namespace mono {

/*
 * One decoded call-site record.
 *
 * start/length/landing_pad are code offsets from the landing-pad base (which,
 * with the LPStart encoding LLVM emits - DW_EH_PE_omit - is the function start).
 * They are NOT offsets into the LSDA buffer, so this decoder cannot and does not
 * range-check them against the code; that is the loaded function's size, which
 * M2 supplies. landing_pad == 0 means "no landing pad" (the call is protected
 * only for the cleanup/terminate path).
 *
 * ttype_indices is the call site's action chain resolved to ttype/clause
 * indices, in encounter order:
 *   - EMPTY  => the call-site action field was 0: no action record, i.e. a
 *               cleanup-only / no-ttype site. This is represented DISTINCTLY
 *               from a real ttype index (per the M1 spec).
 *   - a value > 0 => a catch clause's ttype index (1-based index into the ttype
 *               table); the value M2 consumes.
 *   - a value == 0 => a cleanup action record reached via a non-zero action
 *               field (a catch chain that also runs cleanups).
 * Negative ttype indices (exception-specification "filter" actions) are not
 * representable by M1's catch-only model and make the whole decode decline.
 */
struct LsdaCallSite {
	std::uint32_t start = 0;
	std::uint32_t length = 0;
	std::uint32_t landing_pad = 0;
	std::vector<std::int32_t> ttype_indices;
};

/*
 * A fully decoded .gcc_except_table.
 *
 * The header fields are surfaced so M2 (and tests) can see exactly which
 * encodings the table used. ttype_base_offset/ttype_entry_count describe the
 * ttype table M2 will index with the values in LsdaCallSite::ttype_indices;
 * ttype_entry_count is a safe UPPER BOUND on the number of entries (the exact
 * count is not recoverable from the buffer alone - the action/ttype table
 * boundary is implicit - so this is the count that fits between the action
 * table and ttbase, which is what every ttype_index is validated against).
 */
struct ParsedLsda {
	std::uint8_t  lpstart_encoding = 0;
	std::uint8_t  ttype_encoding = 0;
	std::uint8_t  call_site_encoding = 0;

	/* false when the TType encoding is DW_EH_PE_omit (no catch clauses). */
	bool          has_ttype_table = false;
	/* Offset of ttbase (one past the last ttype entry) from the LSDA start. */
	std::size_t   ttype_base_offset = 0;
	/* Upper bound on valid ttype indices (1 .. ttype_entry_count). */
	std::size_t   ttype_entry_count = 0;

	std::vector<LsdaCallSite> call_sites;
};

/*
 * Decode a stock Itanium .gcc_except_table (LSDA) body spanning [lsda, lsda +
 * size). Returns true on a fully-parsed, self-consistent table, with OUT filled
 * in. Returns false (declining) on anything unexpected - an unknown or
 * unsupported DW_EH_PE encoding, truncation, an offset or length that runs past
 * the buffer, a malformed uleb/sleb, an action index outside the action table,
 * a ttype index outside the ttype table, or a filter action. It never asserts,
 * aborts, or reads out of bounds on malformed input; every read is bounds
 * checked before the dereference.
 *
 * The encodings supported are exactly the ones stock LLVM 18 emits for a
 * JIT-compiled function in mono's target configuration (static relocation model,
 * small code model): LPStart = DW_EH_PE_omit, TType = DW_EH_PE_udata4 (or omit),
 * call-site = DW_EH_PE_uleb128. Every other encoding hard-declines rather than
 * guessing (CAP-EH-0: never produce a plausible-but-wrong table).
 */
bool decode_gcc_except_table (const std::uint8_t *lsda, std::size_t size, ParsedLsda &out);

} // namespace mono

#endif /* __MONO_MINI_LLVM_LSDA_HPP__ */

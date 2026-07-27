/**
 * \file
 * mono_lsda.hpp - C++-only interface for the load-time `.mono_lsda`
 * publish/validate core (custom-emit EH; plan 12 slice C4).
 *
 * The custom-emit EH path (plan 12) has the JIT's own MCStreamer subclass
 * (MonoLSDAStreamer, engine.cpp, slice C3) write a target-neutral, versioned,
 * code-relative `.mono_lsda` section describing a method's catch geometry:
 *
 *   Header (8 bytes):
 *     u32 magic   = 0x4d4c5344 ('MLSD', little-endian)
 *     u16 version = 2
 *     u16 count                      number of entries (one per invoke range)
 *   Entry[count] (20 bytes each, little-endian, all offsets code-relative):
 *     u32 try_start_off              try covers [code+try_start_off, +try_len)
 *     u32 try_len
 *     u32 handler_off                native handler entry = code + handler_off
 *     u32 clause_index               IL clause index (the join key)
 *     u32 kind                       MonoExceptionEnum: 0=NONE(catch), 2=FINALLY,
 *                                    4=FAULT - or one of the marker kinds in
 *                                    mono_lsda_format.hpp, which describe
 *                                    something other than a protected region
 *
 * This header exposes the SOURCE-AGNOSTIC load-time core (plan 12 3): it turns
 * that byte section into MonoLsdaEntry tuples (parse_mono_lsda), validates them
 * against the method's IL clause table and joins the two into a
 * MonoJitExceptionInfo[] that mini.c copies verbatim into jinfo->clauses
 * (build_ex_info / publish_mono_lsda). The join deliberately reads flags and
 * data.catch_class from cfg->header->clauses[] - the section never carries
 * them (plan 12 2), exactly as the fork's decode_llvm_eh_info did.
 *
 * CAP-EH-0 posture: parse_mono_lsda () declines (returns false) on bad magic,
 * wrong version, truncation, or a size that does not exactly match the
 * declared count - genuine uncertainty about whether the section survived
 * intact. build_ex_info () declines only for a non-catch/finally/fault clause
 * (a filter slipping the gate) or an overlapping (nested) invoke range - real
 * unsupported-input shapes; a join key out of range or a kind/flags mismatch
 * against cfg->header->clauses[] now assert instead, since both sides of that
 * comparison trace back to the SAME immutable cfg->header within one compile,
 * so disagreement can only mean our own round-trip broke, not the input. An
 * entry set that is empty, or contains only resume-pad markers, is published
 * as zero clauses rather than declined - confirmed proof nothing in this
 * method's protected regions survived optimization, not uncertainty.
 *
 * Like engine.hpp this is a C++-only header and must NEVER be included by
 * mono's C sources. It is consumed by mono_lsda.cpp, by translator.cpp (slice
 * C6, which wires publish_mono_lsda onto the live compile path), and by
 * mono/unit-tests/test-llvm-ehtable.cpp.
 */

#ifndef __MONO_MINI_LLVM_MONO_LSDA_HPP__
#define __MONO_MINI_LLVM_MONO_LSDA_HPP__

#include <cstddef>
#include <cstdint>
#include <vector>

/*
 * MonoCompile is an anonymous-struct typedef (mini.h), so it cannot be
 * forward-declared; publish_mono_lsda names it directly. mini.h transitively
 * supplies MonoExceptionClause + MONO_EXCEPTION_CLAUSE_NONE (metadata.h),
 * MonoJitExceptionInfo (domain-internals.h), MINI_ADDR_TO_FTNPTR (ftnptr.h) and
 * mono_mempool_alloc0 (mempool.h). metadata.h is named explicitly for the two
 * types build_ex_info's callers (tests) touch without a MonoCompile.
 */
#include <mono/metadata/metadata.h>
#include <mono/mini/mini.h>

/* The non-ECMA `kind` values, shared with the writer side (engine.cpp). */
#include "mono_lsda_format.hpp"

namespace mono {

/*
 * One raw tuple decoded from a `.mono_lsda` entry (already endianness-native).
 * The offsets are code-relative (label differences from the function base the
 * streamer anchored on); parse_mono_lsda cannot range-check them against the
 * code - that is the loaded function's size, which publish/build supplies.
 */
struct MonoLsdaEntry {
	std::uint32_t try_start_off = 0;
	std::uint32_t try_len = 0;
	std::uint32_t handler_off = 0;
	std::uint32_t clause_index = 0;
	/*
	 * The clause's IL flags (a MonoExceptionEnum: NONE=0/catch, FINALLY=2,
	 * FAULT=4), smuggled through the gather pass and carried in v2 so the section
	 * is self-describing. build_ex_info cross-checks it against the joined clause
	 * table (CAP-EH-0) rather than trusting the join blindly.
	 */
	std::uint32_t kind = 0;
};

/*
 * What the runtime's thread-abort guard needs to know about one FINALLY clause:
 * the PC range its handler body occupies, and the frame home of the exvar the
 * guard flags the abort through.
 *
 * One per surviving copy of the body, so a clause whose body the optimizer
 * duplicated has several - all naming the same exvar. MonoFinallyRangePass
 * (engine.cpp) records the ranges; the exvar offset is joined from the marker
 * stackmap.
 *
 * build_ex_info publishes each as a guard-only MonoJitExceptionInfo; see there
 * for why it is a separate entry rather than fields on the dispatch entries.
 */
struct MonoFinallyGuard {
	std::uint32_t clause_index = 0;
	std::uint32_t handler_start_off = 0;
	std::uint32_t handler_end_off = 0;
	std::int32_t exvar_offset = 0;
};

/*
 * Parse a `.mono_lsda` section body [sec, sec+size) into header-checked entries.
 * Returns false (declining) on a null pointer, a truncated/oversized header, bad
 * magic, version != 2, or - the belt-and-suspenders against the one-record-per-
 * module invariant ever breaking - a section whose length is not EXACTLY
 * 8 + count*20 (a concatenation of two method records is longer than one
 * record's exact size and declines here rather than misattributing). Every read
 * is bounds-checked before the dereference; malformed input never faults.
 */
bool parse_mono_lsda (const std::uint8_t *sec, std::size_t size,
                      std::vector<MonoLsdaEntry> &out);

/*
 * The pure validate-and-join core, factored out of publish_mono_lsda so it is
 * unit-testable without a MonoCompile. Validates ENTRIES against the IL clause
 * table (CLAUSES / NUM_CLAUSES) and the loaded code extent (NATIVE_CODE /
 * CODE_LEN), building one MonoJitExceptionInfo per entry into OUT.
 *
 * An empty ENTRIES (or one containing only resume-pad markers) publishes as
 * zero clauses (out left empty), not a decline - confirmed proof this method's
 * protected regions never survived optimization, not uncertainty.
 *
 * Returns false (decline, CAP-EH-0) - real unsupported-input uncertainty - on:
 *   - try_start_off >= code_len, try_start_off+try_len > code_len (64-bit sum,
 *     no wrap), or handler_off >= code_len;
 *   - two entries whose invoke ranges PARTIALLY overlap or strictly nest (see
 *     mono_lsda.cpp for why equal-or-disjoint is the ordering invariant).
 *
 * Asserts (our own invariant, not uncertainty) on: a join key out of range; a
 * kind/flags mismatch against the joined clause; or the joined clause's flags
 * being outside NONE / FINALLY / FAULT. clause_index/kind were read out of the
 * SAME immutable cfg->header this call is given, at emission time - by a
 * translator that already declined every other flags value upstream
 * (mono_llvm_check_method_supported, translator.cpp) before this method
 * reached codegen at all - so any of these disagreeing means our own
 * round-trip or that earlier gate broke, not that the IL disagrees with
 * itself. A filter clause never reaches this function at all: it is caught by
 * MonoEHGatherPass (engine.cpp) before any section is even published.
 *
 * NESTING SYNTHESIS (EH N1, doc 21 4). For each DISTINCT base range whose
 * innermost clause is `c`, build_ex_info APPENDS one extra MonoJitExceptionInfo
 * per ENCLOSING clause `j` (clause c strictly try-contained in clause j, computed
 * from clauses[].try_offset/handler_offset), de-duplicated so a SIBLING catch
 * group over one shared range - several base entries with one landing pad - yields
 * that encloser only ONCE, not once per sibling. The synthesised entry copies the
 * base's EXACT native range and overrides j's flags/catch_class/clause_index, so
 * out.size() >= entries.size(): base entries occupy the lower slots
 * [0, base_count), enclosing entries the higher ones (the ordering the runtime's
 * flat first-match resume relies on).
 *
 * Its handler_start is whichever pad control is in when the runtime reaches j -
 * the base's own pad, until a cleanup runs, and that cleanup's RESUME pad
 * afterwards (MONO_LSDA_KIND_RESUME_PAD). A resume-pad entry is consumed for that
 * purpose only; it is never published.
 *
 * GUARDS supplies the thread-abort guard ranges for this method's FINALLY
 * clauses (empty for a catch/fault-only method), several per clause where the
 * body was duplicated. Each is published as one extra guard-only entry APPENDED
 * after the base and enclosing entries, so it does not disturb the slot ordering
 * above.
 */
bool build_ex_info (const std::vector<MonoLsdaEntry> &entries,
                    const MonoExceptionClause *clauses, int num_clauses,
                    const std::uint8_t *native_code, std::uint32_t code_len,
                    std::vector<MonoJitExceptionInfo> &out,
                    const std::vector<MonoFinallyGuard> &guards = {});

/*
 * Validate ENTRIES against CLAUSES and the loaded code, then, on
 * success, allocate cfg->llvm_ex_info[] from cfg->mempool and set
 * cfg->llvm_ex_info / cfg->llvm_ex_info_len (mini.c copies it verbatim into
 * jinfo->clauses with from_llvm=1). Returns false on any validation failure, in
 * which case cfg is left untouched and the caller declines to the classic JIT
 * (CAP-EH-0). Thin wrapper over build_ex_info.
 */
bool publish_mono_lsda (MonoCompile *cfg,
                        const std::vector<MonoExceptionClause> &clauses,
                        const std::vector<MonoLsdaEntry> &entries,
                        const std::uint8_t *native_code, std::uint32_t code_len,
                        const std::vector<MonoFinallyGuard> &guards = {});

} // namespace mono

#endif /* __MONO_MINI_LLVM_MONO_LSDA_HPP__ */

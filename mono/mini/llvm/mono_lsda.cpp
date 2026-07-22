/*
 * mono_lsda.cpp: the load-time `.mono_lsda` publish/validate core (custom-emit
 * EH, plan 12 slice C4).
 *
 * MonoLSDAStreamer (engine.cpp, slice C3) emits, into a method's own object, a
 * target-neutral, versioned, code-relative `.mono_lsda` section:
 *
 *   Header (8 bytes, little-endian):
 *     u32 magic   = 0x4d4c5344 ('MLSD')
 *     u16 version = 2
 *     u16 count                      one entry PER INVOKE RANGE (plan 12 2)
 *   Entry[count] (20 bytes each, little-endian):
 *     u32 try_start_off              try covers [code+try_start_off, +try_len)
 *     u32 try_len
 *     u32 handler_off                native handler entry = code + handler_off
 *     u32 clause_index               IL clause index (the join key)
 *     u32 kind                       clause flags (MonoExceptionEnum: 0=catch,
 *                                    2=FINALLY, 4=FAULT); self-describing v2
 *
 * This TU is pure C++ with no LLVM dependency: it consumes the emitted BYTES,
 * not any LLVM type. It is not wired onto the live compile path yet - slice C6
 * calls publish_mono_lsda from translator.cpp once the EH gate is lifted; C4
 * only lands the core and its offline tests.
 *
 * CAP-EH-0 (plan 12 6): every uncertainty DECLINES (returns false) so the caller
 * falls back to the classic JIT - the dispatcher cannot detect a wrong clause
 * array (doc 11 11.4), so a plausible-but-wrong table is never produced. The
 * bounds-check discipline is the one salvaged from lsda.cpp: a private cursor,
 * every read reserving its bytes first, malformed input declining rather than
 * faulting. None of the Itanium ttype/DW_EH_PE machinery is reused - this format
 * carries only code-relative offsets and IL indices, so there is no encoding to
 * chase.
 */

#include "config.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

#include "mono_lsda.hpp"

namespace mono {

namespace {

/* Header constants (little-endian on the wire; the section is target-neutral). */
constexpr std::uint32_t MONO_LSDA_MAGIC = 0x4d4c5344u; /* 'MLSD' */
constexpr std::uint16_t MONO_LSDA_VERSION = 2;
constexpr std::size_t   MONO_LSDA_HEADER_SIZE = 8;
constexpr std::size_t   MONO_LSDA_ENTRY_SIZE = 20;

/*
 * A bounds-checked, little-endian cursor over [start, end). Same contract as
 * lsda.cpp's Reader: the buffer is private, every accessor reserves its bytes
 * through has() first, and a failed read latches ok() to false so a whole
 * structure can be decoded then tested once. Reads are decoded byte-by-byte in
 * little-endian order so the result is independent of host endianness (the
 * section is written little-endian by the x86-64 object writer and stays
 * target-neutral for a future big-endian host).
 */
class Reader {
public:
	Reader (const std::uint8_t *start, const std::uint8_t *end)
		: p_ (start), end_ (end), ok_ (start != nullptr && start <= end)
	{
	}

	bool ok () const { return ok_; }
	std::size_t remaining () const { return static_cast<std::size_t> (end_ - p_); }

	bool has (std::size_t n)
	{
		if (!ok_ || remaining () < n) {
			ok_ = false;
			return false;
		}
		return true;
	}

	std::uint16_t u16 ()
	{
		if (!has (2))
			return 0;
		std::uint16_t v = static_cast<std::uint16_t> (
			static_cast<std::uint16_t> (p_[0]) |
			(static_cast<std::uint16_t> (p_[1]) << 8));
		p_ += 2;
		return v;
	}

	std::uint32_t u32 ()
	{
		if (!has (4))
			return 0;
		std::uint32_t v = static_cast<std::uint32_t> (p_[0]) |
			(static_cast<std::uint32_t> (p_[1]) << 8) |
			(static_cast<std::uint32_t> (p_[2]) << 16) |
			(static_cast<std::uint32_t> (p_[3]) << 24);
		p_ += 4;
		return v;
	}

private:
	const std::uint8_t *p_;
	const std::uint8_t *end_;
	bool ok_;
};

/* Half-open [a_start, a_end) overlaps [b_start, b_end)? (a_end/b_end are 64-bit,
 * already bounds-checked <= code_len, so no wrap.) */
bool
ranges_overlap (std::uint64_t a_start, std::uint64_t a_end,
                std::uint64_t b_start, std::uint64_t b_end)
{
	return a_start < b_end && b_start < a_end;
}

/*
 * Does IL clause J strictly ENCLOSE clause C - i.e. is C nested in J's try?
 * (doc 21 4, EH N1). This is byte-for-byte the translator nesting gate's
 * containment predicate (translator.cpp) plus its sibling exemption:
 *
 *   c.try_offset >= j.try_offset && c.handler_offset <= j.handler_offset
 *
 * with SIBLINGS (identical protected region: same try_offset AND same try_len)
 * excluded. Siblings - try { } catch(A) catch(B) - are NOT nesting; they share
 * one landing pad and are already published as several same-range base entries
 * by the gather, so folding them into nested_in would synthesise a spurious
 * duplicate of the co-sibling over the same range. Excluding them keeps the
 * synthesis a no-op for every shape the gate admits today (catch/finally, no
 * true nesting), which is why N1 is runtime-inert: the gate still declines every
 * strictly-nested method, so no live method has a non-empty nested_in here.
 *
 * The predicate keys on handler_offset (not try_len), so it correctly EXCLUDES a
 * clause sitting in another clause's HANDLER body (disjoint try ranges) - those
 * are admitted today and must not be treated as nested.
 */
bool
clause_encloses (const MonoExceptionClause &c, const MonoExceptionClause &j)
{
	bool siblings = c.try_offset == j.try_offset && c.try_len == j.try_len;
	return !siblings &&
	       c.try_offset >= j.try_offset &&
	       c.handler_offset <= j.handler_offset;
}

} // anonymous namespace

bool
parse_mono_lsda (const std::uint8_t *sec, std::size_t size,
                 std::vector<MonoLsdaEntry> &out)
{
	out.clear ();

	if (!sec)
		return false;

	Reader r (sec, sec + size);

	/* --- header --- */
	std::uint32_t magic = r.u32 ();
	std::uint16_t version = r.u16 ();
	std::uint16_t count = r.u16 ();
	if (!r.ok ())
		return false; /* truncated header */
	if (magic != MONO_LSDA_MAGIC)
		return false; /* bad magic - a format/version mismatch, loudly */
	if (version != MONO_LSDA_VERSION)
		return false; /* unknown version (incl. v1) - decline rather than misread */

	/*
	 * EXACT-SIZE validation (plan 12 3 / C4). The section MUST be exactly one
	 * header plus its declared entries - not merely long enough. C3 guarantees
	 * one method record per module (one-method-per-module invariant, enforced
	 * with report_fatal_error in the streamer), so a section that is LONGER than
	 * 8 + count*20 means that invariant broke and a second record was
	 * concatenated. Reading only the first record would misattribute one
	 * method's clause geometry to another (a CAP-EH-0 silent mis-catch), so a
	 * size mismatch declines here. count is a u16 so count*20 cannot overflow.
	 */
	if (size != MONO_LSDA_HEADER_SIZE +
	            static_cast<std::size_t> (count) * MONO_LSDA_ENTRY_SIZE)
		return false;

	/* --- entries --- */
	out.reserve (count);
	for (unsigned i = 0; i < count; ++i) {
		MonoLsdaEntry e;
		e.try_start_off = r.u32 ();
		e.try_len = r.u32 ();
		e.handler_off = r.u32 ();
		e.clause_index = r.u32 ();
		e.kind = r.u32 ();
		if (!r.ok ())
			return false; /* unreachable given exact-size, but bounds-honest */
		out.push_back (e);
	}

	return true;
}

bool
build_ex_info (const std::vector<MonoLsdaEntry> &entries,
               const MonoExceptionClause *clauses, int num_clauses,
               const std::uint8_t *native_code, std::uint32_t code_len,
               std::vector<MonoJitExceptionInfo> &out)
{
	out.clear ();

	/*
	 * Fail-safe 7 (plan 12 3.5 / 6): a clause-bearing method that produced NO
	 * entries - every protected call optimised to a nounwind `call` so the pass
	 * gathered no landing pad - must decline; publishing an empty clause array
	 * for a method that can throw would silently swallow the exception.
	 */
	if (num_clauses > 0 && entries.empty ())
		return false;

	/*
	 * Order the entries so that, within one try range, ascending IL clause_index
	 * comes first. Sibling catches - try { } catch(A) catch(B) - share one try
	 * range and one landing pad, and the runtime takes the FIRST isinst match in
	 * array order (mini-exceptions.c is_address_protected + the catch loop), so
	 * the earlier-declared catch (smaller clause_index) MUST precede the later
	 * one - otherwise `catch(Derived) catch(Base)` would let the Base clause
	 * swallow a Derived throw. The gather pass records one entry per landing-pad
	 * TypeId, and LLVM hands those back in reverse of the emitted clause order, so
	 * the section order is NOT authoritative; clause_index is. Sort on
	 * (try_start_off, try_len, clause_index) - disjoint ranges never share a PC so
	 * their relative order is immaterial, and the sort keeps identical ranges
	 * grouped and clause-ordered. A stable sort keeps duplicate tuples put.
	 */
	std::vector<MonoLsdaEntry> ordered (entries);
	std::stable_sort (ordered.begin (), ordered.end (),
	                  [] (const MonoLsdaEntry &a, const MonoLsdaEntry &b) {
		if (a.try_start_off != b.try_start_off)
			return a.try_start_off < b.try_start_off;
		if (a.try_len != b.try_len)
			return a.try_len < b.try_len;
		return a.clause_index < b.clause_index;
	});

	out.reserve (ordered.size ());

	/*
	 * Per published entry, its [start, end) native invoke range in offsets, so the
	 * equal-or-disjoint invariant (below) can be validated over the FINAL array -
	 * base entries AND the entries the nesting synthesis appends. For base entries
	 * the range is the entry's own; for a synthesised enclosing entry it is a copy
	 * of its base's range (doc 21 2.2), so the array stays equal-or-disjoint.
	 */
	struct RangeOff { std::uint64_t start; std::uint64_t end; };
	std::vector<RangeOff> ranges;
	ranges.reserve (ordered.size ());

	/*
	 * Per base entry, what the synthesis stage needs to append its enclosing
	 * entries: the innermost clause it names, its exact native range, and its
	 * handler_start - the INNER landing pad, which every synthesised enclosing
	 * entry reuses verbatim (doc 21 1.2 / 4: aot-runtime.c's memcpy keeps the base
	 * handler_start, overriding only flags/catch_class/clause_index).
	 */
	struct BaseEntry {
		std::uint32_t clause_index;
		std::uint32_t try_start_off;
		std::uint32_t try_len;
		gpointer handler_start;
	};
	std::vector<BaseEntry> bases;
	bases.reserve (ordered.size ());

	/* --- base entries: validate + join, exactly as the landed catch/finally path --- */
	for (std::size_t i = 0; i < ordered.size (); ++i) {
		const MonoLsdaEntry &e = ordered[i];

		/*
		 * Offsets in range. 64-bit intermediates so try_start_off + try_len
		 * cannot wrap a 32-bit add (the check the fork deferred to the loader,
		 * which now has code_len in hand). handler_off is the landing-pad entry.
		 */
		if (e.try_start_off >= code_len)
			return false;
		if (static_cast<std::uint64_t> (e.try_start_off) + e.try_len > code_len)
			return false;
		if (e.handler_off >= code_len)
			return false;

		/*
		 * Join key in range. num_clauses is a 15-bit IL header field (never
		 * negative); the guard treats <= 0 as "no clause table", so any entry
		 * declines - an entry cannot reference a clause that does not exist. The
		 * cast is safe because num_clauses > 0 here.
		 */
		if (num_clauses <= 0 || e.clause_index >= static_cast<std::uint32_t> (num_clauses))
			return false;

		const MonoExceptionClause &cl = clauses[e.clause_index];

		/*
		 * v2 self-describing cross-check (CAP-EH-0, belt-and-suspenders). The
		 * section carries the clause's kind (smuggled through the gather pass);
		 * it MUST agree with the flags the IL clause table joins in. A mismatch
		 * means the section and the IL header disagree about this clause - a
		 * producer bug or a corrupt section - so decline rather than publish a
		 * table built on a contradiction. (For a catch method both are NONE.)
		 */
		if (e.kind != static_cast<std::uint32_t> (cl.flags))
			return false;

		/*
		 * EH F2 admits catch (NONE), standalone FINALLY and standalone FAULT. A
		 * FILTER clause still needs resume/indicator machinery this path does not
		 * build, and the translator gate already declines it upstream, so decline
		 * here too rather than mis-dispatch (belt-and-suspenders; the cross-check
		 * above has already confirmed kind == flags). Anything that is not one of
		 * NONE / FINALLY / FAULT is likewise refused.
		 */
		if (cl.flags != MONO_EXCEPTION_CLAUSE_NONE &&
		    cl.flags != MONO_EXCEPTION_CLAUSE_FINALLY &&
		    cl.flags != MONO_EXCEPTION_CLAUSE_FAULT)
			return false;

		/*
		 * Build the published ei (CAP-EH-1). flags is joined from the IL header -
		 * the section never carries it. handler_start is FTNPTR-encoded at publish
		 * (never in the section). The try_offset/try_len/handler_offset/handler_len
		 * fields stay 0 (memset): only the llvmonly match path reads them, and catch
		 * delivery is via RAX for from_llvm (doc 11 6.4).
		 *
		 * The `data` union and `exvar_offset` are kind-dependent:
		 *   - CATCH (NONE): data.catch_class is joined from the IL header.
		 *   - FINALLY: the abort-guard fields data.handler_end and exvar_offset are
		 *     the F2 quiet-gap intermediate - both left 0 (memset). The runtime's
		 *     find_last_handler_block never matches on handler_end == 0, so it never
		 *     writes *(bp + exvar_offset); publishing both 0 keeps the §1.3 invariant
		 *     (both real or both 0). F4 supplies both via the stackmap sideband.
		 *   - FAULT: the runtime reads neither field, so 0 is simply correct.
		 */
		MonoJitExceptionInfo ei;
		memset (&ei, 0, sizeof (ei));
		ei.flags = cl.flags;
		if (cl.flags == MONO_EXCEPTION_CLAUSE_NONE)
			ei.data.catch_class = cl.data.catch_class;
		ei.clause_index = static_cast<int> (e.clause_index);
		ei.try_start = (gpointer) (native_code + e.try_start_off);
		ei.try_end = (gpointer) (native_code + e.try_start_off + e.try_len);
		ei.handler_start = (gpointer) MINI_ADDR_TO_FTNPTR (native_code + e.handler_off);

		out.push_back (ei);
		ranges.push_back ({ static_cast<std::uint64_t> (e.try_start_off),
		                    static_cast<std::uint64_t> (e.try_start_off) + e.try_len });
		bases.push_back ({ e.clause_index, e.try_start_off, e.try_len, ei.handler_start });
	}

	/*
	 * NESTING SYNTHESIS (doc 21 4, EH N1). For each DISTINCT base range whose
	 * innermost clause is `c`, append one extra MonoJitExceptionInfo per ENCLOSING
	 * clause `j` (clause c strictly try-contained in clause j) - at most once per
	 * (range, j) pair, so a sibling group over one range does not multiply its
	 * enclosers (see DE-DUP below). Each synthesised entry copies the base's EXACT
	 * native range and EXACT handler_start (the inner landing pad) and overrides
	 * only j's flags / catch_class / clause_index - the reference oracle is
	 * aot-runtime.c's decode_llvm_mono_eh_frame:3247-3267 (memcpy the base entry,
	 * then override the three join fields).
	 *
	 * ORDERING (load-bearing, doc 21 1.1 / 4 step 4). All synthesised entries are
	 * APPENDED after every base entry, so every base (inner) entry occupies a
	 * LOWER array slot than its enclosing (outer) entries. Because a synthesised
	 * entry copies its base's range, for any faulting PC the runtime's flat
	 * first-match walk sees, at that PC's range, the base entry first and its
	 * enclosing entries after - so an intervening finally runs before an enclosing
	 * catch is entered, and pass-2 resume (which continues at the running clause's
	 * ARRAY slot + 1) reaches the enclosers in innermost-first order. The base
	 * block was already stable_sort-ed above; the synthesised block is NEVER fed
	 * into that sort, so nothing can hoist an enclosing entry ahead of its base.
	 *
	 * For a single base range, its enclosing entries are appended in ASCENDING
	 * clause_index order (the j loop runs low->high). By ECMA-335 a more-deeply-
	 * nested clause precedes its enclosers in the clause table, so ascending
	 * clause_index == innermost-enclosing first (doc 21 4.1). At the depth the gate
	 * admits this is moot (<= 1 encloser per clause), but the order is correct for
	 * deeper nests too.
	 *
	 * DE-DUP BY (base range, enclosing clause_index). A SIBLING catch group -
	 * try { } catch(A) catch(B) - publishes SEVERAL base entries over the SAME
	 * invoke range (one per sibling clause), all sharing the one inner landing pad
	 * the gather emits. Driving synthesis off every base entry would then make each
	 * sibling base independently re-synthesise the SAME enclosing clause `j` over
	 * that one range, appending {range R, clause j, handler H} once per sibling.
	 * For a CATCH encloser that is only latent (a matching catch stops pass-2's
	 * walk), but for a FINALLY/FAULT encloser - which pass-2 does NOT stop on - the
	 * duplicates make its handler run once per sibling when an exception propagates
	 * past every sibling (an ECMA-335 §12.4.2 violation). All sibling bases over R
	 * carry the identical landing pad, so {R, j, H} is the same whichever sibling it
	 * came from: appending clause `j` AT MOST ONCE PER DISTINCT (try_start_off,
	 * try_len) base range collapses the duplicates losslessly. This is keyed on the
	 * range, NOT globally - a multi-invoke inner try enclosed by a finally has
	 * several DISTINCT base ranges and MUST keep one enclosing entry per range, so
	 * distinct ranges are never folded together.
	 *
	 * RUNTIME-INERT (doc 21 8.2, EH N1). The translator nesting gate still declines
	 * every strictly-nested method, so on the live compile path nested_in is empty
	 * for every clause a base entry can name (siblings are excluded by
	 * clause_encloses) and this loop appends nothing - the published array is
	 * byte-identical to the landed catch/finally path. The synthesis is exercised
	 * only by the offline unit tests, which feed a synthetic nested clause table.
	 */
	if (num_clauses > 0) {
		/*
		 * The (range, enclosing clause_index) pairs already appended, so a second
		 * base entry sharing a range does not re-synthesise an encloser its
		 * co-sibling already produced. Keyed on the native range - NOT the base's
		 * clause_index - so distinct invoke ranges each keep their own enclosing
		 * entry.
		 */
		struct Synth { std::uint32_t try_start_off; std::uint32_t try_len; int j; };
		std::vector<Synth> synthesised;

		std::size_t base_count = bases.size ();
		for (std::size_t bi = 0; bi < base_count; ++bi) {
			const BaseEntry &b = bases[bi];
			const MonoExceptionClause &cc = clauses[b.clause_index];

			for (int j = 0; j < num_clauses; ++j) {
				if (static_cast<std::uint32_t> (j) == b.clause_index)
					continue;
				const MonoExceptionClause &cj = clauses[j];
				if (!clause_encloses (cc, cj))
					continue;

				/*
				 * CAP-EH-0 (doc 21 7 item 3): an enclosing clause whose kind is
				 * not one of NONE / FINALLY / FAULT (e.g. a FILTER that slipped a
				 * relaxed gate) cannot be encoded by this path. Decline the whole
				 * array rather than publish a partial one. Checked BEFORE the dedup
				 * skip so an unrepresentable encloser declines even when a co-sibling
				 * would have folded it away.
				 */
				if (cj.flags != MONO_EXCEPTION_CLAUSE_NONE &&
				    cj.flags != MONO_EXCEPTION_CLAUSE_FINALLY &&
				    cj.flags != MONO_EXCEPTION_CLAUSE_FAULT)
					return false;

				/*
				 * Already synthesised for this exact base range by an earlier
				 * (co-sibling) base? Then it is byte-identical - skip it. Distinct
				 * ranges never match here, so they still each get their own entry.
				 */
				bool dup = false;
				for (const Synth &s : synthesised) {
					if (s.try_start_off == b.try_start_off &&
					    s.try_len == b.try_len && s.j == j) {
						dup = true;
						break;
					}
				}
				if (dup)
					continue;
				synthesised.push_back ({ b.try_start_off, b.try_len, j });

				MonoJitExceptionInfo ei;
				memset (&ei, 0, sizeof (ei));
				ei.flags = cj.flags;
				if (cj.flags == MONO_EXCEPTION_CLAUSE_NONE)
					ei.data.catch_class = cj.data.catch_class;
				ei.clause_index = j;
				ei.try_start = (gpointer) (native_code + b.try_start_off);
				ei.try_end = (gpointer) (native_code + b.try_start_off + b.try_len);
				ei.handler_start = b.handler_start; /* the INNER landing pad, verbatim */

				out.push_back (ei);
				ranges.push_back ({ static_cast<std::uint64_t> (b.try_start_off),
				                    static_cast<std::uint64_t> (b.try_start_off) + b.try_len });
			}
		}
	}

	/*
	 * EQUAL-OR-DISJOINT invariant over the FINAL published array (doc 21 2.2 / 4
	 * step 5). Kept as the CAP-EH-0 backstop it was for the non-nested milestone,
	 * now validated over base + synthesised entries:
	 *   - SIBLING catches - try { } catch(A) catch(B) - are ONE landing pad
	 *     carrying one TypeId per catch over the shared invoke range, so C2/C3
	 *     emit several entries with IDENTICAL try_start_off/try_len and DIFFERENT
	 *     clause_index. mono consumes this natively: is_address_protected matches
	 *     the shared PC range for every entry, then mono_object_isinst_checked on
	 *     each catch_class picks the type, RDX = ei->clause_index as the selector.
	 *     Exactly-equal ranges are legitimate and accepted.
	 *   - A try with N protected calls yields N DISJOINT ranges (one per call).
	 *   - A synthesised enclosing entry copies its base's EXACT range, so it is
	 *     always EQUAL to that base (and equal-or-disjoint to everything else the
	 *     base was). Nesting is thus encoded purely by same-range entries + array
	 *     order, never by a nested native extent - the invariant is PRESERVED.
	 * Only a PARTIAL overlap or STRICT nesting ([0x10,0x40) containing [0x20,0x30))
	 * is illegal - it implies a genuine crossing (malformed IL / a producer bug),
	 * making is_address_protected's first-match ambiguous. Such ranges are never
	 * exactly equal, so they still decline; the missed-nesting attack stays
	 * covered. O(n^2) over the handful of ranges a method has.
	 */
	for (std::size_t i = 0; i < ranges.size (); ++i) {
		for (std::size_t j = 0; j < i; ++j) {
			if (ranges_overlap (ranges[i].start, ranges[i].end,
			                    ranges[j].start, ranges[j].end) &&
			    !(ranges[i].start == ranges[j].start &&
			      ranges[i].end == ranges[j].end)) /* equal ranges (siblings / enclosers) ok */
				return false;
		}
	}

	return true;
}

bool
publish_mono_lsda (MonoCompile *cfg,
                   const std::vector<MonoLsdaEntry> &entries,
                   const std::uint8_t *native_code, std::uint32_t code_len)
{
	int num_clauses = cfg->header ? static_cast<int> (cfg->header->num_clauses) : 0;
	const MonoExceptionClause *clauses = cfg->header ? cfg->header->clauses : nullptr;

	std::vector<MonoJitExceptionInfo> built;
	if (!build_ex_info (entries, clauses, num_clauses, native_code, code_len, built))
		return false; /* caller set_failure -> classic JIT */

	/*
	 * Copy the validated array into the compile mempool (freed with the
	 * MonoCompile); mini.c:create_jit_info memcpys it verbatim into
	 * jinfo->clauses (num_clauses = llvm_ex_info_len, from_llvm = 1).
	 */
	guint32 n = static_cast<guint32> (built.size ());
	MonoJitExceptionInfo *arr = nullptr;
	if (n) {
		arr = (MonoJitExceptionInfo *) mono_mempool_alloc0 (
			cfg->mempool, n * sizeof (MonoJitExceptionInfo));
		memcpy (arr, built.data (), n * sizeof (MonoJitExceptionInfo));
	}
	cfg->llvm_ex_info = arr;
	cfg->llvm_ex_info_len = n;

	return true;
}

} // namespace mono

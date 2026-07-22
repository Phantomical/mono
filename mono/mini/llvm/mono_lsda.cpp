/*
 * mono_lsda.cpp: the load-time `.mono_lsda` publish/validate core (custom-emit
 * EH, plan 12 slice C4).
 *
 * MonoLSDAStreamer (engine.cpp, slice C3) emits, into a method's own object, a
 * target-neutral, versioned, code-relative `.mono_lsda` section:
 *
 *   Header (8 bytes, little-endian):
 *     u32 magic   = 0x4d4c5344 ('MLSD')
 *     u16 version = 1
 *     u16 count                      one entry PER INVOKE RANGE (plan 12 2)
 *   Entry[count] (16 bytes each, little-endian):
 *     u32 try_start_off              try covers [code+try_start_off, +try_len)
 *     u32 try_len
 *     u32 handler_off                native handler entry = code + handler_off
 *     u32 clause_index               IL clause index (the join key)
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
constexpr std::uint16_t MONO_LSDA_VERSION = 1;
constexpr std::size_t   MONO_LSDA_HEADER_SIZE = 8;
constexpr std::size_t   MONO_LSDA_ENTRY_SIZE = 16;

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
		return false; /* unknown version - decline rather than misread */

	/*
	 * EXACT-SIZE validation (plan 12 3 / C4). The section MUST be exactly one
	 * header plus its declared entries - not merely long enough. C3 guarantees
	 * one method record per module (one-method-per-module invariant, enforced
	 * with report_fatal_error in the streamer), so a section that is LONGER than
	 * 8 + count*16 means that invariant broke and a second record was
	 * concatenated. Reading only the first record would misattribute one
	 * method's clause geometry to another (a CAP-EH-0 silent mis-catch), so a
	 * size mismatch declines here. count is a u16 so count*16 cannot overflow.
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
		 * Catch-only milestone: a finally/fault/filter clause that slipped the
		 * emission gate would need resume/indicator machinery this slice does not
		 * build (plan 12 9). Decline rather than mis-dispatch.
		 */
		if (cl.flags != MONO_EXCEPTION_CLAUSE_NONE)
			return false;

		/*
		 * Ordering / nesting sanity. The plan asks for an "innermost-first"
		 * ordering check to stay honest for the nested slice. For this non-nested
		 * catch-only milestone the correct, false-decline-free invariant is
		 * EQUAL-OR-DISJOINT invoke ranges:
		 *   - SIBLING catches - try { } catch(A) catch(B) - are ONE landing pad
		 *     carrying one TypeId per catch over the shared invoke range, so C2/C3
		 *     emit several entries with IDENTICAL try_start_off/try_len and
		 *     DIFFERENT clause_index. mono consumes this natively (doc 11 dispatch):
		 *     is_address_protected matches the shared PC range for every entry,
		 *     then mono_object_isinst_checked on each catch_class picks the type,
		 *     with RDX = ei->clause_index as the landing-pad selector. So entries
		 *     that share EXACTLY the same range are legitimate and accepted.
		 *   - A try with N protected calls yields N DISJOINT ranges (one per call)
		 *     sharing one handler.
		 * The translator gate admits equal-range sibling catches, so such methods
		 * reach here on the live compile path (several entries sharing one range,
		 * distinct clause_index), alongside single-catch methods (one clause_index,
		 * possibly across several disjoint invoke ranges). The equal-range branch
		 * handles both.
		 * Only a PARTIAL overlap or STRICT nesting ([0x10,0x40) containing
		 * [0x20,0x30)) is illegal here - it implies genuine nesting (unsupported)
		 * or a producer bug, making is_address_protected's first-match ambiguous.
		 * Such ranges are never exactly equal, so they still decline; the
		 * missed-nesting attack stays covered. O(count^2) over the handful of
		 * invoke ranges a method has. The nested slice later replaces this with
		 * the innermost-first ordering it needs.
		 */
		std::uint64_t a_start = e.try_start_off;
		std::uint64_t a_end = static_cast<std::uint64_t> (e.try_start_off) + e.try_len;
		for (std::size_t j = 0; j < i; ++j) {
			std::uint64_t b_start = ordered[j].try_start_off;
			std::uint64_t b_end =
				static_cast<std::uint64_t> (ordered[j].try_start_off) + ordered[j].try_len;
			if (ranges_overlap (a_start, a_end, b_start, b_end) &&
			    !(a_start == b_start && a_end == b_end)) /* sibling catches share one range */
				return false;
		}

		/*
		 * Build the published ei (plan 12 3 / CAP-EH-1). flags and catch_class
		 * are joined from the IL header - the section never carries them.
		 * handler_start is FTNPTR-encoded at publish (never in the section). The
		 * try_offset/try_len/handler_offset/handler_len and exvar_offset fields
		 * stay 0 (memset): only the llvmonly match path reads them, and catch
		 * delivery is via RAX for from_llvm (doc 11 6.4).
		 */
		MonoJitExceptionInfo ei;
		memset (&ei, 0, sizeof (ei));
		ei.flags = cl.flags;
		ei.data.catch_class = cl.data.catch_class;
		ei.clause_index = static_cast<int> (e.clause_index);
		ei.try_start = (gpointer) (native_code + e.try_start_off);
		ei.try_end = (gpointer) (native_code + e.try_start_off + e.try_len);
		ei.handler_start = (gpointer) MINI_ADDR_TO_FTNPTR (native_code + e.handler_off);

		out.push_back (ei);
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

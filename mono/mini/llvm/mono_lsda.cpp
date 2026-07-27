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
 * CAP-EH-0 (plan 12 6): declining (returning false) is reserved for genuine
 * uncertainty about UNSUPPORTED INPUT - right now that is only a filter
 * clause, caught upstream by MonoEHGatherPass (engine.cpp) before any section
 * reaches here - because the dispatcher cannot detect a wrong clause array
 * (doc 11 11.4), so a plausible-but-wrong table is never produced. The
 * build_ex_info () checks that instead validate our own round-trip of data we
 * ourselves wrote (a clause_index/kind read back from the SAME immutable
 * cfg->header we wrote it from) assert: if those ever disagree, it is our own
 * bug, not the input. parse_mono_lsda ()'s own bounds/format checks (magic,
 * version, exact size, offsets within code_len) are still declines - not yet
 * audited for the same split, so left as originally written. None of the Itanium
 * ttype/DW_EH_PE machinery is needed here - this format carries only
 * code-relative offsets and IL indices, so there is no encoding to chase.
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
 * A bounds-checked, little-endian cursor over [start, end). The buffer is
 * private, every accessor reserves its bytes through has() first, and a
 * failed read latches ok() to false so a whole structure can be decoded then
 * tested once. Reads are decoded byte-by-byte in little-endian order so the
 * result is independent of host endianness (the section is written
 * little-endian by the x86-64 object writer and stays target-neutral for a
 * future big-endian host).
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

/*
 * Append one entry per recovered finally body range, carrying the two things the
 * runtime's thread-abort guard reads about a running finally: the PC range that
 * says a frame is inside the body (find_last_handler_block) and the frame byte
 * to flag the abort through (install_handler_block_guard writes
 * *(bp + exvar_offset) = 1; the shared IR reads it once the finally returns).
 *
 * These are DELIBERATELY entries of their own rather than fields on the FINALLY
 * entries built above. A finally whose protected region has no call that can
 * unwind gets no dispatch entry at all - exactly the shape that left the guard
 * uninstallable - so there would be nothing to attach to. And the partitions
 * differ anyway: one guard entry per clause against one dispatch entry per
 * invoke range.
 *
 * They are inert for dispatch: an EMPTY try range makes is_address_protected ()
 * false for every PC, so neither exception delivery nor mono_handle_finally_block
 * can reach them, and they cannot make a handler run twice. Appending them last
 * also keeps the base/enclosing slot ordering the pass-2 resume walk depends on.
 */
static void
append_finally_guards (const std::vector<MonoFinallyGuard> &guards,
                       const MonoExceptionClause *clauses, int num_clauses,
                       const std::uint8_t *native_code, std::uint32_t code_len,
                       std::vector<MonoJitExceptionInfo> &out)
{
	for (const MonoFinallyGuard &g : guards) {
		/*
		 * The caller keyed these off the same cfg->header, and both bounds are
		 * label differences inside this method's own code. A body that runs to
		 * the end of the function ends at exactly code_len, which is where its
		 * closing label sits, not an overrun.
		 */
		g_assert (static_cast<int> (g.clause_index) < num_clauses);
		g_assert (clauses[g.clause_index].flags == MONO_EXCEPTION_CLAUSE_FINALLY);
		g_assert (g.handler_start_off < g.handler_end_off);
		g_assert (g.handler_end_off <= code_len);

		MonoJitExceptionInfo ei;
		memset (&ei, 0, sizeof (ei));
		ei.flags = MONO_EXCEPTION_CLAUSE_FINALLY;
		ei.clause_index = static_cast<int> (g.clause_index);
		ei.try_start = (gpointer) native_code;
		ei.try_end = (gpointer) native_code;
		ei.handler_start = (gpointer) MINI_ADDR_TO_FTNPTR (native_code + g.handler_start_off);
		ei.data.handler_end = (gpointer) MINI_ADDR_TO_FTNPTR (native_code + g.handler_end_off);
		ei.exvar_offset = g.exvar_offset;

		out.push_back (ei);
	}
}

static bool
build_ex_info_entries (const std::vector<MonoLsdaEntry> &entries,
                       const MonoExceptionClause *clauses, int num_clauses,
                       const std::uint8_t *native_code, std::uint32_t code_len,
                       std::vector<MonoJitExceptionInfo> &out)
{
	out.clear ();

	/*
	 * A clause-bearing method that produced NO entries - every protected call
	 * optimised to a nounwind `call`, so the gather pass found nothing to
	 * publish - is safe to publish as an EMPTY clause array, not a reason to
	 * decline: this function is reached only via a successfully parsed
	 * `.mono_lsda` section, and MonoEHGatherPass/MonoLSDAStreamer (engine.cpp)
	 * only ever publish a section - even a zero-entry one - for a method
	 * explicitly marked mono-has-eh-clauses that the gather did NOT decline.
	 * A genuinely uncertain method (the gather declined it, or nothing was
	 * ever marked) never reaches here: its section is absent, so
	 * parse_mono_lsda () already failed and the caller declined before this
	 * runs. So num_clauses > 0 && entries.empty () here just means "confirmed
	 * nothing in this method can throw" - return success with out left empty.
	 */
	if (entries.empty ())
		return true;

	/*
	 * Keep one landing pad's entries for one invoke range together, in the order
	 * the pad named them. That order IS the nesting chain - innermost clause
	 * first, then outwards through the enclosers - because the translator emits a
	 * pad's covering clauses in it (add_covering_clauses, translator-call.cpp) and
	 * the gather pass undoes LLVM's reversal on the way out (eh-gather.cpp).
	 *
	 * Taking the chain from the pad rather than re-deriving it from IL try offsets
	 * is what lets a chain span methods: an inlined body's offsets mean nothing in
	 * the caller, but its pads are right there in the caller's IR, already extended
	 * by LLVM's inliner with the call site's own chain.
	 *
	 * Order is load-bearing twice over. Sibling catches - try { } catch(A) catch(B)
	 * - share a range and a pad, and the runtime takes the FIRST isinst match in
	 * array order (mini-exceptions.c is_address_protected + the catch loop), so the
	 * earlier-declared catch must come first, or `catch(Derived) catch(Base)` lets
	 * Base swallow a Derived throw. And enclosers must come innermost-first so an
	 * intervening finally runs before an enclosing catch is entered.
	 *
	 * Sorting on (try_start_off, try_len, handler_off) groups a pad's entries for
	 * one range without disturbing them, since a stable sort leaves equal keys put.
	 * Disjoint ranges never share a PC, so their relative order is immaterial.
	 */
	std::vector<MonoLsdaEntry> ordered (entries);
	std::stable_sort (ordered.begin (), ordered.end (),
	                  [] (const MonoLsdaEntry &a, const MonoLsdaEntry &b) {
		if (a.try_start_off != b.try_start_off)
			return a.try_start_off < b.try_start_off;
		if (a.try_len != b.try_len)
			return a.try_len < b.try_len;
		return a.handler_off < b.handler_off;
	});

	/*
	 * The RESUME pad of each finally/fault clause that has one - the landing pad
	 * its resume-trampoline invoke unwinds to, reached only once the cleanup has
	 * run (emit_resume_unwind). The chaining below sends everything that comes
	 * after a cleanup through it, so the block the runtime re-enters is the block
	 * the IR shows control reaching, carrying the state the cleanup left behind.
	 * Recognised by its MONO_LSDA_KIND_RESUME_PAD marker and consumed only that
	 * way: a resume pad is not itself a protected region.
	 */
	std::vector<gpointer> resume_pad (num_clauses > 0 ? num_clauses : 0, nullptr);

	/*
	 * The entries that describe a real protected region, in chain order. The rest
	 * of the section is markers - resume pads and finally body extents - which
	 * describe where code sits rather than what it protects.
	 */
	std::vector<MonoLsdaEntry> dispatch;
	dispatch.reserve (ordered.size ());

	for (const MonoLsdaEntry &e : ordered) {
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
		 * Join key in range. clause_index was itself read out of
		 * cfg->header->clauses[] at emission time (add_covering_clauses,
		 * translator-call.cpp) - the SAME immutable cfg->header this call is
		 * given num_clauses from, for the same compile. It cannot legitimately
		 * come back out of range; if it does, our own object round-trip (or our
		 * own indexing) is wrong, not the IL.
		 */
		g_assert (num_clauses > 0 && e.clause_index < static_cast<std::uint32_t> (num_clauses));

		/*
		 * A cleanup's resume pad. It describes where to continue AFTER
		 * clause_index's handler has run, not a protected region of its own, so
		 * record it for the chaining below and publish nothing for it. Its own
		 * range only ever covers the resume trampoline's call site, which cannot
		 * throw back into this frame.
		 */
		if (e.kind == MONO_LSDA_KIND_RESUME_PAD) {
			const MonoExceptionClause &rc = clauses[e.clause_index];

			/*
			 * Only a cleanup resumes, so only a cleanup can own one of these.
			 * emit_resume_unwind only ever runs for the clause it is itself
			 * emitting the resume pad for (translator-call.cpp), which is only
			 * reachable for a FINALLY/FAULT handler body - so rc.flags here is
			 * the same cfg->header entry emit_handler_start already required
			 * to be FINALLY/FAULT before building this clause's handler at all.
			 */
			g_assert (rc.flags == MONO_EXCEPTION_CLAUSE_FINALLY ||
			         rc.flags == MONO_EXCEPTION_CLAUSE_FAULT);

			resume_pad[e.clause_index] = (gpointer) MINI_ADDR_TO_FTNPTR (native_code + e.handler_off);
			continue;
		}

		/*
		 * A finally handler body's PC range. It describes where the handler's own
		 * code sits, not a region the handler protects, and is consumed only by
		 * the thread-abort guard - the caller has already turned these into
		 * MonoFinallyGuards and passes them in as GUARDS, joined with the frame
		 * slot the stackmap named. Nothing to publish for it here.
		 */
		if (e.kind == MONO_LSDA_KIND_FINALLY_BODY) {
			g_assert (clauses[e.clause_index].flags == MONO_EXCEPTION_CLAUSE_FINALLY);
			continue;
		}

		/*
		 * v2 self-describing cross-check. The section carries the clause's kind,
		 * smuggled through the gather pass from the SAME cfg->header->clauses[]
		 * this call reads cl.flags from (clause_type_info_global writes both from
		 * one clauses[i].flags read, translator-call.cpp). Same immutable header,
		 * same compile - a mismatch is our own round-trip breaking, not the IL
		 * disagreeing with itself. (For a catch clause both are NONE.)
		 */
		g_assert (e.kind == static_cast<std::uint32_t> (clauses[e.clause_index].flags));

		/*
		 * EH F2 admits catch (NONE), FINALLY and FAULT -
		 * mono_llvm_check_method_supported (translator.cpp, the 3b EH gate)
		 * already declined every OTHER flags value on this same cfg->header
		 * before this method reached codegen at all, so cl.flags being anything
		 * else here is that earlier gate's own invariant breaking, not new
		 * information about the IL.
		 */
		g_assert (clauses[e.clause_index].flags == MONO_EXCEPTION_CLAUSE_NONE ||
		         clauses[e.clause_index].flags == MONO_EXCEPTION_CLAUSE_FINALLY ||
		         clauses[e.clause_index].flags == MONO_EXCEPTION_CLAUSE_FAULT);

		dispatch.push_back (e);
	}

	/*
	 * Nothing left that describes a protected region. Either the section was all
	 * markers - a nested finally/fault whose OWN protected try-body had every call
	 * optimized to a nounwind call still emits its resume-pad invoke whenever it
	 * has an encloser (emit_resume_unwind, translator-call.cpp) - or the method's
	 * protected calls all optimized away. Both are the same confirmed-safe "nothing
	 * in this method can throw" case as an empty section, so publish zero clauses.
	 */
	if (dispatch.empty ())
		return true;

	/*
	 * Per published entry, its [start, end) native invoke range in offsets, so the
	 * equal-or-disjoint invariant (below) can be validated over the FINAL array.
	 * Every entry of one chain copies that chain's range, so the array stays
	 * equal-or-disjoint.
	 */
	struct RangeOff { std::uint64_t start; std::uint64_t end; };
	std::vector<RangeOff> ranges;

	out.reserve (dispatch.size ());
	ranges.reserve (dispatch.size ());

	/*
	 * ORDERING (load-bearing). A chain is published innermost-first, so for any
	 * faulting PC the runtime's flat first-match walk sees the innermost clause
	 * first and its enclosers after - an intervening finally runs before an
	 * enclosing catch is entered, and pass-2 resume (which continues at the running
	 * clause's ARRAY slot + 1) reaches the enclosers in innermost-first order. For a
	 * depth-3 try/finally C in B in A the published array is [C, B, A] and pass-2
	 * runs finallys C, B, A - slot-for-slot what the classic JIT produces, which
	 * emits jinfo->clauses in the same inner-first IL clause order.
	 */
	for (std::size_t i = 0; i < dispatch.size (); ) {
		/* One landing pad's entries for one invoke range: a single nesting chain. */
		std::size_t end = i;
		while (end < dispatch.size () &&
		       dispatch[end].try_start_off == dispatch[i].try_start_off &&
		       dispatch[end].try_len == dispatch[i].try_len &&
		       dispatch[end].handler_off == dispatch[i].handler_off)
			++end;

		/*
		 * HANDLER CHAINING. A chain's clauses are reached through whichever pad
		 * control is in by the time the runtime gets to them, and that pad's
		 * selector switch routes each one on to its handler body. It only MOVES
		 * when a cleanup runs: a finally/fault ends in an invoke of the resume
		 * trampoline that unwinds to a pad of its own, so from there on the rest
		 * of the chain is dispatched through that resume pad. A catch that did not
		 * match ran nothing, so it leaves the pad where it was.
		 */
		gpointer cur_handler = (gpointer) MINI_ADDR_TO_FTNPTR (native_code + dispatch[i].handler_off);

		for (std::size_t k = i; k < end; ++k) {
			const MonoLsdaEntry &e = dispatch[k];
			const MonoExceptionClause &cl = clauses[e.clause_index];

			/*
			 * Build the published ei (CAP-EH-1). flags is joined from the IL
			 * header - the section never carries it. handler_start is
			 * FTNPTR-encoded at publish (never in the section). The
			 * try_offset/try_len/handler_offset/handler_len fields stay 0
			 * (memset): only the llvmonly match path reads them, and catch
			 * delivery is via RAX for from_llvm (doc 11 6.4).
			 *
			 * The `data` union and `exvar_offset` are kind-dependent:
			 *   - CATCH (NONE): data.catch_class is joined from the IL header.
			 *   - FINALLY: the abort-guard fields data.handler_end and
			 *     exvar_offset are left 0 here and supplied by the guard entries
			 *     append_finally_guards () adds.
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
			ei.handler_start = cur_handler;

			if (resume_pad[e.clause_index])
				cur_handler = resume_pad[e.clause_index];

			out.push_back (ei);
			ranges.push_back ({ static_cast<std::uint64_t> (e.try_start_off),
			                    static_cast<std::uint64_t> (e.try_start_off) + e.try_len });
		}

		i = end;
	}

	/*
	 * EQUAL-OR-DISJOINT invariant over the FINAL published array (doc 21 2.2 / 4
	 * step 5), the CAP-EH-0 backstop:
	 *   - SIBLING catches - try { } catch(A) catch(B) - are ONE landing pad
	 *     carrying one clause per catch over the shared invoke range, so they
	 *     publish several entries with IDENTICAL try_start_off/try_len and
	 *     DIFFERENT clause_index. mono consumes this natively: is_address_protected
	 *     matches the shared PC range for every entry, then
	 *     mono_object_isinst_checked on each catch_class picks the type, RDX =
	 *     ei->clause_index as the selector. Exactly-equal ranges are legitimate.
	 *   - A try with N protected calls yields N DISJOINT ranges (one per call).
	 *   - An enclosing entry shares its chain's EXACT range, so it is always EQUAL
	 *     to the entries beside it. Nesting is thus encoded purely by same-range
	 *     entries + array order, never by a nested native extent.
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
build_ex_info (const std::vector<MonoLsdaEntry> &entries,
               const MonoExceptionClause *clauses, int num_clauses,
               const std::uint8_t *native_code, std::uint32_t code_len,
               std::vector<MonoJitExceptionInfo> &out,
               const std::vector<MonoFinallyGuard> &guards)
{
	if (!build_ex_info_entries (entries, clauses, num_clauses, native_code, code_len, out))
		return false;

	append_finally_guards (guards, clauses, num_clauses, native_code, code_len, out);
	return true;
}

bool
publish_mono_lsda (MonoCompile *cfg,
                   const std::vector<MonoExceptionClause> &clauses,
                   const std::vector<MonoLsdaEntry> &entries,
                   const std::uint8_t *native_code, std::uint32_t code_len,
                   const std::vector<MonoFinallyGuard> &guards)
{
	std::vector<MonoJitExceptionInfo> built;
	if (!build_ex_info (entries, clauses.data (), static_cast<int> (clauses.size ()),
	                    native_code, code_len, built, guards))
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

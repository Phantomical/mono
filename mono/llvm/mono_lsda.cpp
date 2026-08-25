/*
 * mono_lsda.cpp: reads back the `.mono_lsda` clause table a compiled method's
 * object carries. It joins that table against the method's IL clauses to build
 * the runtime's MonoJitExceptionInfo array.
 *
 * The section is little-endian, versioned and target-neutral. Every offset in
 * it is relative to the start of the method's code.
 *
 *   Header (16 bytes):
 *     u32 magic   = 0x4d4c5344 ('MLSD')
 *     u16 version = 3
 *     u16 count            number of entries
 *     u64 function         where the function this describes was linked
 *   Entry[count] (20 bytes each):
 *     u32 try_start_off    one protected range, [code+try_start_off, +try_len)
 *     u32 try_len
 *     u32 handler_off      landing pad = code + handler_off
 *     u32 clause_index     IL clause index, the join key
 *     u32 kind             MonoExceptionEnum: 0=NONE(catch), 1=FILTER,
 *                          2=FINALLY, 4=FAULT. Any other value is a marker
 *                          kind from mono_lsda_format.hpp.
 *
 * One protected region contributes one entry per clause in its chain. A marker
 * entry describes something other than a protected region.
 *
 * The section holds one such block for each clause-bearing function in the
 * object, concatenated, and the function address is what says which block is
 * whose. A batched compile puts several methods in one object.
 *
 * This reader has no LLVM dependency. native_code is only ever used for
 * address arithmetic, which is what lets the unit tests drive it against a
 * made-up base address.
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

constexpr std::uint32_t MONO_LSDA_MAGIC = 0x4d4c5344u; /* 'MLSD' */
constexpr std::uint16_t MONO_LSDA_VERSION = 3;
constexpr std::size_t   MONO_LSDA_HEADER_SIZE = 16;
constexpr std::size_t   MONO_LSDA_ENTRY_SIZE = 20;

// A bounds-checked little-endian cursor over [start, end). A failed read
// latches ok () to false, so a whole structure can be decoded and then tested
// once rather than after every field.
//
// The bytes are assembled one at a time, so the decode does not depend on host
// endianness.
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

	std::uint64_t u64 ()
	{
		std::uint64_t low = u32 ();

		return low | (static_cast<std::uint64_t> (u32 ()) << 32);
	}

	/// Step over n bytes. Fails the reader when there are fewer left.
	void skip (std::size_t n)
	{
		if (has (n))
			p_ += n;
	}

private:
	const std::uint8_t *p_;
	const std::uint8_t *end_;
	bool ok_;
};

bool
ranges_overlap (std::uint64_t a_start, std::uint64_t a_end,
                std::uint64_t b_start, std::uint64_t b_end)
{
	return a_start < b_end && b_start < a_end;
}

} // anonymous namespace

bool
parse_mono_lsda (const std::uint8_t *sec, std::size_t size, const void *code,
                 std::vector<MonoLsdaEntry> &out)
{
	out.clear ();

	if (!sec)
		return false;

	Reader r (sec, sec + size);

	for (;;) {
		std::uint32_t magic = r.u32 ();
		std::uint16_t version = r.u16 ();
		std::uint16_t count = r.u16 ();
		std::uint64_t function = r.u64 ();

		if (!r.ok ())
			return false;
		if (magic != MONO_LSDA_MAGIC)
			return false;
		if (version != MONO_LSDA_VERSION)
			return false; // decline rather than misread an unknown version

		// count is a u16, so count * 20 cannot overflow.
		std::size_t entries = static_cast<std::size_t> (count) * MONO_LSDA_ENTRY_SIZE;

		if (function != (std::uint64_t) (std::uintptr_t) code) {
			r.skip (entries);
			if (!r.ok ())
				return false; // a block that runs past the section
			if (r.remaining () == 0)
				return false; // no block describes this function
			continue;
		}

		out.reserve (count);
		for (unsigned i = 0; i < count; ++i) {
			MonoLsdaEntry e;
			e.try_start_off = r.u32 ();
			e.try_len = r.u32 ();
			e.handler_off = r.u32 ();
			e.clause_index = r.u32 ();
			e.kind = r.u32 ();
			if (!r.ok ())
				return false; // the block runs past the section
			out.push_back (e);
		}
		break;
	}

	return true;
}

/*
 * Appends one inert entry per finally body range. Each carries that range and
 * the exvar the runtime's thread-abort guard writes into.
 *
 * These are separate entries rather than fields on the FINALLY entries
 * build_ex_info_entries () builds. A finally whose protected region has no call
 * that can unwind gets no dispatch entry to hang them on. The two sets are also
 * different sizes: one guard per recorded body range against one dispatch entry
 * per clause in each protected range.
 *
 * A guard entry's try range is empty, so is_address_protected () is false for
 * every PC and no dispatch walk can reach it. Without that, a handler can run
 * twice. They go last so that the array order the pass-2 resume walk depends on
 * is unchanged.
 */
static void
append_finally_guards (const std::vector<MonoFinallyGuard> &guards,
                       const MonoExceptionClause *clauses, int num_clauses,
                       const std::uint8_t *native_code, std::uint32_t code_len,
                       std::vector<MonoJitExceptionInfo> &out)
{
	for (const MonoFinallyGuard &g : guards) {
		// Both bounds are label differences inside this method's own code. A
		// body that runs to the end of the function ends at exactly code_len,
		// where its closing label sits. So the bound is <= and not <.
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
		ei.exvar_base_reg = g.exvar_base_reg;

		out.push_back (ei);
	}
}

/// One published entry's [start, end) protected range.
struct RangeOff {
	std::uint64_t start;
	std::uint64_t end;
};

/// Whether every pair of ranges is either exactly equal or fully disjoint, which
/// is the only shape the dispatch entries may have.
///
///   - Sibling catches share one pad over one range, so they publish
///     several entries with identical try_start_off and try_len. The runtime
///     matches the shared PC range for each, then picks by catch_class.
///   - A try with N protected calls yields N disjoint ranges, one per call.
///   - An enclosing entry copies its chain's exact range. Nesting is encoded by
///     same-range entries plus array order, never by a nested extent.
///
/// So a partial overlap, or strict nesting like [0x10,0x40) containing
/// [0x20,0x30), means a genuine crossing from malformed IL or a producer bug. It
/// leaves the runtime's first match ambiguous. Such ranges are never exactly
/// equal, so this declines them. The cost is O(n^2) over the handful of ranges a
/// method has.
static bool
ranges_equal_or_disjoint (const std::vector<RangeOff> &ranges)
{
	for (std::size_t i = 0; i < ranges.size (); ++i)
		for (std::size_t j = 0; j < i; ++j)
			if (ranges_overlap (ranges[i].start, ranges[i].end,
			                    ranges[j].start, ranges[j].end)
			    && !(ranges[i].start == ranges[j].start
			         && ranges[i].end == ranges[j].end))
				return false;

	return true;
}

/// Writes into resume_pad, at the entry's clause index, where that clause's
/// resume trampoline unwinds to once the cleanup has run. The chaining in
/// build_ex_info_entries () routes the rest of a chain through that pad.
///
/// The caller publishes no MonoJitExceptionInfo for such an entry. Its range
/// only covers the resume trampoline's call site, which cannot throw back into
/// this frame.
static void
record_resume_pad (const MonoLsdaEntry &e, const MonoExceptionClause *clauses,
                   const std::uint8_t *native_code, std::vector<gpointer> &resume_pad)
{
	// Only a cleanup resumes. emit_endfinally () is the only caller of
	// emit_resume_exit () (method-to-llvm/exceptions.cpp), so the flags can only
	// be FINALLY or FAULT.
	g_assert (clauses[e.clause_index].flags == MONO_EXCEPTION_CLAUSE_FINALLY ||
	          clauses[e.clause_index].flags == MONO_EXCEPTION_CLAUSE_FAULT);

	resume_pad[e.clause_index] =
		(gpointer) MINI_ADDR_TO_FTNPTR (native_code + e.handler_off);
}

/*
 * Appends the fault clause the tier counter's pad is the handler of, if the
 * section named one. Its try range is the whole body, so an exception that
 * unwinds out of the frame from any point reaches it.
 *
 * The section carries one entry for each call that unwinds to the pad, and they
 * all name the same pad. This reads their handler off and ignores their ranges:
 * one clause covers the lot.
 *
 * The clause index is past the IL clauses, where handler_il_offset () reads it as
 * no clause of the method's and answers -1. It goes last, so a clause the IL
 * declared is dispatched first. The counter charges after a finally of the
 * method's own has run, and never in front of a catch that takes the exception.
 */
static void
append_tier_unwind (std::uint32_t handler_off, bool present, int num_clauses,
                    const std::uint8_t *native_code, std::uint32_t code_len,
                    std::vector<MonoJitExceptionInfo> &out)
{
	if (!present)
		return;

	MonoJitExceptionInfo ei;
	memset (&ei, 0, sizeof (ei));
	ei.flags = MONO_EXCEPTION_CLAUSE_FAULT;
	ei.clause_index = num_clauses;
	ei.try_start = (gpointer) native_code;
	ei.try_end = (gpointer) (native_code + code_len);
	ei.handler_start = (gpointer) MINI_ADDR_TO_FTNPTR (native_code + handler_off);

	out.push_back (ei);
}

static bool
build_ex_info_entries (const std::vector<MonoLsdaEntry> &entries,
                       const MonoExceptionClause *clauses, int num_clauses,
                       const std::uint8_t *native_code, std::uint32_t code_len,
                       std::vector<MonoJitExceptionInfo> &out,
                       std::uint32_t &tier_unwind_off, bool &tier_unwind)
{
	out.clear ();
	tier_unwind_off = 0;
	tier_unwind = false;

	// An empty list here means every protected call was optimized to one that
	// cannot unwind, not that the section is missing. A method whose gather
	// declined carries no usable section and already failed in parse_mono_lsda ().
	if (entries.empty ())
		return true;

	// entries already lists each landing pad's clauses in nesting order,
	// innermost first. covering_chain () (method-to-llvm/exceptions.cpp) builds
	// that order per pad, eh-gather.cpp preserves it, and compiler.cpp writes it
	// into the section.
	//
	// So the sort is stable and keys only on the range and the pad. It groups
	// one pad's entries for one range and leaves equal keys where they were.
	// Disjoint ranges never share a PC, so their relative order does not matter.
	std::vector<MonoLsdaEntry> ordered (entries);
	std::stable_sort (ordered.begin (), ordered.end (),
	                  [] (const MonoLsdaEntry &a, const MonoLsdaEntry &b) {
		if (a.try_start_off != b.try_start_off)
			return a.try_start_off < b.try_start_off;
		if (a.try_len != b.try_len)
			return a.try_len < b.try_len;
		return a.handler_off < b.handler_off;
	});

	// Per clause, the pad its resume trampoline unwinds to once the cleanup has
	// run. The chaining below routes the rest of a chain through it, so the
	// runtime re-enters carrying the state the cleanup left behind.
	std::vector<gpointer> resume_pad (num_clauses > 0 ? num_clauses : 0, nullptr);

	// The entries that describe a real protected region, in chain order. The
	// markers drop out here: they say where code sits, not what it protects.
	std::vector<MonoLsdaEntry> dispatch;
	dispatch.reserve (ordered.size ());

	for (const MonoLsdaEntry &e : ordered) {
		// try_start_off + try_len is summed in 64 bits so it cannot wrap.
		if (e.try_start_off >= code_len)
			return false;
		if (static_cast<std::uint64_t> (e.try_start_off) + e.try_len > code_len)
			return false;
		if (e.handler_off >= code_len)
			return false;

		// Before the assert below: this pad names no IL clause, and a method
		// whose IL declared none fails it.
		if (e.kind == MONO_LSDA_KIND_TIER_UNWIND) {
			tier_unwind_off = e.handler_off;
			tier_unwind = true;
			continue;
		}

		// clause_index came from cfg->header->clauses[] in the same compile that
		// num_clauses comes from. Out of range means our own object round-trip
		// broke, not that the IL is bad.
		g_assert (num_clauses > 0 && e.clause_index < static_cast<std::uint32_t> (num_clauses));

		if (e.kind == MONO_LSDA_KIND_RESUME_PAD) {
			record_resume_pad (e, clauses, native_code, resume_pad);
			continue;
		}

		// A finally handler body's PC range. Nothing writes this kind today, and
		// the abort guard is built from the separate .mono_guards section
		// instead. Publish nothing for it.
		if (e.kind == MONO_LSDA_KIND_FINALLY_BODY) {
			g_assert (clauses[e.clause_index].flags == MONO_EXCEPTION_CLAUSE_FINALLY);
			continue;
		}

		// The section's kind column was written from the same clauses[i].flags
		// this reads back, in the same compile. A mismatch means our own
		// round-trip broke. A catch clause has kind and flags both NONE.
		g_assert (e.kind == static_cast<std::uint32_t> (clauses[e.clause_index].flags));

		// Catch (NONE), FILTER, FINALLY and FAULT are the publishable kinds.
		g_assert (clauses[e.clause_index].flags == MONO_EXCEPTION_CLAUSE_NONE ||
		         clauses[e.clause_index].flags == MONO_EXCEPTION_CLAUSE_FILTER ||
		         clauses[e.clause_index].flags == MONO_EXCEPTION_CLAUSE_FINALLY ||
		         clauses[e.clause_index].flags == MONO_EXCEPTION_CLAUSE_FAULT);

		dispatch.push_back (e);
	}

	// dispatch can be empty even though entries was not. A nested finally or
	// fault emits its resume-pad invoke whenever it has an encloser. So a method
	// whose calls all became nounwind still has a section, holding only markers.
	// That is the same case as an empty section, so publish nothing.
	if (dispatch.empty ())
		return true;

	std::vector<RangeOff> ranges;

	out.reserve (dispatch.size ());
	ranges.reserve (dispatch.size ());

	// This order is required. A chain publishes innermost-first, so the
	// runtime's flat first-match walk sees the innermost clause before its
	// enclosers. An intervening finally then runs before an enclosing catch is
	// entered. Sibling catches share a range and a pad, so the earlier-declared
	// catch must come first, or catch(Base) takes a throw meant for
	// catch(Derived). Pass-2 resume continues at the running clause's slot plus
	// one, so it reaches the enclosers innermost-first too. For try/finally C
	// inside B inside A the published array is [C, B, A], and pass-2 runs C, B, A.
	for (std::size_t i = 0; i < dispatch.size (); ) {
		// One landing pad's entries for one range: a single nesting chain.
		std::size_t end = i;
		while (end < dispatch.size () &&
		       dispatch[end].try_start_off == dispatch[i].try_start_off &&
		       dispatch[end].try_len == dispatch[i].try_len &&
		       dispatch[end].handler_off == dispatch[i].handler_off)
			++end;

		// A chain's clauses are reached through whichever pad control is in when
		// the runtime gets to them. That pad's selector switch routes each clause
		// to its handler body. The pad only moves when a cleanup runs. A finally
		// or fault ends by invoking the resume trampoline, which unwinds to a pad
		// of its own. A catch that did not match ran nothing, so it leaves the
		// pad where it was.
		gpointer cur_handler = (gpointer) MINI_ADDR_TO_FTNPTR (native_code + dispatch[i].handler_off);

		for (std::size_t k = i; k < end; ++k) {
			const MonoLsdaEntry &e = dispatch[k];
			const MonoExceptionClause &cl = clauses[e.clause_index];

			/*
			 * ei.flags is joined from the method's own IL header, and the
			 * section's kind column carries the same value. handler_start is
			 * FTNPTR-encoded here, since the section only carries a raw offset.
			 *
			 * try_offset, try_len, handler_offset and handler_len stay 0 from
			 * the memset. They are IL offsets. Only mono_llvm_match_exception ()
			 * reads any of them, and this backend never calls it.
			 *
			 * The data union and exvar_offset are kind-dependent:
			 *   - CATCH (NONE): data.catch_class is joined from the IL header.
			 *   - FILTER: data.filter stays null. jinfo.cpp joins the compiled
			 *     filter body once the object is linked.
			 *   - FINALLY: data.handler_end and exvar_offset stay 0.
			 *     append_finally_guards () appends entries carrying them.
			 *   - FAULT: the runtime reads neither field, so 0 is correct.
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

	return ranges_equal_or_disjoint (ranges);
}

bool
build_ex_info (const std::vector<MonoLsdaEntry> &entries,
               const MonoExceptionClause *clauses, int num_clauses,
               const std::uint8_t *native_code, std::uint32_t code_len,
               std::vector<MonoJitExceptionInfo> &out,
               const std::vector<MonoFinallyGuard> &guards)
{
	std::uint32_t tier_unwind_off = 0;
	bool tier_unwind = false;

	if (!build_ex_info_entries (entries, clauses, num_clauses, native_code, code_len, out,
	                            tier_unwind_off, tier_unwind))
		return false;

	append_finally_guards (guards, clauses, num_clauses, native_code, code_len, out);
	append_tier_unwind (tier_unwind_off, tier_unwind, num_clauses, native_code, code_len, out);
	return true;
}

} // namespace mono

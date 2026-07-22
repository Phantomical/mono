/*
 * lsda.cpp: decode a stock Itanium .gcc_except_table (LSDA) body.
 *
 * The forked LLVM emitted a mono-format LSDA carrying a magic word and a custom
 * per-call-site tuple, which unwind.c:decode_lsda() consumed. Unmodified LLVM 18
 * emits a standard Itanium .gcc_except_table; decode_lsda() opens with
 * g_assert (mono_magic == 0x4d4fef4f) and ABORTS on the first byte of a stock
 * table, so the body decoder here is written from scratch. Its layout is the one
 * documented in 07-exception-handling.md 2.2:
 *
 *   header:
 *     u8    LPStart encoding           (LLVM emits DW_EH_PE_omit)
 *     u8    TType encoding             (LLVM emits DW_EH_PE_absptr or udata4, or omit)
 *     uleb  TType base offset          (present only when TType != omit)
 *     u8    call-site encoding         (LLVM emits DW_EH_PE_uleb128)
 *     uleb  call-site table length
 *   call-site table (each record, fields in the call-site encoding except the
 *   action, which is always uleb):
 *     start, length, landing_pad, action
 *   action table (each record, both fields sleb):
 *     ttype_index, next_disp
 *   ttype table (entries in the TType encoding, indexed BACKWARD from ttbase)
 *
 * WHAT THIS DECODES, AND WHAT IT DELIBERATELY DOES NOT. It yields the call-site
 * table and, per call site, the action chain resolved to ttype/clause indices.
 * It does NOT read the ttype table entries: those are relocations (R_X86_64_32
 * to the type_info globals, doc 2.5) that the JIT loader resolves, so they are
 * meaningless in an offline byte buffer. The ttype INDEX is what M2 needs (it
 * follows the index to the global whose i32 value is the IL clause index, the
 * "smuggling" trick of doc 2.4); the entries themselves are M2's to read once
 * loaded. The table is validated far enough that every surfaced index refers to
 * a slot that physically exists between the action table and ttbase.
 *
 * WHICH ENCODINGS ARE SUPPORTED. Exactly what LLVM 18 emits for a JIT-compiled
 * function in mono's configuration, in both the effective code models seen:
 * LPStart omit, call-site uleb128, and a TType table that is ABSOLUTE in one of
 * two widths -
 *   - DW_EH_PE_absptr (0x00): 8-byte absolute entries, relocated R_X86_64_64.
 *     This is what the real JIT emits under the engine's effective Large code
 *     model (M2.1 finding, doc 09 4).
 *   - DW_EH_PE_udata4 (0x03): 4-byte absolute entries, relocated R_X86_64_32.
 *     What clang-18 -fno-pic -fno-pie -mcmodel=small -static emits (see
 *     test-llvm-ehtable.cpp for the exact bytes); a small-model AOT object.
 * Both are absolute, so a resolved slot holds the type_info address directly.
 * The only difference the decoder cares about is the entry width (8 vs 4), which
 * scales ttype_entry_count. Every OTHER encoding declines rather than guesses
 * (CAP-EH-0): notably TType indirect/pcrel (0x9b), which is what a PIC build
 * emits and would make the ttype entry the address OF a pointer rather than
 * resolvable as a direct absolute base.
 *
 * BOUNDS. Every read goes through Reader, the bounds-checked cursor lifted from
 * ehframe.cpp: buffer pointers are private, every accessor reserves its bytes
 * first, and a failed read latches the cursor so a whole structure can be
 * decoded and tested once at the end. The input is our own JIT's output today
 * but is parsed as untrusted: a malformed or truncated section must decline,
 * never walk off the end, and must not choose how much work we do (the action
 * chain walk is capped).
 */

#include "config.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <vector>

#include "lsda.hpp"

namespace mono {

namespace {

/*
 * DW_EH_PE pointer encodings. Spelled out locally rather than pulled from
 * mini-unwind.h so this decoder carries no mono dependency (it is pure byte
 * manipulation) and so the ones we intentionally do NOT support still have
 * names in the switch. Only the low nibble (value format) and 0xff (omit)
 * matter here; the application bits (pcrel/indirect/...) reach us only to be
 * declined.
 */
constexpr std::uint8_t PE_absptr  = 0x00;
constexpr std::uint8_t PE_omit    = 0xff;
constexpr std::uint8_t PE_uleb128 = 0x01;
constexpr std::uint8_t PE_udata4  = 0x03;

/*
 * A crafted action chain could otherwise loop forever (a next_disp that points
 * back into the chain). No legitimate table nests catch handlers anywhere near
 * this deep, so a chain longer than this is malformed - decline.
 */
constexpr int MAX_ACTION_CHAIN = 256;

/*
 * A bounds-checked cursor over [start, end). Same contract as ehframe.cpp's
 * Reader: private buffer, every advancing accessor reserves through has()
 * first, and a failed read permanently latches ok() to false so a caller can
 * decode then test once. seek() is the one member that repositions without
 * reading and range-checks itself.
 */
class Reader {
public:
	Reader () = default;

	Reader (const std::uint8_t *start, const std::uint8_t *end)
		: start_ (start), p_ (start), end_ (end), ok_ (start <= end)
	{
	}

	bool ok () const { return ok_; }
	const std::uint8_t *pos () const { return p_; }
	std::size_t remaining () const { return static_cast<std::size_t> (end_ - p_); }
	bool at_end () const { return p_ >= end_; }

	bool has (std::size_t n)
	{
		if (!ok_ || remaining () < n) {
			ok_ = false;
			return false;
		}
		return true;
	}

	std::uint8_t u8 ()
	{
		if (!has (1))
			return 0;
		return *p_ ++;
	}

	std::uint32_t u32 ()
	{
		std::uint32_t v = 0;
		if (!has (4))
			return 0;
		std::memcpy (&v, p_, 4);
		p_ += 4;
		return v;
	}

	/*
	 * ULEB128, with the shift clamped: an overlong encoding would otherwise
	 * reach shift >= 64 where "res |= x << shift" is undefined. Modeled on
	 * ehframe.cpp's uleb(); the operand is attacker-shaped and this TU is
	 * compiled without -fwrapv.
	 */
	std::uint64_t uleb ()
	{
		std::uint64_t res = 0;
		int shift = 0;

		for (;;) {
			if (!has (1))
				return 0;
			std::uint8_t b = *p_ ++;

			if (shift < 64)
				res |= static_cast<std::uint64_t> (b & 0x7f) << shift;
			else if (b & 0x7f) {
				ok_ = false;
				return 0;
			}

			if (!(b & 0x80))
				break;
			shift += 7;
			if (shift > 70) {
				ok_ = false;
				return 0;
			}
		}
		return res;
	}

	/* SLEB128, same clamping discipline as uleb(). */
	std::int64_t sleb ()
	{
		std::uint64_t res = 0;
		int shift = 0;
		std::uint8_t b = 0;

		for (;;) {
			if (!has (1))
				return 0;
			b = *p_ ++;

			if (shift < 64)
				res |= static_cast<std::uint64_t> (b & 0x7f) << shift;
			else if ((b & 0x7f) != 0 && (b & 0x7f) != 0x7f) {
				ok_ = false;
				return 0;
			}

			shift += 7;
			if (!(b & 0x80))
				break;
			if (shift > 70) {
				ok_ = false;
				return 0;
			}
		}

		if (shift < 64 && (b & 0x40))
			res |= ~static_cast<std::uint64_t> (0) << shift;

		return static_cast<std::int64_t> (res);
	}

private:
	const std::uint8_t *start_ = nullptr;
	const std::uint8_t *p_ = nullptr;
	const std::uint8_t *end_ = nullptr;
	bool ok_ = false;
};

/*
 * Byte width of a ttype table entry for the (already validated) TType encoding.
 * absptr entries are pointer-sized 8-byte absolutes (the JIT case, Large model);
 * udata4 entries are 4-byte absolutes (the small-model AOT case). Any other
 * encoding is unsupported and returns 0 so the caller declines. See the file
 * header for why only these two absolute forms are accepted.
 */
std::size_t
ttype_entry_size (std::uint8_t encoding)
{
	switch (encoding) {
	case PE_absptr: return 8;
	case PE_udata4: return 4;
	default:        return 0;
	}
}

/*
 * Walk the action chain a call site points at, appending each record's ttype
 * index to OUT. ACTION1 is the call site's action field: a 1-based byte offset
 * into the action table [at, at_end). Returns false on any malformed record, a
 * ttype index outside [1, ttype_count], a filter (negative) action, an offset
 * past the action table, or a chain that exceeds MAX_ACTION_CHAIN (a cycle).
 */
bool
resolve_action_chain (const std::uint8_t *at, const std::uint8_t *at_end,
                      std::uint64_t action1, bool has_ttype, std::size_t ttype_count,
                      std::vector<std::int32_t> &out)
{
	const std::size_t span = static_cast<std::size_t> (at_end - at);

	/* 1-based; 0 is handled by the caller as "no action". */
	if (action1 == 0 || action1 - 1 >= span)
		return false;
	std::size_t off = static_cast<std::size_t> (action1 - 1);

	for (int guard = 0; ; ++guard) {
		if (guard >= MAX_ACTION_CHAIN)
			return false;

		Reader r (at + off, at_end);
		std::int64_t ttype = r.sleb ();
		const std::uint8_t *disp_field = r.pos ();
		std::int64_t disp = r.sleb ();
		if (!r.ok ())
			return false;

		/*
		 * ttype > 0: a catch clause's 1-based ttype index. ttype == 0: a cleanup
		 * action record. ttype < 0: an exception-specification filter, which
		 * M1's catch-only model cannot represent - decline the whole table.
		 */
		if (ttype < 0)
			return false;
		if (ttype > 0) {
			if (!has_ttype || static_cast<std::uint64_t> (ttype) > ttype_count)
				return false;
		}
		if (ttype > INT32_MAX || ttype < INT32_MIN)
			return false;
		out.push_back (static_cast<std::int32_t> (ttype));

		/* next_disp == 0 terminates the chain. */
		if (disp == 0)
			break;

		/*
		 * Next record offset is self-relative to the disp field's position.
		 * Compute the sum in unsigned arithmetic so a near-INT64_MAX disp
		 * cannot signed-overflow (UB); a mathematically-negative result wraps
		 * to a huge value and is rejected by the same >= span bound as before.
		 */
		std::uint64_t target = static_cast<std::uint64_t> (disp_field - at)
		                     + static_cast<std::uint64_t> (disp);
		if (target >= span)
			return false;
		off = static_cast<std::size_t> (target);
	}

	return true;
}

} // anonymous namespace

bool
decode_gcc_except_table (const std::uint8_t *lsda, std::size_t size, ParsedLsda &out)
{
	out = ParsedLsda {};

	if (!lsda)
		return false;

	const std::uint8_t *const end = lsda + size;
	Reader r (lsda, end);

	/* --- header --- */

	/*
	 * LPStart encoding. LLVM emits DW_EH_PE_omit (the landing-pad base is the
	 * function start). A non-omit encoding would carry a base pointer to decode,
	 * which the JIT never produces - decline rather than guess.
	 */
	std::uint8_t lp_enc = r.u8 ();
	if (!r.ok () || lp_enc != PE_omit)
		return false;
	out.lpstart_encoding = lp_enc;

	/*
	 * TType encoding: absptr (8-byte) or udata4 (4-byte) - both ABSOLUTE, a real
	 * ttype table - or omit (no catch clauses). Every other encoding declines.
	 */
	std::uint8_t tt_enc = r.u8 ();
	if (!r.ok ())
		return false;
	out.ttype_encoding = tt_enc;

	const std::uint8_t *ttbase = nullptr;
	if (tt_enc == PE_omit) {
		out.has_ttype_table = false;
	} else {
		if (tt_enc != PE_absptr && tt_enc != PE_udata4)
			return false; /* CAP-EH-0: indirect/pcrel/etc. decline */
		out.has_ttype_table = true;

		/* ttbase is measured from the byte AFTER this uleb field. */
		std::uint64_t tt_off = r.uleb ();
		if (!r.ok ())
			return false;
		const std::uint8_t *ttbase_ref = r.pos ();
		if (tt_off > static_cast<std::uint64_t> (end - ttbase_ref))
			return false;
		ttbase = ttbase_ref + tt_off;
		out.ttype_base_offset = static_cast<std::size_t> (ttbase - lsda);
	}

	/* Call-site encoding: LLVM emits uleb128. */
	std::uint8_t cs_enc = r.u8 ();
	if (!r.ok () || cs_enc != PE_uleb128)
		return false; /* CAP-EH-0 */
	out.call_site_encoding = cs_enc;

	/* Call-site table length, then the table itself. */
	std::uint64_t cs_len = r.uleb ();
	if (!r.ok () || cs_len > r.remaining ())
		return false;

	const std::uint8_t *cs_begin = r.pos ();
	const std::uint8_t *cs_end = cs_begin + cs_len;

	/*
	 * The action table begins immediately after the call-site table. Its end is
	 * ttbase when a ttype table is present (the ttype table lies between the
	 * action table and ttbase, indexed backward), otherwise the buffer end. A
	 * ttbase that lands before the action table starts is malformed.
	 */
	const std::uint8_t *action_table = cs_end;
	const std::uint8_t *action_end = out.has_ttype_table ? ttbase : end;
	if (action_end < action_table)
		return false;

	/*
	 * Upper bound on the number of ttype entries: how many fit between the action
	 * table and ttbase. The exact count is not recoverable (the action/ttype
	 * boundary is implicit), but no valid ttype entry can lie before the action
	 * table, so this bounds every index safely. Entries grow backward from
	 * ttbase, so index i (1-based) occupies [ttbase - i*esize, ttbase -
	 * (i-1)*esize). esize is encoding-derived (8 for absptr, 4 for udata4).
	 *
	 * The count is floor((ttbase - action_table) / esize) with the SAME esize, so
	 * count*esize <= (ttbase - action_table); an index validated <= count in
	 * resolve_action_chain therefore has ttbase - i*esize >= ttbase - count*esize
	 * >= action_table, i.e. its whole slot lies inside [action_table, ttbase),
	 * inside the buffer - for esize=8 exactly as for esize=4. (ttbase - action_table
	 * is a same-buffer pointer difference <= size, so neither the division nor a
	 * later i*esize can overflow.) The entries themselves are never dereferenced
	 * here; M2.3 reads them from the loaded object using ttype_encoding.
	 */
	if (out.has_ttype_table) {
		std::size_t esize = ttype_entry_size (tt_enc);
		if (esize == 0)
			return false;
		out.ttype_entry_count = static_cast<std::size_t> (ttbase - action_table) / esize;
	}

	/* --- call-site table --- */

	Reader cs (cs_begin, cs_end);
	while (cs.ok () && !cs.at_end ()) {
		std::uint64_t start = cs.uleb ();
		std::uint64_t length = cs.uleb ();
		std::uint64_t landing_pad = cs.uleb ();
		std::uint64_t action = cs.uleb ();
		if (!cs.ok ())
			return false;

		/*
		 * These are code offsets, stored by mono as guint32. A value that does
		 * not fit is not something we can faithfully carry forward - decline.
		 * (We cannot range-check them against the code here; the LSDA does not
		 * carry the function size. That check is M2's, with code_len in hand.)
		 */
		if (start > UINT32_MAX || length > UINT32_MAX || landing_pad > UINT32_MAX)
			return false;

		LsdaCallSite rec;
		rec.start = static_cast<std::uint32_t> (start);
		rec.length = static_cast<std::uint32_t> (length);
		rec.landing_pad = static_cast<std::uint32_t> (landing_pad);

		if (action != 0 &&
		    !resolve_action_chain (action_table, action_end, action,
		                           out.has_ttype_table, out.ttype_entry_count,
		                           rec.ttype_indices))
			return false;

		out.call_sites.push_back (std::move (rec));
	}
	if (!cs.ok ())
		return false;

	return true;
}

} // namespace mono

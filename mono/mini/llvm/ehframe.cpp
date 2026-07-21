/*
 * ehframe.cpp: transcode stock DWARF .eh_frame into mono's unwind ops.
 *
 * The forked LLVM used to emit a mono-format EH frame that decode_llvm_eh_info()
 * consumed. Unmodified LLVM emits a standard DWARF .eh_frame instead, so the
 * per-method unwind description has to be recovered from that.
 *
 * This TRANSCODES rather than byte-copying the CFI program. That is not a
 * stylistic preference - a copier is incorrect here:
 *
 *   - DW_CFA_restore means "revert this register to the rule the CIE
 *     established", so its meaning is only recoverable while the CIE's initial
 *     rules are in hand. A copier that concatenates the CIE and FDE programs -
 *     which is what mono_unwind_decode_fde() does - has already lost them.
 *     mono_unwind_frame() cannot execute the opcode at all: its outer switch on
 *     (*p & 0xc0) handles 0x40/0x80/0x00 and returns FALSE from the default,
 *     and DW_CFA_restore is 0xc0.
 *   - The same goes for DW_CFA_remember_state/restore_state, which mono supports
 *     only to a single level (UnwindState state_stack[1]), and for
 *     DW_CFA_undefined, which mono cannot represent at all.
 *   - Normalizing those away means mono_unwind_frame needs no new opcodes to
 *     consume the result.
 *
 * On x86-64, LLVM 18 happens to unwind epilogues with plain
 * DW_CFA_def_cfa_offset steps rather than DW_CFA_restore, so the simplest
 * shapes would survive a copier. That is not something to rely on: restore,
 * remember_state, restore_state, expression and undefined all occur in real
 * compiler output - they are present in mono's own binary - and any of them
 * reaching mono_unwind_frame is a failure during exception dispatch.
 *
 * Which is the third reason to transcode: anything not representable becomes a
 * decline here, i.e. a compile-time refusal that falls the method back to the
 * classic JIT, rather than a wrong or fatal unwind later.
 *
 * Every read of the section goes through Reader. Its buffer pointers are
 * private and every accessor reserves its bytes first, so a decode site cannot
 * overrun by forgetting a check; a failed read latches the cursor permanently,
 * so a whole structure can be decoded and tested once at the end. Two things sit
 * outside that guarantee and are checked by hand instead: Reader::seek(), which
 * moves to an already-validated position and range-checks itself, and
 * CieInfo::parse()'s augmentation string, which is indexed directly after the
 * Reader has walked it to its terminator. The input comes from our own JIT
 * today, but it is parsed as untrusted: a malformed or truncated section must
 * decline, never walk off the end, and must not get to choose how much we
 * allocate.
 */

/*
 * ---- conventions, and how far they travel ----
 *
 * This file is the pilot for converting the rest of mono/mini/llvm to C++,
 * including the ~9,500-line translator. So this separates what should be copied
 * from what only works because this file is small, single-purpose, and driven
 * entirely through one exported function.
 *
 * TRAVELS TO DECODERS, NOT TO THE TRANSLATOR
 *
 * 1. The cursor owns the bounds. Any decoder of externally-shaped bytes should
 *    look like Reader: private buffer, latched failure, one check at the end
 *    instead of one per field. Filed on its own because the translator parses no
 *    untrusted input and gains nothing from it: copy this into the next decoder,
 *    not into mini-llvm.
 *
 * TRAVELS TO THE TRANSLATOR
 *
 * 2. RAII from the first allocation, for mono's own memory. UnwindOps owns its
 *    GSList from the first append, so none of the decline paths below can leak
 *    it or publish half of it, and none of them needs a "free and return" tail.
 *    The translator bails out of half-built state constantly; this is the most
 *    transferable thing here.
 *
 * 3. Name the outcomes when there are more than two, and let the COMPILER
 *    enforce that every one is handled. FdeResult exists because "this FDE is
 *    for another function" is routine during the scan and must not read as a
 *    failure.
 *
 *    An earlier draft of this said the opposite: give the switch a "default:
 *    that asserts", because mono/mini compiled with -Wno-switch -Wno-switch-enum
 *    and would not report a new enumerator. That is no longer true. mono/mini's
 *    AM_CXXFLAGS ends in -Wswitch, which overrides the -Wno-switch configure
 *    puts in CPPFLAGS, so every C++ file under llvm/ is compiled with -Wswitch
 *    in effect (the measured compile line for this file ends "-Wno-switch
 *    -Wno-switch-enum -Wswitch", and the last one wins). A
 *    switch over an enum with no default: label now warns about an unhandled
 *    enumerator at compile time - and a default: label SUPPRESSES that warning,
 *    which is why the one over FdeResult no longer has one. See the note at the
 *    switch itself for what is left in its place and why.
 *
 * 4. State threaded through several functions becomes an object.
 *    CfiInterpreter replaced a seven-parameter function; the translator threads
 *    EmitContext through hundreds and would gain far more.
 *
 * DOES NOT TRAVEL. An earlier draft of this block asserted the first three of
 * these module-wide; they are false outside this file, and a convention that
 * breaks on the first line of the translator is worse than no convention.
 *
 * 5. "Failures carry no reason" is local. It holds here because every failure
 *    means exactly "decline this method" and a reason nobody can act on would be
 *    dead weight. The translator has 29 set_failure() sites writing
 *    cfg->exception_message, surfaced to users as "LLVM failed for X: ...".
 *    That channel is a feature; do not model it on this file.
 *
 * 6. std::optional<T*> is a local convenience, not a pattern. decode_pointer()
 *    returns optional<const uint8_t*>, where the emptiness of the optional and
 *    the nullness of the pointer are different facts that look identical at the
 *    call site (see the has_value() note in transcode_fde()). The translator's
 *    currency is LLVMValueRef, a pointer typedef, so that ambiguity would land
 *    on every other line. Use an explicit outcome type there.
 *
 * 7. "mono types only at the extern "C" boundary" is too strong even here. What
 *    actually holds is narrower: mono's SCALAR typedefs (guint8, guint32,
 *    gboolean) are gone internally, because std::uint8_t and bool say the same
 *    thing without the dependency. mono's types and macros are used freely
 *    wherever mono owns the data - GSList inside UnwindOps, G_MAXINT32 in the
 *    range checks - and a translator built on MonoInst, MonoBasicBlock and
 *    MonoType could not do otherwise. Convert the scalars; claim nothing more.
 *
 * 8. The anonymous namespace works here only because every path is reachable
 *    from mono_llvm_eh_frame_to_unwind_ops(), which the unit test drives
 *    directly. At translator scale that stops being true and internal linkage
 *    becomes the reason a component cannot be tested. Decide the test surface -
 *    an internal .hpp, or a test TU that includes the .cpp - BEFORE reaching for
 *    an anonymous namespace there.
 *
 * 9. CfiInterpreter::step() returns bool carrying two meanings: "the opcode is
 *    representable", and - when the reader has already failed - "nothing to
 *    say". That it needs a comment to explain is the tell. Use a named result
 *    type at scale. It is left alone here only because those thirteen
 *    "if (!r_.ok ()) return true" guards are redundant as to the verdict: a
 *    truncated operand declines either way, through the reader check after the
 *    loop. They avoid acting on a zero operand, nothing more.
 *
 *    That redundancy holds ONLY because every decline in this file is total -
 *    run() aborts the whole method. Make run() recoverable in any way ("skip an
 *    unrepresentable opcode and continue" is the obvious future edit) and those
 *    thirteen "return true"s stop meaning "nothing to say" and start meaning
 *    ACCEPT A ZERO OPERAND, silently. Give step() a named result type before, or
 *    as part of, any such change.
 */

#include "config.h"
#include <glib.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <optional>
#include <vector>

#include <mono/utils/freebsd-dwarf.h>
#include "mini.h"
#include "mini-unwind.h"
#include "backend.h"

/*
 * These stay preprocessor constants rather than C++ ones: they complete the
 * DW_* macro namespace that freebsd-dwarf.h and mini-unwind.h define, and the
 * #ifndef is what keeps them from colliding with those headers.
 */

/* Not in mini-unwind.h's DW_EH_PE enum, but legal encodings we may be handed. */
#ifndef DW_EH_PE_udata8
#define DW_EH_PE_udata8 0x04
#endif
/*
 * GCC/LLVM extension recording the argument-stack adjustment across a call.
 * It carries no register or CFA rule, so it is simply skipped.
 */
#ifndef DW_CFA_GNU_args_size
#define DW_CFA_GNU_args_size 0x2e
#endif

namespace {

/*
 * Storage bound for per-register rules. NUM_DWARF_REGS itself is private to
 * unwind.c, so this is a generous compile-time array bound; the real validity
 * limit is computed at runtime by reg_ok() below.
 */
constexpr int MAX_DWARF_REG = 32;

/*
 * mono_unwind_ops_encode_full() writes into a fixed guint8 buf[4096] and only
 * checks the bound AFTER the writes, so an over-long op list is a stack smash
 * rather than a diagnostic. The worst case per op is a 5-byte advance_loc plus
 * an opcode and the largest operand encoding it actually uses, i.e. 16 bytes;
 * this caps the list well inside 4096 / 16 and declines beyond it.
 *
 * Do not raise this without first giving mono_unwind_ops_encode_full() a bound
 * check that runs before the writes.
 */
constexpr int MAX_UNWIND_OPS = 128;

/*
 * Depth bound for the simulated DW_CFA_remember_state stack.
 *
 * This is an intentional, bounded BEHAVIOUR CHANGE, not a no-op: the previous
 * version simulated the stack with an unbounded GQueue and accepted any depth.
 * The boundary is exact - depth 120..128 is accepted by both; depth 129..132 was
 * accepted before and is DECLINED here (ops = null, nothing leaked).
 *
 * It cannot reject real output. The deepest remember_state nesting measured is 1
 * in libc (3,788 FDEs) and 1 in libstdc++ (5,127 FDEs), and mono's own unwinder
 * limit is likewise 1, so 128 is 128x the observed maximum. The bound it buys
 * caps the simulation at 128 x 264 B ~= 33 KB (~68 KB transient across a vector
 * regrow), on the heap rather than the stack, so a malformed section cannot
 * choose how much we allocate. See CfiInterpreter::stack_.
 */
constexpr std::size_t MAX_REMEMBER_DEPTH = 128;

/* ---------------------------------------------------------------- reading */

/*
 * A bounds-checked cursor over [start, end).
 *
 * Reading cannot overrun by construction: the buffer pointers are private, and
 * every accessor that advances the cursor reserves its bytes through has()
 * first, so a decode site cannot forget a check. Once a read fails the cursor is
 * permanently failed and every subsequent read is a no-op returning 0, so a
 * caller can decode a whole structure and test ok() once at the end rather than
 * after every field.
 *
 * seek() is the exception: it repositions without reading, so it cannot reserve
 * anything and range-checks its argument itself. It is the only member that
 * could otherwise leave the cursor outside the buffer.
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

	/*
	 * Reposition within the buffer. Used to step over a region whose length was
	 * already validated (an augmentation block, a whole .eh_frame entry) and, for
	 * an augmentation, to move back to its declared end.
	 *
	 * Out-of-range is a failure rather than an unchecked assignment: this is the
	 * only way to move the cursor that is not itself a length check, so it must
	 * not be able to leave it outside the buffer - which would make remaining()
	 * wrap and defeat every has() after it.
	 */
	void seek (const std::uint8_t *p)
	{
		if (p < start_ || p > end_) {
			ok_ = false;
			return;
		}
		p_ = p;
	}

	/*
	 * Reserve N bytes. Returns false and latches failure if they are not there,
	 * which is what makes every accessor below safe.
	 */
	bool has (std::size_t n)
	{
		if (!ok_ || remaining () < n) {
			ok_ = false;
			return false;
		}
		return true;
	}

	bool skip (std::size_t n)
	{
		if (!has (n))
			return false;
		p_ += n;
		return true;
	}

	std::uint8_t  u8 ()  { return fixed<std::uint8_t> (); }
	std::uint16_t u16 () { return fixed<std::uint16_t> (); }
	std::uint32_t u32 () { return fixed<std::uint32_t> (); }
	std::uint64_t u64 () { return fixed<std::uint64_t> (); }

	std::uint64_t uleb ();
	std::int64_t sleb ();

	std::optional<const std::uint8_t*> decode_pointer (std::uint8_t encoding);
	bool skip_encoded (std::uint8_t encoding);

private:
	template <typename T>
	T fixed ()
	{
		T v = 0;
		if (!has (sizeof (T)))
			return 0;
		std::memcpy (&v, p_, sizeof (T));
		p_ += sizeof (T);
		return v;
	}

	const std::uint8_t *start_ = nullptr;
	const std::uint8_t *p_ = nullptr;
	const std::uint8_t *end_ = nullptr;
	bool ok_ = false;
};

/*
 * LEB128. Besides bounds, the shift is clamped: an overlong encoding would
 * otherwise reach shift >= 64, where "res |= x << shift" is undefined.
 *
 * Both accumulate into std::uint64_t even though sleb() returns a signed value.
 * Shifting into a signed accumulator is undefined once the shifted value stops
 * being representable, which happens at shift == 63 - one short of the clamp
 * above - and the old "res |= -(1 << shift)" sign extension then negated
 * INT64_MIN, undefined again. Unsigned shifts and a single conversion at the end
 * produce the same bits without either. This matters because the operand is
 * attacker-shaped input and this TU is compiled without -fwrapv.
 *
 * What that buys is precisely "no longer UNDEFINED", not "fully defined". The
 * closing static_cast<std::int64_t> in sleb() converts an unsigned value that
 * may be out of the signed range, which C++17 leaves IMPLEMENTATION-DEFINED
 * ([conv.integral]/3) - GCC and clang both document it as reduction modulo
 * 2^64, i.e. exactly the bit pattern wanted here, and C++20 makes it fully
 * defined. Removing the undefined behaviour is the point and is sufficient;
 * std::bit_cast or std::memcpy would make it airtight and is not worth the
 * churn.
 */
std::uint64_t
Reader::uleb ()
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
			/* Significant bits past 64: not representable. */
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

std::int64_t
Reader::sleb ()
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

	/* Sign-extend: set every bit above the last operand bit. */
	if (shift < 64 && (b & 0x40))
		res |= ~static_cast<std::uint64_t> (0) << shift;

	return static_cast<std::int64_t> (res);
}

/*
 * Decode an FDE initial_location. LLVM emits DW_EH_PE_pcrel|DW_EH_PE_sdata4
 * (0x1b); absolute and pcrel/sdata8 are accepted too. Anything else declines -
 * including DW_EH_PE_indirect (bit 7), which would otherwise yield the address
 * OF a pointer rather than the pointer itself.
 */
std::optional<const std::uint8_t*>
Reader::decode_pointer (std::uint8_t encoding)
{
	const std::uint8_t *base = p_;
	std::int64_t val;

	if (encoding & DW_EH_PE_indirect)
		return std::nullopt;

	switch (encoding & 0x0f) {
	case DW_EH_PE_absptr:
		if (sizeof (void*) == 8)
			val = static_cast<std::int64_t> (u64 ());
		else
			val = static_cast<std::int32_t> (u32 ());
		break;
	case DW_EH_PE_sdata4:
		val = static_cast<std::int32_t> (u32 ());
		break;
	case DW_EH_PE_udata4:
		val = static_cast<std::int64_t> (u32 ());
		break;
	case DW_EH_PE_sdata8:
	case DW_EH_PE_udata8:
		val = static_cast<std::int64_t> (u64 ());
		break;
	default:
		return std::nullopt;
	}

	if (!ok_)
		return std::nullopt;

	switch (encoding & 0x70) {
	case DW_EH_PE_absptr:
		return reinterpret_cast<const std::uint8_t*> (static_cast<std::uintptr_t> (val));
	case DW_EH_PE_pcrel:
		return base + val;
	default:
		return std::nullopt;
	}
}

/* Skip the value an encoding describes, without interpreting it. */
bool
Reader::skip_encoded (std::uint8_t encoding)
{
	std::size_t n;

	switch (encoding & 0x0f) {
	case DW_EH_PE_absptr:
		n = sizeof (void*);
		break;
	case DW_EH_PE_sdata4:
	case DW_EH_PE_udata4:
		n = 4;
		break;
	case DW_EH_PE_sdata8:
	case DW_EH_PE_udata8:
		n = 8;
		break;
	default:
		return false;
	}

	return skip (n);
}

/* --------------------------------------------------------------- rules */

/*
 * Only dwarf regs mono knows about survive its unwinder: mono_unwind_frame()
 * silently DISCARDS a DW_CFA_offset for anything at or above NUM_DWARF_REGS.
 * Silently dropping a saved register is precisely the sort of thing that turns
 * into an unexplainable crash later, so a rule for such a register makes us
 * decline the method instead.
 *
 * The highest register mono maps is the return-address column (rip = 16 on
 * amd64), which is what mono_unwind_get_dwarf_pc_reg() reports, so that + 1 is
 * the count. Anything above it - notably the XMM registers - is declined. On
 * SysV amd64 there are no callee-saved XMMs, so this is a guard rather than a
 * workaround.
 */
bool
reg_ok (std::uint64_t reg)
{
	return reg <= static_cast<std::uint64_t> (mono_unwind_get_dwarf_pc_reg ()) &&
	       reg < static_cast<std::uint64_t> (MAX_DWARF_REG);
}

enum class RuleKind {
	/* No rule: the register is not recoverable from this frame. */
	Undefined,
	/* Register still holds its caller value. */
	Same,
	/* Register was spilled to cfa + offset. */
	Offset
};

struct RegRule {
	RuleKind kind = RuleKind::Undefined;
	int offset = 0; /* byte offset from the CFA, already scaled by data_align */

	friend bool operator== (const RegRule &a, const RegRule &b)
	{
		return a.kind == b.kind && a.offset == b.offset;
	}
	friend bool operator!= (const RegRule &a, const RegRule &b) { return !(a == b); }
};

struct CfiState {
	int cfa_reg = -1; /* dwarf register number, -1 if unset */
	int cfa_offset = 0;
	std::array<RegRule, MAX_DWARF_REG> regs {};
};

/*
 * factored * data_align, without the signed overflow.
 *
 * FACTORED is a signed LEB128 straight off the wire, so it can be any 64-bit
 * value, and signed overflow is undefined (this TU is compiled without -fwrapv).
 * transcode_fde() has already pinned data_align to
 * mono_unwind_get_dwarf_data_align() (-8 on amd64) before any CFI runs, so
 * |data_align| >= 1; therefore |factored| > 2^31 implies an exact product
 * outside 32 bits, which is outside BOTH accepted windows ([INT32_MIN, 0] for
 * the offset forms, [0, INT32_MAX] for the CFA _sf forms). Under exact
 * arithmetic this check therefore declines a strict subset of what the callers'
 * own range checks decline: anything it turns away, they would have too.
 *
 * 2^31 is the threshold of this CHECK, not the threshold at which the verdict
 * DIVERGES from the old unchecked multiply. Between 2^31 and a full 2^64 wrap
 * the two agree: the old product, though it had already overflowed, was still
 * outside every caller's 32-bit window and was rejected there. A verdict only
 * flips once the wrapped product lands back INSIDE an accepted window, which at
 * data_align = -8 first happens at |factored| = (2^64 - 2^31) / 8 = 2^61 - 2^28
 * (~2.3e18) - and 2,305,843,009,213,693,949, i.e. exactly that value, is the
 * smallest operand observed to change a verdict. Every such flip is an ACCEPT
 * the old code reached only through undefined behaviour.
 *
 * A zero data_align cannot overflow, so it is passed through for the caller to
 * judge.
 */
std::optional<std::int64_t>
scale_by_data_align (std::int64_t factored, int data_align)
{
	constexpr std::int64_t LIMIT = -static_cast<std::int64_t> (G_MININT32); /* 2^31 */

	if (data_align != 0 && (factored > LIMIT || factored < -LIMIT))
		return std::nullopt;

	return factored * static_cast<std::int64_t> (data_align);
}

/* Parsed CIE fields we care about. */
struct CieInfo {
	int code_align = 0;
	int data_align = 0;
	int return_reg = 0;
	std::uint8_t fde_ptr_encoding = DW_EH_PE_absptr; /* from the 'R' augmentation */
	bool has_augmentation_len = false;               /* 'z' */
	const std::uint8_t *cfi = nullptr;               /* start of the initial instructions */
	const std::uint8_t *cfi_end = nullptr;

	/*
	 * Convert a factored DWARF offset into the byte offset mono stores,
	 * rejecting anything that will not survive mono's encoding.
	 *
	 * A positive byte offset is rejected outright: mono encodes the factored
	 * value with encode_uleb128(), which takes a guint32, so a register saved
	 * above the CFA would encode as a huge unsigned value that
	 * mono_unwind_frame() multiplies back out and dereferences - a wild read in
	 * async-signal context during GC stack scanning.
	 *
	 * Nothing here has to check that the byte offset re-factors exactly.
	 * mono_unwind_ops_encode_full() re-factors with TRUNCATING integer division
	 * (op->val / DWARF_DATA_ALIGN), so the round trip only cancels for exact
	 * multiples of mono's own alignment - but the product below is a multiple of
	 * data_align by construction, and transcode_fde() has already required
	 * data_align to BE mono's DWARF_DATA_ALIGN before any of this runs. That
	 * check, not one here, is what makes the round trip exact.
	 */
	std::optional<int> factored_to_byte_offset (std::int64_t factored) const
	{
		/*
		 * Defensive, not load-bearing: an interpreter is only ever constructed
		 * after transcode_fde() has required data_align to equal
		 * mono_unwind_get_dwarf_data_align(), which is non-zero. Kept because it
		 * costs nothing and keeps the function correct in isolation.
		 */
		if (data_align == 0)
			return std::nullopt;

		std::optional<std::int64_t> bytes = scale_by_data_align (factored, data_align);
		if (!bytes)
			return std::nullopt;

		if (*bytes > 0 || *bytes < G_MININT32)
			return std::nullopt;

		return static_cast<int> (*bytes);
	}

	/*
	 * Parse the CIE whose *contents* (starting at the version byte) span
	 * [start, end).
	 */
	static std::optional<CieInfo> parse (const std::uint8_t *start, const std::uint8_t *end);

	/*
	 * Locate and parse the CIE entry beginning at CIE_START, which an FDE
	 * reached by a backward offset. SECTION_END bounds the whole .eh_frame.
	 */
	static std::optional<CieInfo> locate (const std::uint8_t *cie_start,
	                                      const std::uint8_t *section_end);
};

/* ------------------------------------------------------------ op list */

/*
 * The unwind ops built for one method, owning the GSList until it is handed to
 * the caller. Anything that declines part-way simply lets this go out of scope;
 * there is no path that leaks the partially built list or publishes it.
 */
class UnwindOps {
public:
	UnwindOps () = default;
	/*
	 * Neither copyable nor movable: it is built in place and handed over with
	 * release(). A move constructor was written for a version that returned it
	 * by value; nothing needs it now, and leaving an unused one on a resource
	 * owner is a hazard rather than a convenience.
	 */
	UnwindOps (const UnwindOps &) = delete;
	UnwindOps &operator= (const UnwindOps &) = delete;

	~UnwindOps ()
	{
		if (list_)
			mono_free_unwind_info (list_);
	}

	void emit (std::uint32_t when, int op, int dwarf_reg, int val)
	{
		int hwreg = dwarf_reg >= 0 ? mono_dwarf_reg_to_hw_reg (dwarf_reg) : 0;

		list_ = g_slist_append (list_, mono_create_unwind_op (when, op, hwreg, val));
		count_ ++;
	}

	int size () const { return count_; }

	/* Give up ownership to the caller. */
	GSList *release ()
	{
		GSList *l = list_;
		list_ = nullptr;
		count_ = 0;
		return l;
	}

private:
	GSList *list_ = nullptr;
	int count_ = 0;
};

/* ------------------------------------------------------- CFI interpreter */

/*
 * Executes one CFI program against a CfiState.
 *
 * If OPS is null the run only updates the state, which is how the CIE's initial
 * instructions are interpreted; otherwise every state change is also appended to
 * OPS as a MonoUnwindOp.
 *
 * INITIAL supplies the CIE rules that DW_CFA_restore reverts to; it is null
 * while the CIE itself is being interpreted, which makes restore a decline
 * there rather than a lookup into rules that do not exist yet.
 */
class CfiInterpreter {
public:
	CfiInterpreter (const CieInfo &cie, CfiState &state, const CfiState *initial,
	                UnwindOps *ops, std::uint32_t code_len)
		: cie_ (cie), state_ (state), initial_ (initial), ops_ (ops), code_len_ (code_len)
	{
	}

	bool run (const std::uint8_t *start, const std::uint8_t *end);

private:
	/*
	 * Handle one opcode. Returns false only for an opcode this cannot represent.
	 * A truncated operand returns true: run() checks the reader once after the
	 * loop, so it declines regardless. See convention 9 in the file header.
	 */
	bool step (std::uint8_t opcode);
	bool extended (std::uint8_t opcode);

	/*
	 * Advance the current code offset by DELTA code-alignment units.
	 *
	 * Computed unsigned: code_align comes off the wire, so delta * code_align
	 * overflows int for a large one. loc_ is 32-bit and every opcode is followed
	 * by the loc_ > code_len_ check in run(), which is what catches a wrapped or
	 * runaway offset.
	 */
	void advance (std::uint32_t delta)
	{
		loc_ += delta * static_cast<std::uint32_t> (cie_.code_align);
	}

	/*
	 * Append an op at the current code offset. Named for the location it
	 * supplies, so it does not read as an overload of UnwindOps::emit(), which
	 * takes the location explicitly.
	 */
	void emit_at_loc (int op, int dwarf_reg, int val)
	{
		if (ops_)
			ops_->emit (loc_, op, dwarf_reg, val);
	}

	/* Emit whatever concretely represents RULE for REG at the current location. */
	void emit_rule (int reg, const RegRule &rule)
	{
		if (rule.kind == RuleKind::Offset)
			emit_at_loc (DW_CFA_offset, reg, rule.offset);
		else
			/*
			 * Not saved by this frame. same_value leaves mono's locations[] as
			 * LOC_SAME, so the register is not restored - which is what both
			 * "reverted an epilogue's spill" and "explicitly undefined" mean as
			 * far as mono is able to express them.
			 */
			emit_at_loc (DW_CFA_same_value, reg, 0);
	}

	const CieInfo &cie_;
	CfiState &state_;
	const CfiState *initial_;
	UnwindOps *ops_;
	std::uint32_t code_len_;

	Reader r_;
	std::uint32_t loc_ = 0;
	/*
	 * DW_CFA_remember_state/restore_state are simulated here and never emitted:
	 * mono_unwind_frame()'s state stack is literally UnwindState state_stack[1]
	 * and fails outright at depth >= 2. Materializing the restored rules instead
	 * lifts that limit, which is the whole point - the depth mono can express is
	 * not the depth we can accept.
	 *
	 * Not, however, unbounded. remember_state emits nothing, so MAX_UNWIND_OPS
	 * never fires on it: without MAX_REMEMBER_DEPTH a run of 0x0a bytes would
	 * push one ~264-byte CfiState each, letting a 1 MB .eh_frame cost a few
	 * hundred MB before anything declined. The input is parsed as untrusted, so
	 * it does not get to choose how much we allocate.
	 */
	std::vector<CfiState> stack_;
};

bool
CfiInterpreter::run (const std::uint8_t *start, const std::uint8_t *end)
{
	bool ok = true;

	/*
	 * Reset rather than assume a fresh object: nothing stops a caller running
	 * two programs through one interpreter, and leftover loc_/stack_ would be a
	 * silent wrong answer rather than a crash.
	 */
	r_ = Reader (start, end);
	loc_ = 0;
	stack_.clear ();

	while (ok && r_.ok () && !r_.at_end ()) {
		/*
		 * The loop condition established ok() and at least one byte remaining,
		 * so this read cannot fail; every read of an OPERAND can, and those are
		 * checked in step().
		 */
		std::uint8_t b = r_.u8 ();

		if (ops_ && ops_->size () > MAX_UNWIND_OPS) {
			ok = false;
			break;
		}

		ok = step (b);

		/*
		 * Checked after every opcode, not only the extended ones: advance_loc is
		 * the common case and is exactly what can run past the function.
		 */
		if (loc_ > code_len_)
			ok = false;
	}

	return ok && r_.ok ();
}

bool
CfiInterpreter::step (std::uint8_t b)
{
	const int primary = b & 0xc0;
	const int operand = b & 0x3f;

	if (primary == DW_CFA_advance_loc) {
		advance (static_cast<std::uint32_t> (operand));
		return true;
	}

	if (primary == DW_CFA_offset) {
		std::int64_t factored = static_cast<std::int64_t> (r_.uleb ());

		if (!r_.ok ())
			return true;
		if (!reg_ok (operand))
			return false;

		std::optional<int> bytes = cie_.factored_to_byte_offset (factored);
		if (!bytes)
			return false;

		state_.regs [operand] = { RuleKind::Offset, *bytes };
		emit_at_loc (DW_CFA_offset, operand, *bytes);
		return true;
	}

	if (primary == DW_CFA_restore) {
		/*
		 * "Revert to the CIE rule." This is the opcode a byte-copier gets wrong,
		 * and that mono_unwind_frame() cannot execute, so resolve it to the
		 * concrete rule here.
		 */
		if (!reg_ok (operand) || !initial_)
			return false;
		state_.regs [operand] = initial_->regs [operand];
		emit_rule (operand, state_.regs [operand]);
		return true;
	}

	/* primary == 0: extended opcode */
	return extended (b);
}

bool
CfiInterpreter::extended (std::uint8_t b)
{
	switch (b) {
	case DW_CFA_nop:
		return true;

	case DW_CFA_set_loc:
		/* Absolute location; we only track offsets, so decline. */
		return false;

	case DW_CFA_advance_loc1:
		advance (r_.u8 ());
		return true;

	case DW_CFA_advance_loc2:
		advance (r_.u16 ());
		return true;

	case DW_CFA_advance_loc4:
		advance (r_.u32 ());
		return true;

	case DW_CFA_def_cfa: {
		std::uint64_t reg = r_.uleb ();
		std::uint64_t off = r_.uleb ();

		if (!r_.ok ())
			return true;
		if (!reg_ok (reg) || off > G_MAXINT32)
			return false;

		state_.cfa_reg = static_cast<int> (reg);
		state_.cfa_offset = static_cast<int> (off);
		emit_at_loc (DW_CFA_def_cfa, static_cast<int> (reg), static_cast<int> (off));
		return true;
	}

	case DW_CFA_def_cfa_register: {
		std::uint64_t reg = r_.uleb ();

		if (!r_.ok ())
			return true;
		if (!reg_ok (reg))
			return false;

		state_.cfa_reg = static_cast<int> (reg);
		emit_at_loc (DW_CFA_def_cfa_register, static_cast<int> (reg), 0);
		return true;
	}

	case DW_CFA_def_cfa_offset: {
		std::uint64_t off = r_.uleb ();

		if (!r_.ok ())
			return true;
		if (off > G_MAXINT32)
			return false;

		state_.cfa_offset = static_cast<int> (off);
		emit_at_loc (DW_CFA_def_cfa_offset, -1, static_cast<int> (off));
		return true;
	}

	case DW_CFA_def_cfa_sf: {
		std::uint64_t reg = r_.uleb ();
		std::int64_t off = r_.sleb ();
		std::optional<std::int64_t> bytes = scale_by_data_align (off, cie_.data_align);

		if (!r_.ok ())
			return true;
		/* mono encodes the CFA offset unsigned, so it must be >= 0. */
		if (!reg_ok (reg) || !bytes || *bytes < 0 || *bytes > G_MAXINT32)
			return false;

		state_.cfa_reg = static_cast<int> (reg);
		state_.cfa_offset = static_cast<int> (*bytes);
		emit_at_loc (DW_CFA_def_cfa, static_cast<int> (reg), static_cast<int> (*bytes));
		return true;
	}

	case DW_CFA_def_cfa_offset_sf: {
		std::int64_t off = r_.sleb ();
		std::optional<std::int64_t> bytes = scale_by_data_align (off, cie_.data_align);

		if (!r_.ok ())
			return true;
		if (!bytes || *bytes < 0 || *bytes > G_MAXINT32)
			return false;

		state_.cfa_offset = static_cast<int> (*bytes);
		emit_at_loc (DW_CFA_def_cfa_offset, -1, static_cast<int> (*bytes));
		return true;
	}

	case DW_CFA_offset_extended:
	case DW_CFA_offset_extended_sf: {
		std::uint64_t reg = r_.uleb ();
		std::int64_t factored = (b == DW_CFA_offset_extended)
			? static_cast<std::int64_t> (r_.uleb ()) : r_.sleb ();

		if (!r_.ok ())
			return true;
		if (!reg_ok (reg))
			return false;

		std::optional<int> bytes = cie_.factored_to_byte_offset (factored);
		if (!bytes)
			return false;

		state_.regs [reg] = { RuleKind::Offset, *bytes };
		emit_at_loc (DW_CFA_offset, static_cast<int> (reg), *bytes);
		return true;
	}

	case DW_CFA_restore_extended: {
		std::uint64_t reg = r_.uleb ();

		if (!r_.ok ())
			return true;
		if (!reg_ok (reg) || !initial_)
			return false;

		state_.regs [reg] = initial_->regs [reg];
		emit_rule (static_cast<int> (reg), state_.regs [reg]);
		return true;
	}

	case DW_CFA_same_value: {
		std::uint64_t reg = r_.uleb ();

		if (!r_.ok ())
			return true;
		if (!reg_ok (reg))
			return false;

		state_.regs [reg].kind = RuleKind::Same;
		emit_at_loc (DW_CFA_same_value, static_cast<int> (reg), 0);
		return true;
	}

	case DW_CFA_undefined: {
		std::uint64_t reg = r_.uleb ();

		if (!r_.ok ())
			return true;
		if (!reg_ok (reg))
			return false;

		/*
		 * "This register is not recoverable from here." mono has no such rule,
		 * but it MUST still be emitted rather than merely recorded: the CIE's
		 * rules are restated at offset 0, and on x86-64 those include "pc at
		 * cfa-8". An FDE declaring the return column undefined - which is how a
		 * frame says "stop unwinding here" - would otherwise leave that stale,
		 * and mono_unwind_frame would load a return address from a slot that no
		 * longer holds one. same_value is the closest mono can express: not
		 * restored.
		 */
		state_.regs [reg].kind = RuleKind::Undefined;
		emit_at_loc (DW_CFA_same_value, static_cast<int> (reg), 0);
		return true;
	}

	case DW_CFA_remember_state:
		if (stack_.size () >= MAX_REMEMBER_DEPTH)
			return false;
		stack_.push_back (state_);
		return true;

	case DW_CFA_restore_state: {
		if (stack_.empty ())
			return false;

		const CfiState saved = stack_.back ();
		stack_.pop_back ();

		/*
		 * A restored state with no CFA rule cannot be expressed, and emitting
		 * def_cfa with reg -1 would fabricate a rule on r0.
		 */
		if (saved.cfa_reg == -1)
			return false;

		/* Materialize the differences; emit neither opcode. */
		if (saved.cfa_reg != state_.cfa_reg || saved.cfa_offset != state_.cfa_offset)
			emit_at_loc (DW_CFA_def_cfa, saved.cfa_reg, saved.cfa_offset);
		for (int i = 0; i < MAX_DWARF_REG; ++i) {
			if (saved.regs [i] != state_.regs [i])
				emit_rule (i, saved.regs [i]);
		}

		state_ = saved;
		return true;
	}

	case DW_CFA_GNU_args_size:
		/* Stack adjustment for calls; irrelevant to mono's unwinder. */
		r_.uleb ();
		return true;

	/*
	 * Expressions would need a DWARF expression evaluator in async-signal
	 * context. Decline instead - the method falls back to the classic JIT.
	 */
	case DW_CFA_def_cfa_expression:
	case DW_CFA_expression:
	case DW_CFA_val_expression:
	default:
		return false;
	}
}

/* --------------------------------------------------------------- CIE */

std::optional<CieInfo>
CieInfo::parse (const std::uint8_t *start, const std::uint8_t *end)
{
	CieInfo cie;
	Reader r (start, end);

	std::uint8_t version = r.u8 ();
	if (!r.ok () || (version != 1 && version != 3))
		return std::nullopt;

	/*
	 * Bounded scan for the augmentation string's terminator. A plain strlen()
	 * here would run off a CIE truncated after the version byte.
	 */
	const std::uint8_t *aug_str = r.pos ();
	std::size_t aug_str_len = 0;
	for (;;) {
		std::uint8_t c = r.u8 ();
		if (!r.ok ())
			return std::nullopt;
		if (c == '\0')
			break;
		aug_str_len ++;
		if (aug_str_len > 16) /* no legitimate augmentation is this long */
			return std::nullopt;
	}

	/*
	 * Decoded at their wire width and range-checked BEFORE they are narrowed to
	 * the int fields below.
	 *
	 * Every check that polices these three - code_align > 0, data_align ==
	 * mono_unwind_get_dwarf_data_align (), return_reg ==
	 * mono_unwind_get_dwarf_pc_reg () - runs in transcode_fde() on the already
	 * narrowed int. A static_cast<int> here would therefore let any 64-bit
	 * operand whose low 32 bits happen to ALIAS an acceptable value walk past
	 * all three: sleb 0xffffffff8 (2^32 - 8) and sleb -(2^32 + 8) both narrow
	 * to -8 and were accepted as "mono's data alignment"; uleb 2^32 + 1 narrowed
	 * to code_align 1; uleb 2^32 + 16 narrowed to return column 16. The operands
	 * come off the wire, so that is a header-field check defeated by a cast.
	 *
	 * Out of range is a decline rather than a clamp: a CIE that cannot mean what
	 * it says is not a CIE we should be transcoding, and every legitimate value
	 * here is tiny (1, -8, 16 on amd64).
	 */
	std::uint64_t code_align = r.uleb ();
	std::int64_t data_align = r.sleb ();
	std::uint64_t return_reg = (version == 1) ? r.u8 () : r.uleb ();
	if (!r.ok ())
		return std::nullopt;
	if (code_align > static_cast<std::uint64_t> (G_MAXINT32) ||
	    data_align > G_MAXINT32 || data_align < G_MININT32 ||
	    return_reg > static_cast<std::uint64_t> (G_MAXINT32))
		return std::nullopt;

	cie.code_align = static_cast<int> (code_align);
	cie.data_align = static_cast<int> (data_align);
	cie.return_reg = static_cast<int> (return_reg);

	if (aug_str_len > 0 && aug_str [0] == 'z') {
		std::uint64_t aug_len = r.uleb ();

		if (!r.ok () || aug_len > r.remaining ())
			return std::nullopt;
		cie.has_augmentation_len = true;

		const std::uint8_t *aug_end = r.pos () + aug_len;

		for (std::size_t i = 1; i < aug_str_len; ++i) {
			switch (aug_str [i]) {
			case 'R':
				cie.fde_ptr_encoding = r.u8 ();
				break;
			case 'P': {
				std::uint8_t enc = r.u8 ();
				if (!r.ok () || !r.skip_encoded (enc))
					return std::nullopt;
				break;
			}
			case 'L':
				r.u8 (); /* LSDA encoding; the LSDA itself is not needed here */
				break;
			case 'S':
				/* Signal frame; no extra data. */
				break;
			default:
				return std::nullopt;
			}
			if (!r.ok ())
				return std::nullopt;
		}
		r.seek (aug_end);
	} else if (aug_str_len > 0) {
		/* Augmentation without a length field: cannot skip it safely. */
		return std::nullopt;
	}

	if (!r.ok ())
		return std::nullopt;

	cie.cfi = r.pos ();
	cie.cfi_end = end;
	return cie;
}

std::optional<CieInfo>
CieInfo::locate (const std::uint8_t *cie_start, const std::uint8_t *section_end)
{
	Reader r (cie_start, section_end);

	std::uint32_t cie_len = r.u32 ();
	if (!r.ok () || cie_len == 0 || cie_len == 0xffffffff)
		return std::nullopt;
	/*
	 * The FDE's own extent was bounded by the section scan; the CIE is reached
	 * by a backward offset and needs the same check, or a bogus pointer lets
	 * parse() succeed on unrelated memory and the interpreter read megabytes of
	 * it as a CFI program.
	 */
	if (cie_len > r.remaining ())
		return std::nullopt;

	const std::uint8_t *cie_contents_end = r.pos () + cie_len;

	/* Skip the CIE id field; parse() starts at the version byte. */
	if (!r.skip (4))
		return std::nullopt;

	return parse (r.pos (), cie_contents_end);
}

/* --------------------------------------------------------------- FDE */

/*
 * Three outcomes, which is why this is not an optional: an FDE for a different
 * function is an ordinary event while scanning (the GC safepoint poll rides
 * along with the method), and must not be confused with a decline.
 */
enum class FdeResult {
	Transcoded,
	NotThisFunction,
	Decline
};

/*
 * Transcode the FDE whose contents span [fde_start, fde_end) - i.e. starting
 * just after its CIE-pointer field - if it describes CODE_START.
 */
FdeResult
transcode_fde (const std::uint8_t *fde_start, const std::uint8_t *fde_end,
               const CieInfo &cie, const std::uint8_t *code_start,
               std::uint32_t code_len, UnwindOps &out)
{
	Reader r (fde_start, fde_end);

	std::optional<const std::uint8_t*> pc_begin = r.decode_pointer (cie.fde_ptr_encoding);
	/*
	 * has_value(), not "!pc_begin": the optional wraps a POINTER, so the terse
	 * spelling reads as a null check when it means "could not decode". A null
	 * pc_begin is itself representable (DW_EH_PE_absptr 0) and is a legitimate
	 * value here, not an error - it simply will not match code_start.
	 */
	if (!pc_begin.has_value ())
		return FdeResult::Decline;
	/* pc_range, same size class as the pointer, but never relocated. */
	if (!r.skip_encoded (cie.fde_ptr_encoding))
		return FdeResult::Decline;

	if (cie.has_augmentation_len) {
		std::uint64_t aug_len = r.uleb ();
		if (!r.ok () || aug_len > r.remaining ())
			return FdeResult::Decline;
		r.seek (r.pos () + aug_len);
	}
	if (!r.ok ())
		return FdeResult::Decline;

	if (*pc_begin != code_start)
		return FdeResult::NotThisFunction;

	/*
	 * mono re-factors register offsets with TRUNCATING division by its own
	 * DWARF_DATA_ALIGN, so that must be the alignment they were produced with or
	 * slots silently shift (a -4 CIE would turn cfa-12 into cfa-8). code_align is
	 * applied to every advance_loc, so any positive value works; LLVM emits 1 on
	 * x86-64.
	 */
	if (cie.code_align <= 0 || cie.data_align >= 0)
		return FdeResult::Decline;
	if (cie.data_align != mono_unwind_get_dwarf_data_align ())
		return FdeResult::Decline;
	/*
	 * The return-address column has to be the one mono restores as the caller's
	 * pc, or the unwound frame would resume at the wrong place.
	 */
	if (cie.return_reg != mono_unwind_get_dwarf_pc_reg ())
		return FdeResult::Decline;

	CfiState initial;
	CfiInterpreter cie_run (cie, initial, nullptr, nullptr, code_len);
	if (!cie_run.run (cie.cfi, cie.cfi_end))
		return FdeResult::Decline;

	/*
	 * Without a CFA rule there is nothing to unwind with. Returning a
	 * well-formed-looking op list here would defer the failure to exception
	 * dispatch; decline at compile time instead.
	 */
	if (initial.cfa_reg == -1)
		return FdeResult::Decline;

	/*
	 * Restate the CIE rules at offset 0 so the descriptor stands alone: mono's
	 * unwinder is handed only this op list, never the CIE.
	 */
	CfiState state = initial;
	out.emit (0, DW_CFA_def_cfa, initial.cfa_reg, initial.cfa_offset);
	for (int i = 0; i < MAX_DWARF_REG; ++i) {
		if (initial.regs [i].kind == RuleKind::Offset)
			out.emit (0, DW_CFA_offset, i, initial.regs [i].offset);
	}

	CfiInterpreter fde_run (cie, state, &initial, &out, code_len);
	if (!fde_run.run (r.pos (), fde_end) || out.size () > MAX_UNWIND_OPS)
		return FdeResult::Decline;

	return FdeResult::Transcoded;
}

} /* anonymous namespace */

/* --------------------------------------------------------- entry point */

gboolean
mono_llvm_eh_frame_to_unwind_ops (guint8 *eh_frame, guint32 eh_frame_size,
                                  gpointer code_start, guint32 code_len,
                                  GSList **out_ops)
{
	if (!out_ops)
		return FALSE;
	*out_ops = NULL;

	if (!eh_frame || eh_frame_size < 4)
		return FALSE;

	const std::uint8_t *section = reinterpret_cast<const std::uint8_t*> (eh_frame);
	const std::uint8_t *section_end = section + eh_frame_size;
	Reader sec (section, section_end);

	while (sec.ok () && sec.remaining () >= 4) {
		std::uint32_t length = sec.u32 ();
		if (!sec.ok ())
			return FALSE;

		/* A zero-length entry terminates the section. */
		if (length == 0)
			break;
		/* 64-bit DWARF is not produced by LLVM here; decline rather than guess. */
		if (length == 0xffffffff)
			return FALSE;
		if (length > sec.remaining ())
			return FALSE;

		const std::uint8_t *entry_end = sec.pos () + length;
		const std::uint8_t *id_pos = sec.pos ();
		/*
		 * The id must be read through the bounds-checked reader: an entry
		 * claiming length 1 satisfies the check above but has only one byte.
		 */
		std::uint32_t id = sec.u32 ();
		if (!sec.ok ())
			return FALSE;

		if (id == 0) {
			/* CIE: skip; FDEs locate their own CIE below. */
			sec.seek (entry_end);
			continue;
		}

		/*
		 * FDE. In .eh_frame the second word is the distance BACK from its own
		 * position to the CIE (unlike .debug_frame, where it is a section
		 * offset).
		 */
		if (static_cast<std::size_t> (id) > static_cast<std::size_t> (id_pos - section))
			return FALSE;

		std::optional<CieInfo> cie = CieInfo::locate (id_pos - id, section_end);
		if (!cie)
			return FALSE;

		UnwindOps ops;
		FdeResult res = transcode_fde (sec.pos (), entry_end, *cie,
		                               reinterpret_cast<const std::uint8_t*> (code_start),
		                               code_len, ops);

		/*
		 * No default: label, deliberately. Every arm below returns or continues,
		 * so an unhandled enumerator would fall out of the switch and re-enter
		 * the scan loop at id_pos + 4, silently misparsing the section. What
		 * stops that is now -Wswitch (see convention 3 in the file header):
		 * without a default: label the compiler names the missing enumerator at
		 * the point it is added. Adding a default: back would suppress exactly
		 * that diagnostic.
		 *
		 * The g_assert_not_reached () after the switch is what a default: arm
		 * used to be, minus the suppression: -Wswitch is a warning, not an
		 * error, and mono's build is far too noisy for one warning to be certain
		 * of being read, so the runtime trap stays. It is unreachable today and
		 * shows as such under gcov.
		 */
		switch (res) {
		case FdeResult::Transcoded:
			*out_ops = ops.release ();
			return TRUE;
		case FdeResult::NotThisFunction:
			/*
			 * A different function in the same module - the GC safepoint poll
			 * rides along with the method, so this is normal.
			 */
			sec.seek (entry_end);
			continue;
		case FdeResult::Decline:
			return FALSE;
		}
		g_assert_not_reached ();
	}

	/*
	 * No FDE for this function. LLVM omits one for a nounwind leaf. We cannot
	 * tell that apart from a lookup failure here, and publishing a frame with no
	 * unwind information is exactly the unsafe outcome this exists to prevent,
	 * so decline and let the classic JIT take the method.
	 */
	return FALSE;
}

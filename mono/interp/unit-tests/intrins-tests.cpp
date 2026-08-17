/*
 * The bodies behind the MINT_INTRINS_* opcodes.
 *
 * Each case checks against a reference written out longhand rather than a value
 * read off the implementation, so a body and its check cannot drift together.
 * Marvin is the exception: it is a mixing function with no simpler statement, so
 * it gets known answers computed away from this tree.
 */

#include <cstdint>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "harness.hpp"
#include "mono/interp/runtime/intrins.hpp"

using namespace mono::interp;

namespace {

/* Two UTF-16 characters packed into a word, low half first, the way the
 * Utf16Utility bodies read them. */
constexpr guint32
pack2 (char16_t lo, char16_t hi)
{
	return (guint32) lo | ((guint32) hi << 16);
}

constexpr guint64
pack4 (char16_t a, char16_t b, char16_t c, char16_t d)
{
	return (guint64) pack2 (a, b) | ((guint64) pack2 (c, d) << 32);
}

char16_t
upper_ascii (char16_t c)
{
	return (c >= u'a' && c <= u'z') ? (char16_t) (c - u'a' + u'A') : c;
}

} // namespace

TEST (Intrins, MarvinBlockMatchesKnownAnswers)
{
	struct Vector {
		guint32 p0, p1, want0, want1;
	};
	static const Vector vectors[] = {
		{0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u},
		{0x00000001u, 0x00000000u, 0x08108201u, 0x10080080u},
		{0x00000000u, 0x00000001u, 0x08000201u, 0x10080000u},
		{0x12345678u, 0x9ABCDEF0u, 0x5099083Au, 0xD6E708C5u},
		{0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFEu, 0xFFFFFFFFu},
	};

	for (const Vector &v : vectors) {
		guint32 p0 = v.p0, p1 = v.p1;
		marvin_block (&p0, &p1);
		EXPECT_EQ (p0, v.want0) << "p0 for " << v.p0 << "," << v.p1;
		EXPECT_EQ (p1, v.want1) << "p1 for " << v.p0 << "," << v.p1;
	}
}

/* The rotate is by a non-zero amount everywhere it is used. A rotate of 0 would
 * shift a guint32 by 32, which is undefined, so nothing may ask for one. */
TEST (Intrins, MarvinBlockIsAPermutation)
{
	guint32 a0 = 0x0BADF00Du, a1 = 0xFEEDFACEu;
	guint32 b0 = 0x0BADF00Du, b1 = 0xFEEDFACFu; // one bit apart

	marvin_block (&a0, &a1);
	marvin_block (&b0, &b1);

	EXPECT_NE (a0, b0);
	EXPECT_NE (a1, b1);
}

TEST (Intrins, AsciiCharsToUppercaseUppercasesBothHalves)
{
	for (char16_t lo = 0; lo < 0x80; lo++) {
		for (char16_t hi : {u'a', u'z', u'A', u'Z', u'0', u'{', u'`'}) {
			guint32 got = ascii_chars_to_uppercase (pack2 (lo, hi));
			EXPECT_EQ (got, pack2 (upper_ascii (lo), upper_ascii (hi)))
				<< "lo=" << (int) lo << " hi=" << (int) hi;
		}
	}
}

/* The body brackets the letter range with an add and a carry test, so the
 * characters either side of 'a' and 'z' are where an off-by-one would show. */
TEST (Intrins, AsciiCharsToUppercaseLeavesNonLettersAlone)
{
	for (char16_t c : {u'`', u'{', u'@', u'[', u'0', u'9', u' ', u'\x7f'}) {
		EXPECT_EQ (ascii_chars_to_uppercase (pack2 (c, c)), pack2 (c, c))
			<< "c=" << (int) c;
	}
}

TEST (Intrins, OrdinalIgnoreCaseAscii32MatchesPerCharacter)
{
	static const char16_t interesting[] = {u'a', u'z', u'A', u'Z', u'`',  u'{',
	                                       u'@', u'[', u'0', u'9', u'_'};

	for (char16_t a : interesting) {
		for (char16_t b : interesting) {
			bool want = upper_ascii (a) == upper_ascii (b);
			EXPECT_EQ (ordinal_ignore_case_ascii (pack2 (a, a), pack2 (b, b)) != 0, want)
				<< "a=" << (int) a << " b=" << (int) b;
		}
	}
}

/* Both halves have to agree: a word that matches only in its low half is not a
 * match. */
TEST (Intrins, OrdinalIgnoreCaseAscii32NeedsBothHalves)
{
	EXPECT_NE (ordinal_ignore_case_ascii (pack2 (u'a', u'b'), pack2 (u'A', u'B')), 0);
	EXPECT_EQ (ordinal_ignore_case_ascii (pack2 (u'a', u'b'), pack2 (u'A', u'C')), 0);
	EXPECT_EQ (ordinal_ignore_case_ascii (pack2 (u'a', u'b'), pack2 (u'C', u'B')), 0);
}

TEST (Intrins, OrdinalIgnoreCaseAscii64MatchesPerCharacter)
{
	static const char16_t interesting[] = {u'a', u'z', u'A', u'Z', u'`', u'{', u'0'};

	for (char16_t a : interesting) {
		for (char16_t b : interesting) {
			bool want = upper_ascii (a) == upper_ascii (b);
			EXPECT_EQ (ordinal_ignore_case_ascii (pack4 (a, a, a, a), pack4 (b, b, b, b)) != 0,
			           want)
				<< "a=" << (int) a << " b=" << (int) b;
		}
	}
}

/* Each of the four characters has to agree, so a difference is checked at every
 * position rather than only at the ends. */
TEST (Intrins, OrdinalIgnoreCaseAscii64NeedsEveryCharacter)
{
	const guint64 base = pack4 (u'a', u'b', u'c', u'd');

	EXPECT_NE (ordinal_ignore_case_ascii (base, pack4 (u'A', u'B', u'C', u'D')), 0);
	EXPECT_EQ (ordinal_ignore_case_ascii (base, pack4 (u'X', u'B', u'C', u'D')), 0);
	EXPECT_EQ (ordinal_ignore_case_ascii (base, pack4 (u'A', u'X', u'C', u'D')), 0);
	EXPECT_EQ (ordinal_ignore_case_ascii (base, pack4 (u'A', u'B', u'X', u'D')), 0);
	EXPECT_EQ (ordinal_ignore_case_ascii (base, pack4 (u'A', u'B', u'C', u'X')), 0);
}

TEST (Intrins, WidenAsciiToUtf16CopiesEveryAsciiByte)
{
	std::vector<guint8> in;
	for (int i = 0; i < 0x80; i++)
		in.push_back ((guint8) i);
	std::vector<mono_unichar2> out (in.size (), 0xFFFF);

	mono_u done = widen_ascii_to_utf16 (in.data (), out.data (), in.size ());

	ASSERT_EQ (done, in.size ());
	for (size_t i = 0; i < in.size (); i++)
		EXPECT_EQ (out[i], (mono_unichar2) in[i]) << "at " << i;
}

/* It stops at the first byte with the high bit set and reports how far it got,
 * leaving the rest for the caller. */
TEST (Intrins, WidenAsciiToUtf16StopsAtTheFirstNonAscii)
{
	for (size_t stop = 0; stop < 8; stop++) {
		std::vector<guint8> in (8, 'x');
		in[stop] = 0x80;
		std::vector<mono_unichar2> out (in.size (), 0xFFFF);

		EXPECT_EQ (widen_ascii_to_utf16 (in.data (), out.data (), in.size ()), stop);
		for (size_t i = 0; i < stop; i++)
			EXPECT_EQ (out[i], u'x') << "at " << i;
		EXPECT_EQ (out[stop], 0xFFFF) << "wrote past the stop at " << stop;
	}
}

TEST (Intrins, WidenAsciiToUtf16AcceptsAnEmptyBuffer)
{
	guint8 in = 0x80;
	mono_unichar2 out = 0xFFFF;

	EXPECT_EQ (widen_ascii_to_utf16 (&in, &out, 0), (mono_u) 0);
	EXPECT_EQ (out, 0xFFFF);
}

/*
 * u32_to_decstr allocates, so it needs a booted runtime and a MonoString
 * vtable. The one-digit values return out of the caller's cache before the
 * body does any work, so passing no cache is safe for everything above nine.
 */
TEST (Intrins, U32ToDecStrMatchesToString)
{
	MONO_SKIP_WITHOUT_CORPUS ();
	mono::test::init_runtime ();

	ERROR_DECL (error);
	MonoVTable *vtable =
		mono_class_vtable_checked (mono_get_root_domain (), mono_defaults.string_class, error);
	ASSERT_TRUE (is_ok (error));
	ASSERT_NE (vtable, nullptr);

	static const guint32 values[] = {10u,      11u,        99u,        100u,       999u,
	                                 1000u,    9999u,      10000u,     99999u,     100000u,
	                                 999999u,  1000000u,   99999999u,  100000000u, 999999999u,
	                                 1000000000u, 4294967294u, 4294967295u};

	for (guint32 v : values) {
		MonoString *s = u32_to_decstr (v, nullptr, vtable);
		ASSERT_NE (s, nullptr) << "for " << v;

		std::string want = std::to_string (v);
		ASSERT_EQ ((size_t) mono_string_length_internal (s), want.size ()) << "length for " << v;

		const mono_unichar2 *chars = mono_string_chars_internal (s);
		for (size_t i = 0; i < want.size (); i++)
			EXPECT_EQ (chars[i], (mono_unichar2) want[i]) << "digit " << i << " of " << v;
	}
}

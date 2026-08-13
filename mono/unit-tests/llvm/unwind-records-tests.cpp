/*
 * Tests for parse_unwind_records () in mono/llvm/sidetables.hpp, which reads the
 * `.mono_unwind` block describing one function out of the section the compiler
 * emits next to the code.
 *
 * Two readers depend on it: jinfo.cpp, which builds the MonoJitInfo the runtime
 * unwinder walks, and the perf jit dump, which turns the records into DWARF. A
 * record read wrong there does not stop a walk, it continues it to a wrong
 * frame, so the refusals matter as much as the values.
 *
 * Everything here drives it with byte buffers. The layout below is written from
 * the constants in sidetables.hpp rather than echoed from the parser.
 */

#include "sidetables.hpp"

#include "debugging/perf/jitdump.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <vector>

namespace mono {
namespace test {
namespace {

/*
 * A `.mono_unwind` block:
 *
 *     +0   u32  magic 'MUWD'
 *     +4   u16  version
 *     +8   u32  how many records follow the header
 *     +12  u64  the address the function was linked at
 *     +20       the records, 17 bytes each
 *
 * and one record:
 *
 *     +0   u32  code offset the rule takes effect at
 *     +4   u8   op
 *     +5   i32  register
 *     +9   i64  value
 *
 * Bytes 6 and 7 of the header are not read. The section holds one block per
 * function, one after another.
 */
void
put_u8 (std::vector<uint8_t> &b, uint8_t v)
{
	b.push_back (v);
}

template <typename T>
void
put_le (std::vector<uint8_t> &b, T v)
{
	uint8_t bytes[sizeof (T)];

	memcpy (bytes, &v, sizeof (T));
	b.insert (b.end (), bytes, bytes + sizeof (T));
}

void
put_record (std::vector<uint8_t> &b, const UnwindRecord &r)
{
	size_t from = b.size ();

	put_le<uint32_t> (b, r.offset);
	put_u8 (b, r.op);
	put_le<int32_t> (b, r.reg);
	put_le<int64_t> (b, r.value);

	ASSERT_EQ (b.size () - from, unwind_record_size);
}

/// One block, with the header saying exactly what follows it.
void
put_block (std::vector<uint8_t> &b, const uint8_t *function,
           const std::vector<UnwindRecord> &records)
{
	size_t from = b.size ();

	put_le<uint32_t> (b, unwind_section_magic);
	put_le<uint16_t> (b, unwind_section_version);
	put_le<uint16_t> (b, 0);
	put_le<uint32_t> (b, (uint32_t) records.size ());
	put_le<uint64_t> (b, (uint64_t) (uintptr_t) function);

	ASSERT_EQ (b.size () - from, unwind_header_size);

	for (const UnwindRecord &r : records)
		put_record (b, r);
}

/// A code address to key a block on. Nothing reads through it.
const uint8_t *
code_at (uintptr_t v)
{
	return (const uint8_t *) v;
}

void
expect_same (const std::vector<UnwindRecord> &got,
             const std::vector<UnwindRecord> &want)
{
	ASSERT_EQ (got.size (), want.size ());

	for (size_t i = 0; i < want.size (); ++i) {
		EXPECT_EQ (got[i].offset, want[i].offset) << "record " << i;
		EXPECT_EQ (got[i].op, want[i].op) << "record " << i;
		EXPECT_EQ (got[i].reg, want[i].reg) << "record " << i;
		EXPECT_EQ (got[i].value, want[i].value) << "record " << i;
	}
}

/*
 * The fields are signed and the section is little-endian whatever the host is,
 * so a negative register or value is where a wrong read shows up first.
 */
TEST (UnwindRecords, RecordsComeBackAsWritten)
{
	const std::vector<UnwindRecord> want = {
		{ 0x00, MONO_UNWIND_OP_DEF_CFA, 7, 8 },
		{ 0x01, MONO_UNWIND_OP_OFFSET, 6, -16 },
		{ 0x04, MONO_UNWIND_OP_DEF_CFA_REGISTER, 6, 0 },
		{ 0x2000, MONO_UNWIND_OP_OFFSET, 3, -4096 },
		{ 0xfffffff0, MONO_UNWIND_OP_SAME_VALUE, -1, -1 },
	};

	std::vector<uint8_t> section;
	std::vector<UnwindRecord> got;

	put_block (section, code_at (0x4000), want);

	ASSERT_TRUE (parse_unwind_records (section.data (), section.size (),
	                                   code_at (0x4000), got));
	expect_same (got, want);
}

/*
 * A section holds every function of an object, so picking the block by its
 * address is the whole job. Taking the first would describe one function with
 * another's rules, which unwinds to a wrong frame rather than to none.
 */
TEST (UnwindRecords, TheBlockNamingTheCodeIsPicked)
{
	const std::vector<UnwindRecord> first
		= { { 0x00, MONO_UNWIND_OP_DEF_CFA, 7, 8 } };
	const std::vector<UnwindRecord> wanted
		= { { 0x11, MONO_UNWIND_OP_DEF_CFA_OFFSET, 0, 0x20 },
		    { 0x22, MONO_UNWIND_OP_OFFSET, 6, -32 } };
	const std::vector<UnwindRecord> last
		= { { 0x99, MONO_UNWIND_OP_RESTORE, 6, 0 } };

	std::vector<uint8_t> section;
	std::vector<UnwindRecord> got;

	put_block (section, code_at (0x1000), first);
	put_block (section, code_at (0x2000), wanted);
	put_block (section, code_at (0x3000), last);

	ASSERT_TRUE (parse_unwind_records (section.data (), section.size (),
	                                   code_at (0x2000), got));
	expect_same (got, wanted);
}

/*
 * A function with no rules kept the frame it was called with, which is what a
 * stub has. That is a description, not the absence of one, so it has to come
 * back as success with nothing in it.
 */
TEST (UnwindRecords, ABlockWithNoRecordsIsStillADescription)
{
	std::vector<uint8_t> section;
	std::vector<UnwindRecord> got;

	put_block (section, code_at (0x1000), {});

	EXPECT_TRUE (parse_unwind_records (section.data (), section.size (),
	                                   code_at (0x1000), got));
	EXPECT_TRUE (got.empty ());
}

/* -- The refusals --------------------------------------------------------- */

/*
 * Every refusal below has to leave the output alone as well as return false.
 * A caller that reads a half-filled vector gets a program that stops in the
 * middle, and a program missing its later rules unwinds to a wrong answer.
 */

TEST (UnwindRecords, NoBlockNamesTheCode)
{
	std::vector<uint8_t> section;
	std::vector<UnwindRecord> got;

	put_block (section, code_at (0x1000),
	           { { 0x00, MONO_UNWIND_OP_DEF_CFA, 7, 8 } });
	put_block (section, code_at (0x2000),
	           { { 0x00, MONO_UNWIND_OP_DEF_CFA, 7, 8 } });

	EXPECT_FALSE (parse_unwind_records (section.data (), section.size (),
	                                    code_at (0x9999), got));
	EXPECT_TRUE (got.empty ());
}

TEST (UnwindRecords, NoSectionAtAll)
{
	std::vector<UnwindRecord> got;

	EXPECT_FALSE (parse_unwind_records (nullptr, 0, code_at (0x1000), got));
	EXPECT_TRUE (got.empty ());
}

TEST (UnwindRecords, AWrongMagicIsRefused)
{
	std::vector<uint8_t> section;
	std::vector<UnwindRecord> got;

	put_block (section, code_at (0x1000),
	           { { 0x00, MONO_UNWIND_OP_DEF_CFA, 7, 8 } });
	section[0] ^= 0xff;

	EXPECT_FALSE (parse_unwind_records (section.data (), section.size (),
	                                    code_at (0x1000), got));
	EXPECT_TRUE (got.empty ());
}

TEST (UnwindRecords, AnotherVersionIsRefused)
{
	std::vector<uint8_t> section;
	std::vector<UnwindRecord> got;

	put_block (section, code_at (0x1000),
	           { { 0x00, MONO_UNWIND_OP_DEF_CFA, 7, 8 } });
	section[4] = (uint8_t) (unwind_section_version + 1);

	EXPECT_FALSE (parse_unwind_records (section.data (), section.size (),
	                                    code_at (0x1000), got));
	EXPECT_TRUE (got.empty ());
}

/*
 * The count is the one field that says how far to read, so a count larger than
 * the section is the read that runs off the end.
 *
 * What the check covers is a count past the end of the whole section. A count
 * that lies but stays inside it moves the walk to the wrong place and is not
 * detectable here: the blocks are chained by their counts, so the section is
 * trusted to that extent.
 */
TEST (UnwindRecords, ACountPastTheEndIsRefused)
{
	std::vector<uint8_t> section;
	std::vector<UnwindRecord> got;

	put_block (section, code_at (0x1000),
	           { { 0x00, MONO_UNWIND_OP_DEF_CFA, 7, 8 } });

	uint32_t count = 4;

	memcpy (section.data () + 8, &count, sizeof (count));

	EXPECT_FALSE (parse_unwind_records (section.data (), section.size (),
	                                    code_at (0x1000), got));
	EXPECT_TRUE (got.empty ());
}

TEST (UnwindRecords, AHeaderCutShortIsRefused)
{
	std::vector<uint8_t> section;
	std::vector<UnwindRecord> got;

	put_block (section, code_at (0x1000), {});
	section.resize (unwind_header_size - 1);

	EXPECT_FALSE (parse_unwind_records (section.data (), section.size (),
	                                    code_at (0x1000), got));
	EXPECT_TRUE (got.empty ());
}

/*
 * Trailing bytes that are too few to be a header end the walk. The block before
 * them still has to be found, since a section that grows a partial tail must not
 * lose the functions in front of it.
 */
TEST (UnwindRecords, ABlockBeforeAShortTailIsStillFound)
{
	std::vector<uint8_t> section;
	std::vector<UnwindRecord> got;
	const std::vector<UnwindRecord> want
		= { { 0x01, MONO_UNWIND_OP_DEF_CFA_OFFSET, 0, 0x10 } };

	put_block (section, code_at (0x1000), want);
	section.resize (section.size () + unwind_header_size - 1, 0);

	ASSERT_TRUE (parse_unwind_records (section.data (), section.size (),
	                                   code_at (0x1000), got));
	expect_same (got, want);
}

/* -- What a closed dump costs --------------------------------------------- */

/*
 * The slack is what code allocations are spaced by, so it has to be nothing at
 * all when no dump is open. Nothing in this binary passes `--jitdump`.
 */
TEST (UnwindRecords, NoDumpMeansNoSlack)
{
	ASSERT_FALSE (perf::enabled ());
	EXPECT_EQ (perf::code_slack (), 0u);
}

} // namespace
} // namespace test
} // namespace mono

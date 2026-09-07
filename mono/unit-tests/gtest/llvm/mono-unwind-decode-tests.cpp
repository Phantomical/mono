/*
 * Tests for decode_mono_unwind_ops () in mono/llvm/debugging/perf/eh-frame.cpp,
 * which turns a classic tier-0 method's own unwind-op encoding into the
 * UnwindRecord shape build_eh_frame () already knows how to write as DWARF.
 *
 * Most of that encoding shares DWARF's own opcode space and round-trips
 * unremarked. DW_CFA_mono_advance_loc is the one opcode that is not DWARF.
 * These cases pin what it turns into, and that a caller with nothing to turn
 * it into gets a decline rather than a partial program.
 */

#include "debugging/perf/eh-frame.hpp"

#include "sidetables.hpp"

#include <gtest/gtest.h>

#include <llvm/DebugInfo/DWARF/DWARFDataExtractor.h>
#include <llvm/DebugInfo/DWARF/DWARFDebugFrame.h>
#include <llvm/TargetParser/Triple.h>

#include <cstdint>
#include <vector>

using namespace llvm;

namespace mono {
namespace test {
namespace {

constexpr uint8_t dwarf_rsp = 7;
constexpr uint8_t dwarf_rbp = 6;

constexpr uint8_t DW_CFA_advance_loc = 0x40;
constexpr uint8_t DW_CFA_offset = 0x80;
constexpr uint8_t DW_CFA_restore = 0xc0;
constexpr uint8_t DW_CFA_remember_state = 0x0a;
constexpr uint8_t DW_CFA_restore_state = 0x0b;
constexpr uint8_t DW_CFA_def_cfa = 0x0c;
constexpr uint8_t DW_CFA_def_cfa_offset = 0x0e;
constexpr uint8_t DW_CFA_mono_advance_loc = 0x1c;
constexpr uint8_t DW_CFA_register = 0x09;

/// Builds mono_unwind_ops_encode ()'s own byte stream by hand, in values that
/// each fit a one-byte ULEB, so a case reads as a literal byte sequence.
struct MonoOpsBuilder {
	std::vector<uint8_t> bytes;

	MonoOpsBuilder &advance (uint8_t delta)
	{
		bytes.push_back ((uint8_t) (DW_CFA_advance_loc | delta));
		return *this;
	}
	MonoOpsBuilder &def_cfa_offset (uint8_t value)
	{
		bytes.push_back (DW_CFA_def_cfa_offset);
		bytes.push_back (value);
		return *this;
	}
	MonoOpsBuilder &def_cfa (uint8_t reg, uint8_t value)
	{
		bytes.push_back (DW_CFA_def_cfa);
		bytes.push_back (reg);
		bytes.push_back (value);
		return *this;
	}
	MonoOpsBuilder &remember_state ()
	{
		bytes.push_back (DW_CFA_remember_state);
		return *this;
	}
	MonoOpsBuilder &restore_state ()
	{
		bytes.push_back (DW_CFA_restore_state);
		return *this;
	}
	MonoOpsBuilder &mono_advance_loc ()
	{
		bytes.push_back (DW_CFA_mono_advance_loc);
		return *this;
	}
	MonoOpsBuilder &byte (uint8_t b)
	{
		bytes.push_back (b);
		return *this;
	}
};

/// The rows a reader recovers from the first (only) FDE a description holds,
/// the same way perf-eh-frame-tests.cpp reads one back.
Expected<dwarf::UnwindTable>
unwind_table (const perf::EhFrame &frame, size_t image_size)
{
	size_t align8 = (image_size + 7) & ~(size_t) 7;
	size_t eh_size = frame.bytes.size () - frame.header_size;
	StringRef data ((const char *) frame.bytes.data (), eh_size);
	DWARFDebugFrame parsed (Triple::x86_64, true, align8);
	DWARFDataExtractor extractor (data, true, 8);

	if (Error err = parsed.parse (extractor))
		return std::move (err);

	for (const dwarf::FrameEntry &entry : parsed)
		if (const auto *fde = dyn_cast<dwarf::FDE> (&entry))
			return dwarf::createUnwindTable (fde);

	return createStringError (std::errc::invalid_argument,
	                          "the description holds no FDE");
}

const dwarf::UnwindRow *
row_at (const dwarf::UnwindTable &table, uint64_t offset)
{
	const dwarf::UnwindRow *found = nullptr;

	for (const dwarf::UnwindRow &row : table)
		if (row.getAddress () <= offset)
			found = &row;

	return found;
}

void
expect_cfa (const dwarf::UnwindTable &table, uint64_t offset, uint8_t reg,
            int32_t off)
{
	const dwarf::UnwindRow *row = row_at (table, offset);

	ASSERT_NE (row, nullptr) << "no row covers offset " << offset;

	const dwarf::UnwindLocation &cfa = row->getCFAValue ();

	EXPECT_EQ (cfa.getLocation (), dwarf::UnwindLocation::RegPlusOffset)
		<< "at offset " << offset;
	EXPECT_EQ (cfa.getRegister (), reg) << "at offset " << offset;
	EXPECT_EQ (cfa.getOffset (), off) << "at offset " << offset;
}

/// decode_mono_unwind_ops () plus build_eh_frame (), the pair a classic
/// tier-0 method's dump actually calls.
perf::EhFrame
describe (const MonoOpsBuilder &ops, size_t code_size, size_t epilog_offset)
{
	perf::FrameFunction fn{0, code_size, {}};

	if (!perf::decode_mono_unwind_ops (ops.bytes.data (), ops.bytes.size (),
	                                   epilog_offset, fn.records))
		return {};

	return perf::build_eh_frame ({ fn }, code_size);
}

TEST (MonoUnwindDecode, PlainRecordsRoundTrip)
{
	MonoOpsBuilder ops;

	ops.advance (4).def_cfa_offset (16);

	perf::EhFrame frame = describe (ops, 0x20, perf::no_epilog_offset);

	ASSERT_FALSE (frame.bytes.empty ());

	Expected<dwarf::UnwindTable> table = unwind_table (frame, 0x20);

	ASSERT_TRUE ((bool) table) << toString (table.takeError ());
	expect_cfa (*table, 0, dwarf_rsp, 8);
	expect_cfa (*table, 4, dwarf_rsp, 16);
}

/*
 * The shape a classic tier-0 amd64 epilog actually writes
 * (mono_arch_emit_epilog (), mono/mini/tier0/arch/amd64/codegen.c). A mark
 * opens the epilog, and a remember/restore pair brackets the CFA it sets
 * back to rsp+8 there.
 */
TEST (MonoUnwindDecode, MonoAdvanceLocJumpsToTheEpilogOffset)
{
	MonoOpsBuilder ops;

	ops.advance (4).def_cfa_offset (64)
		.mono_advance_loc ()
		.remember_state ()
		.def_cfa (dwarf_rsp, 8)
		.advance (1)
		.restore_state ();

	perf::EhFrame frame = describe (ops, 0x20, /*epilog_offset=*/0x10);

	ASSERT_FALSE (frame.bytes.empty ());

	Expected<dwarf::UnwindTable> table = unwind_table (frame, 0x20);

	ASSERT_TRUE ((bool) table) << toString (table.takeError ());
	/* Before the epilog: the prologue's rule. */
	expect_cfa (*table, 4, dwarf_rsp, 64);
	/* At the mark, jumped straight to 0x10 rather than reached by an
	 * ordinary advance: the epilog's own rule. */
	expect_cfa (*table, 0x10, dwarf_rsp, 8);
	/* Past DW_CFA_restore_state: back to what remember_state saved. That is
	 * the prologue's rule again, which mono_arch_emit_epilog ()'s own
	 * comment says the out-of-line code past the epilog's `ret` still
	 * needs. */
	expect_cfa (*table, 0x11, dwarf_rsp, 64);
}

TEST (MonoUnwindDecode, MonoAdvanceLocWithoutAnEpilogOffsetIsDeclined)
{
	MonoOpsBuilder ops;

	ops.advance (4).def_cfa_offset (64).mono_advance_loc ();

	perf::EhFrame frame = describe (ops, 0x20, perf::no_epilog_offset);

	EXPECT_TRUE (frame.bytes.empty ());
}

TEST (MonoUnwindDecode, AnUnknownOpIsDeclined)
{
	MonoOpsBuilder ops;

	/* DW_CFA_register: mono's own unwinder (mono_unwind_frame ()) does not
	 * decode this op either. */
	ops.advance (4).byte (DW_CFA_register).byte (0).byte (0);

	perf::EhFrame frame = describe (ops, 0x20, perf::no_epilog_offset);

	EXPECT_TRUE (frame.bytes.empty ());
}

TEST (MonoUnwindDecode, ThePackedRestoreFormIsDeclined)
{
	MonoOpsBuilder ops;

	/* mono's own encoder never emits DW_CFA_restore (0xc0 | reg). */
	ops.advance (4).byte ((uint8_t) (DW_CFA_restore | dwarf_rbp));

	perf::EhFrame frame = describe (ops, 0x20, perf::no_epilog_offset);

	EXPECT_TRUE (frame.bytes.empty ());
}

TEST (MonoUnwindDecode, ARegisterSavedAtCfaMinusEightRoundTrips)
{
	MonoOpsBuilder ops;

	/* DW_CFA_offset (packed): rbp saved one data-alignment unit below the
	 * CFA, the same as mono_emit_unwind_op_offset () writes for a push. */
	ops.advance (1).byte ((uint8_t) (DW_CFA_offset | dwarf_rbp)).byte (1);

	perf::EhFrame frame = describe (ops, 0x20, perf::no_epilog_offset);

	ASSERT_FALSE (frame.bytes.empty ());

	Expected<dwarf::UnwindTable> table = unwind_table (frame, 0x20);

	ASSERT_TRUE ((bool) table) << toString (table.takeError ());

	const dwarf::UnwindRow *row = row_at (*table, 1);

	ASSERT_NE (row, nullptr);

	std::optional<dwarf::UnwindLocation> loc
		= row->getRegisterLocations ().getRegisterLocation (dwarf_rbp);

	ASSERT_TRUE (loc.has_value ());
	EXPECT_EQ (loc->getLocation (), dwarf::UnwindLocation::CFAPlusOffset);
	EXPECT_EQ (loc->getOffset (), -8);
}

} // namespace
} // namespace test
} // namespace mono

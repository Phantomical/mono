/*
 * Tests for the DWARF frame description the perf jit dump carries.
 *
 * A description that is wrong does not stop a stack walk. The walk reads the
 * caller from wherever the rules point and goes on, so the profile gets deeper
 * and stays wrong. That is why these read the bytes back with LLVM's own CFI
 * parser and check the rules, rather than checking that a walk got longer.
 *
 * The resolver's frame is the case worth pinning. Its rules are written by hand
 * against the instruction offsets of hand-written machine code, and the two
 * only stay true together.
 */

#include "arch/arch.hpp"
#include "debugging/perf/eh-frame.hpp"
#include "sidetables.hpp"

#include <gtest/gtest.h>

#include <llvm/DebugInfo/DWARF/DWARFDataExtractor.h>
#include <llvm/DebugInfo/DWARF/DWARFDebugFrame.h>
#include <llvm/TargetParser/Triple.h>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

using namespace llvm;

namespace mono {
namespace test {
namespace {

constexpr uint8_t dwarf_rsp = 7;
constexpr uint8_t dwarf_rbp = 6;
constexpr uint8_t dwarf_rip = 16;

size_t
align8 (size_t v)
{
	return (v + 7) & ~(size_t) 7;
}

/// The rows a reader recovers from one function's description.
///
/// The `.eh_frame` a jit dump carries holds displacements rather than
/// addresses, and they only come out right once the pieces sit where `perf
/// inject --jit` puts them. Placing the section at align8 (image size) is that
/// layout, and it puts the code at zero.
Expected<dwarf::UnwindTable>
unwind_table (const perf::EhFrame &frame, size_t image_size)
{
	size_t eh_size = frame.bytes.size () - frame.header_size;
	StringRef data ((const char *) frame.bytes.data (), eh_size);
	DWARFDebugFrame parsed (Triple::x86_64, true, align8 (image_size));
	DWARFDataExtractor extractor (data, true, 8);

	if (Error err = parsed.parse (extractor))
		return std::move (err);

	/* The rows are a value of their own, so they outlive the parse above. */
	for (const dwarf::FrameEntry &entry : parsed) {
		if (const auto *fde = dyn_cast<dwarf::FDE> (&entry))
			return dwarf::createUnwindTable (fde);
	}

	return createStringError (std::errc::invalid_argument,
	                          "the description holds no FDE");
}

/// The row in effect at a code offset, which is the last one at or below it.
const dwarf::UnwindRow *
row_at (const dwarf::UnwindTable &table, uint64_t offset)
{
	const dwarf::UnwindRow *found = nullptr;

	for (const dwarf::UnwindRow &row : table) {
		if (row.getAddress () <= offset)
			found = &row;
	}

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

/// Where a register is saved relative to the CFA, or no value when the row
/// leaves it where it was.
std::optional<int32_t>
saved_at (const dwarf::UnwindTable &table, uint64_t offset, uint8_t reg)
{
	const dwarf::UnwindRow *row = row_at (table, offset);

	if (row == nullptr)
		return std::nullopt;

	std::optional<dwarf::UnwindLocation> loc
		= row->getRegisterLocations ().getRegisterLocation (reg);

	if (!loc || loc->getLocation () != dwarf::UnwindLocation::CFAPlusOffset)
		return std::nullopt;

	return (int32_t) loc->getOffset ();
}

perf::EhFrame
resolver_description ()
{
	perf::FrameFunction fn;

	fn.offset = 0;
	fn.size = arch::LazyEntryABI::ResolverCodeSize;
	fn.records = arch::lazy_resolver_frame ();

	return perf::build_eh_frame ({ fn }, fn.size);
}

/*
 * The rules the resolver's frame has to produce. Read them against the code in
 * arch/amd64/lazy-entry.cpp: 0x00 is its first instruction, 0x04 is where its
 * body starts, 0xa4 is the `ret`, 0xa5 is the first instruction of the throw
 * path and 0xb3 is where that path has the caller's frame back.
 */
TEST (PerfEhFrame, ResolverDeclaresTheCallersFrame)
{
	perf::EhFrame frame = resolver_description ();

	ASSERT_FALSE (frame.bytes.empty ());

	Expected<dwarf::UnwindTable> table
		= unwind_table (frame, arch::LazyEntryABI::ResolverCodeSize);

	ASSERT_TRUE ((bool) table) << toString (table.takeError ());

	/* Entry: the trampoline's return address is on top of the caller's, so the
	 * CFA is two slots up rather than one. */
	expect_cfa (*table, 0x00, dwarf_rsp, 0x10);

	/* pushq %rbp */
	expect_cfa (*table, 0x01, dwarf_rsp, 0x18);
	EXPECT_EQ (saved_at (*table, 0x01, dwarf_rbp), -0x18);

	/* The body, which is all of the compile. */
	expect_cfa (*table, 0x04, dwarf_rbp, 0x18);
	expect_cfa (*table, 0x61, dwarf_rbp, 0x18);
	expect_cfa (*table, 0xa3, dwarf_rbp, 0x18);
	EXPECT_EQ (saved_at (*table, 0x61, dwarf_rbp), -0x18);

	/* After popq %rbp the frame is the caller's again. */
	expect_cfa (*table, 0xa4, dwarf_rsp, 0x10);
	EXPECT_EQ (saved_at (*table, 0xa4, dwarf_rbp), std::nullopt);

	/* The throw path is entered by a jump, so it restates the body's rules. */
	expect_cfa (*table, 0xa5, dwarf_rbp, 0x18);
	EXPECT_EQ (saved_at (*table, 0xa5, dwarf_rbp), -0x18);

	expect_cfa (*table, 0xb3, dwarf_rsp, 0x08);
	EXPECT_EQ (saved_at (*table, 0xb3, dwarf_rbp), std::nullopt);
}

/*
 * Whatever the rules say, a reader takes the return address from CFA-8 on this
 * target. The CIE is what says so, and every FDE written here inherits it.
 */
TEST (PerfEhFrame, ReturnAddressComesFromTheCieRule)
{
	perf::EhFrame frame = resolver_description ();
	Expected<dwarf::UnwindTable> table
		= unwind_table (frame, arch::LazyEntryABI::ResolverCodeSize);

	ASSERT_TRUE ((bool) table) << toString (table.takeError ());

	for (uint64_t offset : { (uint64_t) 0x00, (uint64_t) 0x61, (uint64_t) 0xa4 })
		EXPECT_EQ (saved_at (*table, offset, dwarf_rip), -8)
			<< "at offset " << offset;
}

/*
 * An empty program says the function still has the frame it was called with,
 * which is what a stub has. The rules then have to be the CIE's alone.
 */
TEST (PerfEhFrame, NoRulesMeansTheEntryFrame)
{
	perf::FrameFunction fn;

	fn.offset = 0;
	fn.size = 16;

	perf::EhFrame frame = perf::build_eh_frame ({ fn }, fn.size);
	Expected<dwarf::UnwindTable> table = unwind_table (frame, fn.size);

	ASSERT_TRUE ((bool) table) << toString (table.takeError ());

	expect_cfa (*table, 0x00, dwarf_rsp, 8);
	EXPECT_EQ (saved_at (*table, 0x00, dwarf_rip), -8);
}

/*
 * One record covers a whole object, so its description carries an FDE for each
 * function in it. A reader binary searches them, so they have to come out in
 * the order they were put in and each has to name its own code.
 */
TEST (PerfEhFrame, EveryFunctionGetsItsOwnFde)
{
	std::vector<perf::FrameFunction> functions;

	for (size_t i = 0; i < 3; ++i) {
		perf::FrameFunction fn;

		fn.offset = i * 0x40;
		fn.size = 0x30;
		fn.records = { { 0x01, MONO_UNWIND_OP_DEF_CFA_OFFSET, 0, 0x10 } };
		functions.push_back (fn);
	}

	size_t image = 3 * 0x40;
	perf::EhFrame frame = perf::build_eh_frame (functions, image);

	ASSERT_FALSE (frame.bytes.empty ());

	size_t eh_size = frame.bytes.size () - frame.header_size;
	StringRef data ((const char *) frame.bytes.data (), eh_size);
	DWARFDebugFrame parsed (Triple::x86_64, true, align8 (image));
	DWARFDataExtractor extractor (data, true, 8);

	ASSERT_FALSE ((bool) parsed.parse (extractor));

	std::vector<uint64_t> starts;

	for (const dwarf::FrameEntry &entry : parsed) {
		if (const auto *fde = dyn_cast<dwarf::FDE> (&entry)) {
			starts.push_back (fde->getInitialLocation ());
			EXPECT_EQ (fde->getAddressRange (), 0x30u);
		}
	}

	EXPECT_EQ (starts, (std::vector<uint64_t>{ 0x00, 0x40, 0x80 }));

	/* The header indexes them, and its size is what says how many. */
	EXPECT_EQ (frame.header_size, 12u + 8u * 3u);
}

} // namespace
} // namespace test
} // namespace mono

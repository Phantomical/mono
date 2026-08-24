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
unwind_table (const perf::EhFrame &frame, size_t image_size, size_t index = 0)
{
	size_t eh_size = frame.bytes.size () - frame.header_size;
	StringRef data ((const char *) frame.bytes.data (), eh_size);
	DWARFDebugFrame parsed (Triple::x86_64, true, align8 (image_size));
	DWARFDataExtractor extractor (data, true, 8);
	size_t seen = 0;

	if (Error err = parsed.parse (extractor))
		return std::move (err);

	/* The rows are a value of their own, so they outlive the parse above. */
	for (const dwarf::FrameEntry &entry : parsed) {
		if (const auto *fde = dyn_cast<dwarf::FDE> (&entry)) {
			if (seen++ == index)
				return dwarf::createUnwindTable (fde);
		}
	}

	return createStringError (std::errc::invalid_argument,
	                          "the description holds no FDE");
}

/// The code offset each FDE describes, in the order they were written.
std::vector<uint64_t>
fde_starts (const perf::EhFrame &frame, size_t image_size)
{
	size_t eh_size = frame.bytes.size () - frame.header_size;
	StringRef data ((const char *) frame.bytes.data (), eh_size);
	DWARFDebugFrame parsed (Triple::x86_64, true, align8 (image_size));
	DWARFDataExtractor extractor (data, true, 8);
	std::vector<uint64_t> starts;

	if (Error err = parsed.parse (extractor)) {
		consumeError (std::move (err));
		return starts;
	}

	for (const dwarf::FrameEntry &entry : parsed) {
		if (const auto *fde = dyn_cast<dwarf::FDE> (&entry))
			starts.push_back (fde->getInitialLocation ());
	}

	return starts;
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
 * body starts, 0xa0 is the `ret`, 0xa1 is the first instruction of the throw
 * path and 0xaf is where that path has the caller's frame back.
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
	expect_cfa (*table, 0x9f, dwarf_rbp, 0x18);
	EXPECT_EQ (saved_at (*table, 0x61, dwarf_rbp), -0x18);

	/* After popq %rbp the frame is the caller's again. */
	expect_cfa (*table, 0xa0, dwarf_rsp, 0x10);
	EXPECT_EQ (saved_at (*table, 0xa0, dwarf_rbp), std::nullopt);

	/* The throw path is entered by a jump, so it restates the body's rules. */
	expect_cfa (*table, 0xa1, dwarf_rbp, 0x18);
	EXPECT_EQ (saved_at (*table, 0xa1, dwarf_rbp), -0x18);

	expect_cfa (*table, 0xaf, dwarf_rsp, 0x08);
	EXPECT_EQ (saved_at (*table, 0xaf, dwarf_rbp), std::nullopt);
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

/* -- The rules against the code they describe ----------------------------- */

/// Where the first record with this op and register sits, or the code size
/// when the program has no such record.
uint32_t
rule_offset (const std::vector<UnwindRecord> &records, int op, int reg,
             uint32_t after = 0)
{
	for (const UnwindRecord &r : records) {
		if (r.op == op && r.offset >= after && (reg < 0 || r.reg == reg))
			return r.offset;
	}

	return arch::LazyEntryABI::ResolverCodeSize;
}

void
expect_bytes (const std::vector<char> &code, uint32_t at,
              const std::vector<uint8_t> &want, const char *what)
{
	ASSERT_LE (at + want.size (), code.size ()) << what;

	for (size_t i = 0; i < want.size (); ++i)
		EXPECT_EQ ((uint8_t) code[at + i], want[i])
			<< what << ", byte " << i << " at " << (at + i);
}

/*
 * The rules are written by hand against hand-written machine code, and only the
 * two together are correct. Nothing else holds them to each other: a rule at a
 * wrong offset unwinds to a wrong frame, which reads as a deeper profile rather
 * than as a failure.
 *
 * So each rule is looked up in the program and the instruction it claims to
 * follow is read out of the code. Move either side alone and this fails.
 */
TEST (PerfEhFrame, ResolverRulesSitOnInstructionBoundaries)
{
	std::vector<char> code (arch::LazyEntryABI::ResolverCodeSize);

	arch::LazyEntryABI::writeResolverCode (
		code.data (), orc::ExecutorAddr::fromPtr (code.data ()),
		orc::ExecutorAddr (0x1000), orc::ExecutorAddr (0x2000));

	std::vector<UnwindRecord> records = arch::lazy_resolver_frame ();

	ASSERT_FALSE (records.empty ());

	// A rule past the end describes code that is not there.
	uint32_t last = 0;

	for (const UnwindRecord &r : records) {
		EXPECT_LT (r.offset, arch::LazyEntryABI::ResolverCodeSize);
		EXPECT_GE (r.offset, last) << "the rules are out of order";
		last = r.offset;
	}

	// pushq %rbp, which is what moves the CFA to 0x18.
	uint32_t pushed
		= rule_offset (records, MONO_UNWIND_OP_DEF_CFA_OFFSET, -1);
	expect_bytes (code, pushed - 1, { 0x55 }, "pushq %rbp");

	// movq %rsp, %rbp, after which the body stands on %rbp.
	uint32_t framed = rule_offset (records, MONO_UNWIND_OP_DEF_CFA_REGISTER,
	                               dwarf_rbp);
	expect_bytes (code, framed - 3, { 0x48, 0x89, 0xe5 }, "movq %rsp, %rbp");

	/*
	 * popq %rbp puts the frame back, and the ret is the instruction the rule
	 * covers - so the rule has to name the offset of the ret itself, not the
	 * pop before it.
	 */
	uint32_t popped = rule_offset (records, MONO_UNWIND_OP_DEF_CFA, dwarf_rsp,
	                               framed);
	expect_bytes (code, popped - 1, { 0x5d }, "popq %rbp");
	expect_bytes (code, popped, { 0xc3 }, "retq");

	/*
	 * The throw path is entered by a jump from inside the body, so it starts
	 * right after the ret and restates the body's rules rather than inheriting
	 * what the line above leaves.
	 */
	uint32_t thrown = rule_offset (records, MONO_UNWIND_OP_DEF_CFA, dwarf_rbp,
	                               popped);
	EXPECT_EQ (thrown, popped + 1) << "the throw path does not follow the ret";
	expect_bytes (code, thrown, { 0x48, 0x89, 0xc7 }, "movq %rax, %rdi");

	// movq %r11, %rbp, after which %rbp and %rsp are both the caller's.
	uint32_t cut = rule_offset (records, MONO_UNWIND_OP_DEF_CFA, dwarf_rsp,
	                            thrown);
	expect_bytes (code, cut - 3, { 0x4c, 0x89, 0xdd }, "movq %r11, %rbp");
	expect_bytes (code, cut - 7, { 0x48, 0x8d, 0x65, 0x10 },
	              "leaq 0x10(%rbp), %rsp");
}

/* -- The rules a method's own program uses -------------------------------- */

/*
 * A method's program saves and restores state around a shrink-wrapped body,
 * which the resolver's does not. Without a case here those three ops reach a
 * profile untranslated by any test.
 */
TEST (PerfEhFrame, RememberAndRestoreStateBracketTheBody)
{
	perf::FrameFunction fn;

	fn.offset = 0;
	fn.size = 0x20;
	fn.records = {
		{ 0x01, MONO_UNWIND_OP_DEF_CFA_OFFSET, 0, 0x10 },
		{ 0x01, MONO_UNWIND_OP_OFFSET, dwarf_rbp, -0x10 },
		{ 0x04, MONO_UNWIND_OP_REMEMBER_STATE, 0, 0 },
		{ 0x04, MONO_UNWIND_OP_DEF_CFA_REGISTER, dwarf_rbp, 0 },
		{ 0x10, MONO_UNWIND_OP_RESTORE_STATE, 0, 0 },
	};

	perf::EhFrame frame = perf::build_eh_frame ({ fn }, fn.size);
	Expected<dwarf::UnwindTable> table = unwind_table (frame, fn.size);

	ASSERT_TRUE ((bool) table) << toString (table.takeError ());

	// def_cfa_register keeps the offset the remembered state carried.
	expect_cfa (*table, 0x04, dwarf_rbp, 0x10);
	EXPECT_EQ (saved_at (*table, 0x04, dwarf_rbp), -0x10);

	// And restore_state puts the whole row back, the CFA register included.
	expect_cfa (*table, 0x10, dwarf_rsp, 0x10);
	EXPECT_EQ (saved_at (*table, 0x10, dwarf_rbp), -0x10);
}

/*
 * RESTORE takes one register back to the rule the CIE gave it. The CIE here
 * names only the return address, so a restored %rbp keeps no location at all.
 */
TEST (PerfEhFrame, RestoreReturnsOneRegisterToTheEntryRule)
{
	perf::FrameFunction fn;

	fn.offset = 0;
	fn.size = 0x20;
	fn.records = {
		{ 0x01, MONO_UNWIND_OP_DEF_CFA_OFFSET, 0, 0x10 },
		{ 0x01, MONO_UNWIND_OP_OFFSET, dwarf_rbp, -0x10 },
		{ 0x08, MONO_UNWIND_OP_RESTORE, dwarf_rbp, 0 },
	};

	perf::EhFrame frame = perf::build_eh_frame ({ fn }, fn.size);
	Expected<dwarf::UnwindTable> table = unwind_table (frame, fn.size);

	ASSERT_TRUE ((bool) table) << toString (table.takeError ());

	EXPECT_EQ (saved_at (*table, 0x01, dwarf_rbp), -0x10);
	EXPECT_EQ (saved_at (*table, 0x08, dwarf_rbp), std::nullopt);

	// The return address is the CIE's rule throughout either way.
	EXPECT_EQ (saved_at (*table, 0x08, dwarf_rip), -8);
}

/* -- What happens to a rule DWARF cannot say ------------------------------ */

/*
 * A saved register that does not land on a whole data alignment factor has no
 * DWARF spelling in this encoding. The writer drops that function's whole
 * description rather than the one rule, because a program missing a rule in the
 * middle unwinds to a wrong answer, and a function with no FDE only stops.
 */
TEST (PerfEhFrame, AnUnsayableRuleDropsThatFunctionOnly)
{
	perf::FrameFunction good, bad;

	good.offset = 0;
	good.size = 0x30;
	good.records = { { 0x01, MONO_UNWIND_OP_DEF_CFA_OFFSET, 0, 0x10 } };

	bad.offset = 0x40;
	bad.size = 0x30;
	bad.records = {
		{ 0x01, MONO_UNWIND_OP_DEF_CFA_OFFSET, 0, 0x10 },
		{ 0x01, MONO_UNWIND_OP_OFFSET, dwarf_rbp, -0x0c },
	};

	perf::EhFrame frame = perf::build_eh_frame ({ good, bad }, 0x70);

	ASSERT_FALSE (frame.bytes.empty ());
	EXPECT_EQ (fde_starts (frame, 0x70), (std::vector<uint64_t>{ 0x00 }));
	EXPECT_EQ (frame.header_size, 12u + 8u * 1u);
}

/*
 * With nothing left to describe there is no description, and the caller has to
 * see that rather than an empty section that reads as a valid one.
 */
TEST (PerfEhFrame, NoSayableFunctionMeansNoDescription)
{
	perf::FrameFunction bad;

	bad.offset = 0;
	bad.size = 0x30;
	bad.records = { { 0x01, MONO_UNWIND_OP_OFFSET, dwarf_rbp, -0x0c } };

	EXPECT_TRUE (perf::build_eh_frame ({ bad }, 0x30).bytes.empty ());
}

/*
 * A function of no size describes nothing, and an FDE covering an empty range
 * would be a lookup that can match no address.
 */
TEST (PerfEhFrame, AnEmptyFunctionGetsNoFde)
{
	perf::FrameFunction empty, real;

	empty.offset = 0;
	empty.size = 0;
	real.offset = 0x40;
	real.size = 0x30;
	real.records = { { 0x01, MONO_UNWIND_OP_DEF_CFA_OFFSET, 0, 0x10 } };

	perf::EhFrame frame = perf::build_eh_frame ({ empty, real }, 0x70);

	EXPECT_EQ (fde_starts (frame, 0x70), (std::vector<uint64_t>{ 0x40 }));
}

} // namespace
} // namespace test
} // namespace mono

/*
 * test-llvm-jitdump.cpp: unit tests for the perf unwind tables
 * mono/mini/llvm/jitdump.cpp builds.
 *
 * build_perf_unwind_data () rewrites the .eh_frame LLVM emitted into the
 * .eh_frame/.eh_frame_hdr pair perf wants, and every pointer it writes is
 * measured against the ELF `perf inject --jit` synthesizes rather than against
 * anything in this process. That layout is not observable from here, and perf
 * is not installed on a build machine, so the checks below RESOLVE each encoded
 * pointer the way a consumer would - in a simulated DSO whose section addresses
 * are computed from perf's own rules - and assert it lands where it should.
 * That is the property that decides whether a profile can unwind, and it is
 * checkable offline.
 *
 * The input .eh_frame is synthesized rather than taken from a real compile, so
 * the cases can vary one thing at a time: two functions in the section (which
 * happens for real - the GC safepoint poll rides along with a method), a
 * pointer encoding we decline, a truncated section.
 */

#include "config.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#ifdef ENABLE_LLVM

#include "mini/llvm/jitdump.hpp"

using mono::build_perf_unwind_data;
using mono::PERF_EH_FRAME_HDR_SIZE;
using mono::PERF_ELF_TEXT_OFFSET;
using mono::PERF_ELF_EH_FRAME_ALIGN;

/* ------------------------------------------------------------ reporting */

static int failures;
static int cases_run;

static void
check (bool cond, const char *what)
{
	cases_run++;
	if (!cond) {
		failures++;
		printf ("FAIL: %s\n", what);
	}
}

static void
check_eq (std::int64_t got, std::int64_t want, const char *what)
{
	cases_run++;
	if (got != want) {
		failures++;
		printf ("FAIL: %s: got %lld, want %lld\n", what, (long long) got, (long long) want);
	}
}

/* --------------------------------------------------- synthetic .eh_frame */

/*
 * A CIE in the shape LLVM emits for amd64: augmentation "zR" whose only datum
 * is the FDE pointer encoding. The CFI program is padding - nothing here reads
 * it, and the rewrite copies it through untouched.
 */
static void
append_cie (std::vector<std::uint8_t> &s, std::uint8_t fde_encoding)
{
	std::vector<std::uint8_t> body = {
		0, 0, 0, 0,             /* CIE id */
		1,                      /* version */
		'z', 'R', 0,            /* augmentation */
		1,                      /* code alignment factor */
		0x78,                   /* data alignment factor, sleb -8 */
		16,                     /* return address register */
		1, fde_encoding,        /* augmentation data: length, 'R' datum */
		0x0c, 0x07, 0x08,       /* DW_CFA_def_cfa r7 8 */
	};

	while (body.size () % 8 != 4)   /* the length word makes the record 8-aligned */
		body.push_back (0);

	std::uint32_t length = (std::uint32_t) body.size ();
	for (int i = 0; i < 4; i++)
		s.push_back ((std::uint8_t) (length >> (8 * i)));
	s.insert (s.end (), body.begin (), body.end ());
}

/*
 * An FDE at the end of S pointing back at the CIE that starts at CIE_OFFSET,
 * and its offset. initial_location is left for fix_initial_location () below:
 * it is pcrel, so it cannot be written until the section has stopped moving.
 */
static std::uint32_t
append_fde (std::vector<std::uint8_t> &s, std::uint32_t cie_offset, std::uint32_t code_size)
{
	std::uint32_t fde_offset = (std::uint32_t) s.size ();
	std::vector<std::uint8_t> body (4 + 4 + 4 + 1 + 3, 0);
	std::uint32_t cie_pointer = fde_offset + 4 - cie_offset;

	std::memcpy (body.data (), &cie_pointer, 4);
	std::memcpy (body.data () + 8, &code_size, 4);
	/* augmentation data length 0, then a DW_CFA_def_cfa_offset run. */
	body [12] = 0;
	body [13] = 0x0e;
	body [14] = 0x10;

	std::uint32_t length = (std::uint32_t) body.size ();
	for (int i = 0; i < 4; i++)
		s.push_back ((std::uint8_t) (length >> (8 * i)));
	s.insert (s.end (), body.begin (), body.end ());
	return fde_offset;
}

/*
 * Point the FDE at CODE, the way a loaded section does: initial_location is
 * pcrel, so its value depends on where the section itself lives.
 */
static void
fix_initial_location (std::vector<std::uint8_t> &s, std::uint32_t fde_offset, const std::uint8_t *code)
{
	const std::uint8_t *field = s.data () + fde_offset + 8;
	std::int32_t initial_location = (std::int32_t) (code - field);

	std::memcpy (s.data () + fde_offset + 8, &initial_location, 4);
}

/*
 * An address to stand in for the compiled function. It is never dereferenced,
 * only matched - but it has to be within an sdata4 of the section, because
 * that is the range a pcrel FDE pointer has and the one the small code model
 * gives a real compile.
 */
static const std::uint8_t *
fake_code (const std::vector<std::uint8_t> &section)
{
	return section.data () + 0x10000;
}

static std::int32_t
read32 (const std::vector<std::uint8_t> &b, std::uint32_t offset)
{
	std::int32_t v;
	std::memcpy (&v, b.data () + offset, 4);
	return v;
}

/* ------------------------------------------------------------- the cases */

/*
 * Resolve every pointer in the produced tables inside a simulated injected
 * DSO, and check each one reaches the section it names.
 */
static void
check_dso_pointers (const std::vector<std::uint8_t> &out, std::uint32_t code_size,
                    std::uint32_t expect_cie_size)
{
	std::uint32_t frame_size = (std::uint32_t) out.size () - PERF_EH_FRAME_HDR_SIZE;
	std::uint32_t fde_off = expect_cie_size;

	/* perf's layout: .text, then .eh_frame aligned to 8, then .eh_frame_hdr. */
	std::uint64_t text = PERF_ELF_TEXT_OFFSET;
	std::uint64_t frame = text + ((code_size + PERF_ELF_EH_FRAME_ALIGN - 1) & ~(std::uint64_t) (PERF_ELF_EH_FRAME_ALIGN - 1));
	std::uint64_t hdr = frame + frame_size;

	/* The FDE's CIE pointer counts back from its own position. */
	check_eq ((std::int64_t) (frame + fde_off + 4) - read32 (out, fde_off + 4),
	          (std::int64_t) frame, "FDE's CIE pointer reaches the CIE");
	/* initial_location is pcrel from the field itself. */
	check_eq ((std::int64_t) (frame + fde_off + 8) + read32 (out, fde_off + 8),
	          (std::int64_t) text, "FDE's initial_location reaches .text");
	/* The function extent is copied through unchanged. */
	check_eq (read32 (out, fde_off + 12), code_size, "FDE's address_range survives the rewrite");

	check_eq (out [frame_size + 0], 1, "eh_frame_hdr version");
	check_eq (out [frame_size + 1], 0x1b, "eh_frame_ptr is pcrel|sdata4");
	check_eq (out [frame_size + 2], 0x03, "fde_count is udata4");
	check_eq (out [frame_size + 3], 0x3b, "table entries are datarel|sdata4");

	/* eh_frame_ptr is pcrel from the field. */
	check_eq ((std::int64_t) (hdr + 4) + read32 (out, frame_size + 4),
	          (std::int64_t) frame, "eh_frame_ptr reaches .eh_frame");
	check_eq (read32 (out, frame_size + 8), 1, "one FDE in the search table");
	/* Table entries are datarel, i.e. relative to the header's own start. */
	check_eq ((std::int64_t) hdr + read32 (out, frame_size + 12),
	          (std::int64_t) text, "search table entry reaches .text");
	check_eq ((std::int64_t) hdr + read32 (out, frame_size + 16),
	          (std::int64_t) (frame + fde_off), "search table entry reaches the FDE");

	/* The section ends in the zero word a .eh_frame is terminated with. */
	check_eq (read32 (out, frame_size - 4), 0, ".eh_frame is terminated");
}

static void
case_single_function (void)
{
	const std::uint32_t code_size = 0x2b;   /* deliberately not 8-aligned */
	std::vector<std::uint8_t> section;
	std::vector<std::uint8_t> out;
	std::uint32_t cie_size, fde_off;
	const std::uint8_t *code;

	append_cie (section, 0x1b);
	cie_size = (std::uint32_t) section.size ();
	fde_off = append_fde (section, 0, code_size);
	code = fake_code (section);
	fix_initial_location (section, fde_off, code);

	check (build_perf_unwind_data (code, code_size, section.data (),
	                               (std::uint32_t) section.size (), out),
	       "a well-formed section produces unwind data");
	if (out.empty ())
		return;

	check_eq ((std::int64_t) out.size (),
	          (std::int64_t) (section.size () + 4 + PERF_EH_FRAME_HDR_SIZE),
	          "output is the CIE, the FDE, a terminator and a header");
	check (std::memcmp (out.data (), section.data (), cie_size) == 0,
	       "the CIE is copied through byte for byte");

	check_dso_pointers (out, code_size, cie_size);
}

/*
 * A method's object can hold a second function (the GC safepoint poll), so the
 * FDE has to be chosen by the address it describes, not by being first.
 */
static void
case_picks_the_right_fde (void)
{
	const std::uint32_t code_size = 0x40;
	std::vector<std::uint8_t> section;
	std::vector<std::uint8_t> out;
	std::uint32_t cie_size, first_fde_off, second_fde_off;
	const std::uint8_t *code, *other;

	append_cie (section, 0x1b);
	cie_size = (std::uint32_t) section.size ();
	first_fde_off = append_fde (section, 0, 0x10);
	second_fde_off = append_fde (section, 0, code_size);
	code = fake_code (section);
	other = code + 0x1000;
	fix_initial_location (section, first_fde_off, other);
	fix_initial_location (section, second_fde_off, code);

	check (build_perf_unwind_data (code, code_size, section.data (),
	                               (std::uint32_t) section.size (), out),
	       "a section with two functions produces unwind data");
	if (out.empty ())
		return;

	check_eq ((std::int64_t) out.size (),
	          (std::int64_t) (cie_size + (section.size () - second_fde_off) + 4 + PERF_EH_FRAME_HDR_SIZE),
	          "only the matching FDE is carried over");
	check_dso_pointers (out, code_size, cie_size);
}

/* A CIE declaring an encoding we cannot relocate must be declined, not guessed
 * at: the pointer would resolve to an arbitrary address in the injected DSO. */
static void
case_declines_unknown_encoding (void)
{
	std::vector<std::uint8_t> section;
	std::vector<std::uint8_t> out;
	std::uint32_t fde_off;
	const std::uint8_t *code;

	/* DW_EH_PE_absptr: legal, but an 8-byte absolute address. */
	append_cie (section, 0x00);
	fde_off = append_fde (section, 0, 0x20);
	code = fake_code (section);
	fix_initial_location (section, fde_off, code);

	check (!build_perf_unwind_data (code, 0x20, section.data (),
	                                (std::uint32_t) section.size (), out),
	       "an absptr FDE encoding is declined");
}

static void
case_declines_missing_fde (void)
{
	std::vector<std::uint8_t> section;
	std::vector<std::uint8_t> out;
	std::uint32_t fde_off;
	const std::uint8_t *code, *other;

	append_cie (section, 0x1b);
	fde_off = append_fde (section, 0, 0x20);
	code = fake_code (section);
	other = code + 0x1000;
	fix_initial_location (section, fde_off, other);

	check (!build_perf_unwind_data (code, 0x20, section.data (),
	                                (std::uint32_t) section.size (), out),
	       "a section with no FDE for the method is declined");
	check (!build_perf_unwind_data (code, 0x20, nullptr, 0, out),
	       "an absent section is declined");
}

/*
 * Every prefix of a valid section, each in a buffer of its own sized exactly to
 * the prefix, so a decoder that reads past the size it was handed runs off a
 * heap allocation rather than into the rest of a longer buffer. None may be
 * accepted: the FDE is only complete at the full length.
 */
static void
case_truncation (void)
{
	const std::uint32_t code_size = 0x20;
	std::vector<std::uint8_t> section;
	std::uint32_t fde_off;

	append_cie (section, 0x1b);
	fde_off = append_fde (section, 0, code_size);

	for (std::size_t n = 0; n < section.size (); n++) {
		std::vector<std::uint8_t> prefix (section.begin (), section.begin () + n);
		std::vector<std::uint8_t> out;
		const std::uint8_t *code = fake_code (prefix);

		/* initial_location is pcrel, so it has to be rewritten for
		 * wherever this copy landed - otherwise a prefix would decline
		 * for the wrong reason. */
		if (n >= fde_off + 12)
			fix_initial_location (prefix, fde_off, code);

		if (build_perf_unwind_data (code, code_size, prefix.data (), (std::uint32_t) n, out)) {
			failures++;
			printf ("FAIL: %u-byte prefix of the section was accepted\n", (unsigned) n);
		}
		cases_run++;
	}
}

#ifdef __cplusplus
extern "C"
#endif
int test_llvm_jitdump_main (void);

int
test_llvm_jitdump_main (void)
{
	failures = 0;
	cases_run = 0;

	setvbuf (stdout, nullptr, _IOLBF, 0);

	case_single_function ();
	case_picks_the_right_fde ();
	case_declines_unknown_encoding ();
	case_declines_missing_fde ();
	case_truncation ();

	printf ("%d cases run, %d failed\n", cases_run, failures);
	return failures ? 1 : 0;
}

#else /* !ENABLE_LLVM */

#ifdef __cplusplus
extern "C"
#endif
int test_llvm_jitdump_main (void);

int
test_llvm_jitdump_main (void)
{
	return 0;
}

#endif /* ENABLE_LLVM */

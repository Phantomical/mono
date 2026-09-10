/**
 * \file
 * \brief Composing the jitdump records that describe one range of JIT'd code.
 *
 * perf applies a line record and an unwinding record to the next code load it
 * reads. So they have to reach the file back to back, with no other writer in
 * between, and the dump writer takes them together for exactly that reason.
 */

#include "debugging/perf/jitdump.hpp"

#include "debugging/perf/eh-frame.hpp"
#include "debugging/perf/perf.h"

#include "mini-runtime.h"

#include "mono/utils/mono-time.h"

#include <algorithm>
#include <cstring>
#include <vector>

namespace mono::perf {

namespace {

constexpr uint32_t jit_code_debug_info = 2;
constexpr uint32_t jit_code_unwinding_info = 4;

/// id, total size and timestamp, then the three sizes.
constexpr size_t unwinding_record_header_size = 16 + 24;

/// id, total size and timestamp, then the code address and the entry count.
constexpr size_t debug_record_header_size = 16 + 16;

void
put (std::vector<uint8_t> &out, uint64_t value, size_t width)
{
	for (size_t i = 0; i < width; ++i)
		out.push_back ((uint8_t) (value >> (8 * i)));
}

/// The monotonic clock, which mono names differently per host.
mono_clock_id_t
monotonic_clock ()
{
	static const mono_clock_id_t id = [] {
		mono_clock_id_t clk;

		mono_clock_init (&clk);
		return clk;
	}();

	return id;
}

/// A JIT_CODE_UNWINDING_INFO record around a frame description.
///
/// mapped_size covers the whole description, which is what puts it inside the
/// mapping perf synthesizes for the code. Zero says the opposite: that there is
/// no lookup table and the reader must fall back to frame pointers. The
/// description then lands outside the mapping and is never read.
std::vector<uint8_t>
unwinding_record (const EhFrame &frame)
{
	std::vector<uint8_t> record;
	size_t total = unwinding_record_header_size + frame.bytes.size ();

	/* A record sits at whatever offset the one before it ended at. A length that
	 * is a multiple of 8 therefore keeps the fields inside every record aligned. */
	size_t padded = (total + 7) & ~(size_t) 7;

	record.reserve (padded);
	put (record, jit_code_unwinding_info, 4);
	put (record, padded, 4);
	/* The clock the dump's other records are stamped from
	 * (mono_emit_jit_dump_code). perf cannot line a dump of two clocks up
	 * against its samples. */
	put (record, mono_clock_get_time_ns (monotonic_clock ()), 8);
	put (record, frame.bytes.size (), 8);
	put (record, frame.header_size, 8);
	put (record, frame.bytes.size (), 8);
	record.insert (record.end (), frame.bytes.begin (), frame.bytes.end ());
	record.resize (padded, 0);
	return record;
}

/// A JIT_CODE_DEBUG_INFO record around one function's line rows.
///
/// An entry is 16 bytes and a NUL-terminated name, and perf finds the next one
/// at the end of that name.
std::vector<uint8_t>
debug_record (const uint8_t *code, const std::vector<DebugLine> &lines)
{
	std::vector<uint8_t> record;
	static const char repeat[] = { '\xff', '\0' };
	const std::string *last = nullptr;

	record.reserve (debug_record_header_size + lines.size () * 24);
	put (record, jit_code_debug_info, 4);
	// The total size, once the entries below say what it is.
	put (record, 0, 4);
	put (record, mono_clock_get_time_ns (monotonic_clock ()), 8);
	put (record, (uint64_t) (uintptr_t) code, 8);
	put (record, lines.size (), 8);

	for (const DebugLine &line : lines) {
		// perf reads the repeat marker against the name it kept from the
		// entry before, so the first entry carries a real name.
		bool repeats = last != nullptr && *last == line.file;
		const char *name = repeats ? repeat : line.file.c_str ();

		put (record, (uint64_t) (uintptr_t) (code + line.offset), 8);
		put (record, line.line, 4);
		// The column discriminator.
		put (record, 0, 4);
		record.insert (record.end (), name, name + std::strlen (name) + 1);
		last = &line.file;
	}

	size_t padded = (record.size () + 7) & ~(size_t) 7;

	record.resize (padded, 0);
	record[4] = (uint8_t) padded;
	record[5] = (uint8_t) (padded >> 8);
	record[6] = (uint8_t) (padded >> 16);
	record[7] = (uint8_t) (padded >> 24);
	return record;
}

/// The code load, with the line rows and the frame description in front of it
/// under the dump writer's lock.
void
write (const char *name, const uint8_t *code, size_t size, const EhFrame &frame,
       const std::vector<DebugLine> &lines)
{
	std::vector<uint8_t> pre;

	if (!lines.empty ())
		pre = debug_record (code, lines);

	if (!frame.bytes.empty ()) {
		std::vector<uint8_t> unwinding = unwinding_record (frame);

		pre.insert (pre.end (), unwinding.begin (), unwinding.end ());
	}

	mono_emit_jit_dump_code (name, (gpointer) (uintptr_t) code, (guint32) size,
	                         pre.empty () ? nullptr : pre.data (),
	                         (guint32) pre.size ());
}

} // namespace

bool
enabled ()
{
	return mono_jit_dump_is_enabled () != FALSE;
}

size_t
code_slack ()
{
	/*
	 * Room for one function's description, which is 96 bytes plus 32 and the
	 * function's rules. Nothing bounds how many rules a body's CFI program
	 * holds, so this is a generous number rather than a proof.
	 */
	return enabled () ? 512 : 0;
}

namespace {

void
publish_frame (const char *name, const CodeRange &range, EhFrame frame,
               const std::vector<DebugLine> &lines = {})
{
	size_t room = std::max (range.room, range.extent);

	/* The image reaches align8(extent) + the description past the code. A
	 * description that runs out of the room this record owns reaches into the
	 * next record, which then cuts this map back, and a walk out of a frame in a
	 * map perf cut back stops there. So the code is named and left undescribed
	 * instead.
	 *
	 * perf builds the line rows into sections it leaves unallocated, so they
	 * lengthen the image's file and leave the addresses it maps alone. */
	if (((range.extent + 7) & ~(size_t) 7) + frame.bytes.size () > room)
		frame = {};

	write (name, range.code, range.extent, frame, lines);
}

} // namespace

void
publish (const char *name, const CodeRange &range, std::vector<FrameFunction> functions,
         std::vector<DebugLine> lines)
{
	if (!enabled () || range.code == nullptr || range.extent == 0)
		return;

	publish_frame (name, range, build_eh_frame (std::move (functions), range.extent),
	               lines);
}

void
publish (const char *name, const CodeRange &range, const uint8_t *cfi, size_t cfi_size)
{
	if (!enabled () || range.code == nullptr || range.extent == 0)
		return;

	publish_frame (name, range,
	               build_eh_frame (cfi, cfi_size, range.extent, range.extent));
}

} // namespace mono::perf

void
mono_llvm_perf_dump_stub (const char *name, gpointer code, guint32 code_size,
                          const guint8 *cfi, guint32 cfi_size)
{
	mono::perf::publish (name,
	                     {(const uint8_t *) code, code_size,
	                      code_size + mono::perf::code_slack ()},
	                     cfi, cfi_size);
}

guint32
mono_llvm_perf_code_slack (void)
{
	return (guint32) mono::perf::code_slack ();
}

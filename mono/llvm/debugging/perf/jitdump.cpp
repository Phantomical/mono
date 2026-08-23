/**
 * \file
 * \brief Composing the jitdump records that describe one range of JIT'd code.
 *
 * perf applies an unwinding record to the next code load it reads. So the pair
 * has to reach the file back to back, with no other writer in between, and the
 * dump writer takes the two together for exactly that reason.
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

/* Of the record ids perf defines, this is the only one written here. The code
 * load is the dump writer's own. */
constexpr uint32_t jit_code_unwinding_info = 4;

/// id, total size and timestamp, then the three sizes.
constexpr size_t unwinding_record_header_size = 16 + 24;

void
put (std::vector<uint8_t> &out, uint64_t value, size_t width)
{
	for (size_t i = 0; i < width; ++i)
		out.push_back ((uint8_t) (value >> (8 * i)));
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
	put (record, mono_clock_get_time_ns (CLOCK_MONOTONIC), 8);
	put (record, frame.bytes.size (), 8);
	put (record, frame.header_size, 8);
	put (record, frame.bytes.size (), 8);
	record.insert (record.end (), frame.bytes.begin (), frame.bytes.end ());
	record.resize (padded, 0);
	return record;
}

/// The code load, with the frame description in front of it under the dump
/// writer's lock.
void
write (const char *name, const uint8_t *code, size_t size, const EhFrame &frame)
{
	std::vector<uint8_t> pre;

	if (!frame.bytes.empty ())
		pre = unwinding_record (frame);

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
	 * Room for the description of a whole object. A description is 96 bytes plus
	 * 32 and its rules for each function, so what needs the most is a method with
	 * many filter bodies. Over the mini corpora the largest is 192 bytes.
	 *
	 * There is no bound on how many functions an object can hold, so this is a
	 * generous number rather than a proof.
	 */
	return enabled () ? 512 : 0;
}

void
publish (const char *name, const CodeRange &range, std::vector<FrameFunction> functions)
{
	if (!enabled () || range.code == nullptr || range.extent == 0)
		return;

	size_t room = std::max (range.room, range.extent);
	EhFrame frame = build_eh_frame (std::move (functions), range.extent);

	/* The image reaches align8(extent) + the description past the code. A
	 * description that runs out of the room this record owns reaches into the
	 * next record's bytes and loses both, so the code is named and left
	 * undescribed instead. */
	if (((range.extent + 7) & ~(size_t) 7) + frame.bytes.size () > room)
		frame = {};

	write (name, range.code, range.extent, frame);
}

} // namespace mono::perf

void
mono_llvm_perf_dump_stub (const char *name, gpointer code, guint32 code_size,
                          const guint8 *cfi, guint32 cfi_size)
{
	if (!mono::perf::enabled () || code == nullptr || code_size == 0)
		return;

	mono::perf::write (name, (const uint8_t *) code, code_size,
	                   mono::perf::build_eh_frame (cfi, cfi_size, code_size,
	                                               code_size));
}

guint32
mono_llvm_perf_code_slack (void)
{
	return (guint32) mono::perf::code_slack ();
}

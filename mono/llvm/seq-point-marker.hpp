/**
 * \file
 * \brief How a sequence point is marked in the emitted line table.
 *
 * The one channel the translator has for telling the engine something about a
 * particular address is the DWARF line table, whose line number carries an IL
 * offset (il-line-table.hpp). A sequence point needs a second fact recorded
 * against an address - "the soft debugger's trampolines return into the
 * construct that starts here" - so it rides the same channel with the IL offset
 * biased out of the range a real one can reach.
 *
 * mono spells the two synthetic sequence points with an offset that does not
 * fit a line number (method entry is -1), so they get their own encodings just
 * below the bias.
 *
 * A marker also carries the flags the soft debugger's stepper reads off a
 * sequence point, which fit above the offset: the offset part of a marker never
 * reaches 2^26, so everything from that bit up is free. They are a property of
 * the stop rather than of the IL offset, so they ride with the address here
 * instead of being joined on later from a table of their own.
 */

#ifndef MONO_LLVM_SEQ_POINT_MARKER_HPP
#define MONO_LLVM_SEQ_POINT_MARKER_HPP

#include <cstdint>

namespace mono {

/// Line numbers at or above this stand for a sequence point marker rather than
/// the IL offset in effect at an address.
constexpr uint32_t SEQ_POINT_MARKER_BASE = 0x2000000;

/// METHOD_ENTRY_IL_OFFSET and METHOD_EXIT_IL_OFFSET as this encoding spells
/// them.
constexpr uint32_t SEQ_POINT_ENCODED_ENTRY = SEQ_POINT_MARKER_BASE - 2;
constexpr uint32_t SEQ_POINT_ENCODED_EXIT = SEQ_POINT_MARKER_BASE - 1;

/// Where a marker's flags start; below them is SEQ_POINT_MARKER_BASE plus the
/// IL offset the marker stands for.
constexpr uint32_t SEQ_POINT_FLAGS_SHIFT = 26;
constexpr uint32_t SEQ_POINT_OFFSET_MASK = (1u << SEQ_POINT_FLAGS_SHIFT) - 1;

static_assert (2 * SEQ_POINT_MARKER_BASE <= SEQ_POINT_OFFSET_MASK + 1,
               "a marker's offset has to fit below its flags");

/// The line number marking a sequence point at ENCODED_IL that carries FLAGS.
/// The flags are MonoSeqPointFlags and reach the published table untouched;
/// nothing between here and there interprets them.
constexpr uint32_t
seq_point_marker_line (uint32_t encoded_il, uint8_t flags)
{
	return (uint32_t (flags) << SEQ_POINT_FLAGS_SHIFT)
	       | (SEQ_POINT_MARKER_BASE + encoded_il);
}

/// Whether LINE - a line number with IL_OFFSET_LINE_BIAS already taken off -
/// marks a sequence point rather than naming the IL offset in effect.
constexpr bool
seq_point_is_marker (uint32_t line)
{
	return line >= SEQ_POINT_MARKER_BASE;
}

constexpr uint32_t
seq_point_marker_offset (uint32_t line)
{
	return (line & SEQ_POINT_OFFSET_MASK) - SEQ_POINT_MARKER_BASE;
}

constexpr uint8_t
seq_point_marker_flags (uint32_t line)
{
	return (uint8_t) (line >> SEQ_POINT_FLAGS_SHIFT);
}

} // namespace mono

#endif /* MONO_LLVM_SEQ_POINT_MARKER_HPP */

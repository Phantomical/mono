/**
 * \file
 * \brief How a sequence point is marked in the emitted line table.
 *
 * The translator's only channel for saying something about a particular address
 * is the DWARF line table, whose line number carries an IL offset
 * (il-line-table.hpp). A sequence point needs a second fact recorded against an
 * address: the soft debugger's trampolines return into the construct that
 * starts here. So it rides the same channel, with the IL offset raised out of
 * the range a real one reaches.
 *
 * mono spells its two synthetic sequence points with sentinel offsets rather
 * than real ones. METHOD_ENTRY_IL_OFFSET is -1 and METHOD_EXIT_IL_OFFSET is
 * 0xffffff, so each gets an encoding of its own just below
 * SEQ_POINT_MARKER_BASE.
 *
 * A marker also carries the flags the soft debugger's stepper reads off a
 * sequence point. They fit above the offset, because a marker's offset never
 * reaches 2^26 and every bit from there up is free. The flags belong to the
 * stop rather than to the IL offset. So they ride with the address here rather
 * than being joined on later from a table of their own.
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

/// Where a marker's flags start. Below them is SEQ_POINT_MARKER_BASE plus the
/// IL offset the marker stands for.
constexpr uint32_t SEQ_POINT_FLAGS_SHIFT = 26;
constexpr uint32_t SEQ_POINT_OFFSET_MASK = (1u << SEQ_POINT_FLAGS_SHIFT) - 1;

static_assert (2 * SEQ_POINT_MARKER_BASE <= SEQ_POINT_OFFSET_MASK + 1,
               "a marker's offset has to fit below its flags");

/// The line number marking a sequence point at an encoded IL offset, carrying
/// the given flags.
///
/// The flags are the MONO_SEQ_POINT_FLAG_* bits. They reach the published table
/// untouched, because nothing between here and there interprets them.
constexpr uint32_t
seq_point_marker_line (uint32_t encoded_il, uint8_t flags)
{
	return (uint32_t (flags) << SEQ_POINT_FLAGS_SHIFT)
	       | (SEQ_POINT_MARKER_BASE + encoded_il);
}

/// Whether a line number, with IL_OFFSET_LINE_BIAS already taken off, marks a
/// sequence point rather than naming the IL offset in effect.
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

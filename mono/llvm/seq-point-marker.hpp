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

} // namespace mono

#endif /* MONO_LLVM_SEQ_POINT_MARKER_HPP */

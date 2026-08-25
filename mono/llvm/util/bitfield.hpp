#ifndef MONO_LLVM_UTIL_BITFIELD_HPP
#define MONO_LLVM_UTIL_BITFIELD_HPP

/**
 * \file
 * \brief Where a bitfield sits, for generated code that has to load one.
 */

#include <glib.h>

#include <cstdint>
#include <cstring>

namespace mono {

/// Where one bitfield sits inside the structure that declares it.
struct BitfieldPlace {
	/// Of the 32-bit word that holds the field, from the start of the structure.
	uint32_t offset;
	/// The bits the field owns inside that word.
	uint32_t mask;
	/// How far right the field sits inside that word.
	uint32_t shift;
};

/*
 * A bitfield has no offsetof, so a compiler that emits a load of one needs the
 * word it sits in and the mask it owns inside that word. The C++ compiler in
 * front of this header already made those choices, and locate_bitfield () reads
 * them back: it sets the field to all ones in a zeroed object and copies the
 * object representation out. The runtime and the backend compile the same
 * struct declarations with the same compiler, so what comes back describes the
 * objects generated code loads.
 *
 * A constant expression cannot do this work. Two routes exist and this
 * toolchain refuses both: a constexpr read of an inactive union member is
 * ill-formed, and clang answers __builtin_bit_cast over a bitfield with
 * "constexpr bit_cast involving bit-field is not yet supported". So the probe
 * runs once, at the first read.
 */

/// Reads back where one bitfield of T sits.
///
/// write sets that one field, and all_ones is the value it sets: the largest
/// the field's declared width holds. Together they are what
/// MONO_BITFIELD_PLACE spells, and callers want that macro rather than this.
///
/// The caller owns the two claims this cannot make for itself. The field must
/// be the only one write touches, since a second one widens the mask. And
/// all_ones must match the declared width, which is what lets a caller check
/// the answer against the neighbours: two fields whose masks overlap say the
/// widths disagree with the header.
template <typename T, typename Write>
BitfieldPlace
locate_bitfield (Write write, uint32_t all_ones)
{
	unsigned char bytes[sizeof (T)];
	T probe;

	memset (&probe, 0, sizeof probe);
	write (probe);
	memcpy (bytes, &probe, sizeof bytes);

	size_t first = 0;

	while (first < sizeof (T) && bytes[first] == 0)
		first++;

	// The word is the aligned one that holds the first byte the write reached.
	// A field wider than the bytes left in it does not fit a 32-bit load, and
	// the mask check below is what catches that.
	size_t offset = first & ~static_cast<size_t> (3);
	uint32_t mask = 0;

	if (offset + sizeof mask <= sizeof (T))
		memcpy (&mask, bytes + offset, sizeof mask);

	uint32_t shift = 0;

	while (shift < 32 && ((mask >> shift) & 1) == 0)
		shift++;

	BitfieldPlace place = { static_cast<uint32_t> (offset), mask, shift };

	// A field the load cannot reach reads as zero, which is a wrong answer
	// rather than a slow one. Fail here instead.
	g_assert (mask != 0 && (mask >> shift) == all_ones);
	return place;
}

/// Where field of type sits, read back once and held.
///
/// cast is the type write assigns through, which is the field's own declared
/// type. all_ones is the largest value the field's width holds.
#define MONO_BITFIELD_PLACE(type, field, cast, all_ones)                      \
	([] () -> const ::mono::BitfieldPlace & {                             \
		static const ::mono::BitfieldPlace place =                    \
			::mono::locate_bitfield<type> (                       \
				[] (type &probe) {                            \
					probe.field =                         \
						static_cast<cast> (all_ones); \
				},                                            \
				(all_ones));                                  \
		return place;                                                 \
	} ())

} // namespace mono

#endif

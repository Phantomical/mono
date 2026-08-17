#ifndef __MONO_INTERP_INTERP_INTRINS_HPP__
#define __MONO_INTERP_INTERP_INTRINS_HPP__

/**
 * \file
 * \brief Bodies for library methods the interpreter answers with one opcode.
 *
 * Each stands in for a BCL method that shows up in interpreter-heavy work and
 * would otherwise run a bytecode at a time.
 */

#include <mono/metadata/gc-internals.h>
#include <mono/metadata/object-internals.h>

#include "internals.hpp"

namespace mono::interp {

inline guint32
rotate_left (guint32 value, int offset)
{
	return (value << offset) | (value >> (32 - offset));
}

inline void
marvin_block (guint32 *pp0, guint32 *pp1)
{
	// Marvin.Block
	guint32 p0 = *pp0;
	guint32 p1 = *pp1;

	p1 ^= p0;
	p0 = rotate_left (p0, 20);

	p0 += p1;
	p1 = rotate_left (p1, 9);

	p1 ^= p0;
	p0 = rotate_left (p0, 27);

	p0 += p1;
	p1 = rotate_left (p1, 19);

	*pp0 = p0;
	*pp1 = p1;
}

inline guint32
ascii_chars_to_uppercase (guint32 value)
{
	// Utf16Utility.ConvertAllAsciiCharsInUInt32ToUppercase
	guint32 lowerIndicator = value + 0x00800080 - 0x00610061;
	guint32 upperIndicator = value + 0x00800080 - 0x007B007B;
	guint32 combinedIndicator = (lowerIndicator ^ upperIndicator);
	guint32 mask = (combinedIndicator & 0x00800080) >> 2;

	return value ^ mask;
}

inline int
ordinal_ignore_case_ascii (guint32 valueA, guint32 valueB)
{
	// Utf16Utility.UInt32OrdinalIgnoreCaseAscii
	guint32 differentBits = (valueA ^ valueB) << 2;
	guint32 indicator = valueA + 0x00050005;
	indicator |= 0x00A000A0;
	indicator += 0x001A001A;
	indicator |= 0xFF7FFF7F;
	return (differentBits & indicator) == 0;
}

inline int
ordinal_ignore_case_ascii (guint64 valueA, guint64 valueB)
{
	// Utf16Utility.UInt64OrdinalIgnoreCaseAscii
	guint64 differentBits = (valueA ^ valueB) << 2;
	guint64 indicator = valueA + 0x0005000500050005ull;
	indicator |= 0x00A000A000A000A0ull;
	indicator += 0x001A001A001A001Aull;
	indicator |= 0xFF7FFF7FFF7FFF7Full;
	return (differentBits & indicator) == 0;
}

inline int
count_digits (guint32 value)
{
	int digits = 1;
	if (value >= 100000) {
		value /= 100000;
		digits += 5;
	}
	if (value < 10) {
		// no-op
	} else if (value < 100) {
		digits++;
	} else if (value < 1000) {
		digits += 2;
	} else if (value < 10000) {
		digits += 3;
	} else {
		digits += 4;
	}
	return digits;
}

inline guint32
math_divrem (guint32 a, guint32 b, guint32 *result)
{
	guint32 div = a / b;
	*result = a - (div * b);
	return div;
}

inline MonoString *
u32_to_decstr (guint32 value, MonoArray *cache, MonoVTable *vtable)
{
	// Number.UInt32ToDecStr
	int bufferLength = count_digits (value);

	if (bufferLength == 1)
		return mono_array_get_fast (cache, MonoString*, value);

	int size = (G_STRUCT_OFFSET (MonoString, chars) + (((size_t)bufferLength + 1) * 2));
	MonoString* result = mono_gc_alloc_string (vtable, size, bufferLength);
	mono_unichar2 *buffer = &result->chars [0];
	mono_unichar2 *p = buffer + bufferLength;
	do {
		guint32 remainder;
		value = math_divrem (value, 10, &remainder);
		*(--p) = (mono_unichar2)(remainder + '0');
	} while (value != 0);
	return result;
}

inline mono_u
widen_ascii_to_utf16 (guint8 *pAsciiBuffer, mono_unichar2 *pUtf16Buffer, mono_u elementCount)
{
	// ASCIIUtility.WidenAsciiToUtf16
	mono_u currentOffset = 0;

	while (currentOffset < elementCount) {
		guint16 asciiData = pAsciiBuffer [currentOffset];
		if ((asciiData & 0x80) != 0)
			return currentOffset;

		pUtf16Buffer [currentOffset] = (mono_unichar2)asciiData;
		currentOffset++;
	}
	return currentOffset;
}

} // namespace mono::interp

#endif

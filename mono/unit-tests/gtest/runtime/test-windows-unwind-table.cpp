/*
 * test-windows-unwind-table.cpp: Tests for dynamic Windows unwind tables.
 */

#include "config.h"

#include "mini/mini-windows.h"

#include <gtest/gtest.h>

#include "harness.hpp"

#ifdef MONO_ARCH_HAVE_UNWIND_TABLE

extern "C" {
PRUNTIME_FUNCTION
mono_arch_unwindinfo_find_rt_func_in_table (const gpointer code, gsize code_size);
}

namespace {

// Version 1, no flags, no prologue, no unwind codes, no frame register: the
// smallest well-formed UNWIND_INFO.
const uint8_t trivial_unwind_info[4] = { 0x01, 0x00, 0x00, 0x00 };

} // namespace

TEST (WindowsUnwindTable, GrowPreservesRecordWithZeroUnwindData)
{
	const size_t chunk_size = 4096;
	const size_t range_size = 256;

	uint8_t *chunk = (uint8_t *) VirtualAlloc (
		NULL, chunk_size, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
	ASSERT_NE (nullptr, chunk);

	memcpy (chunk, trivial_unwind_info, sizeof (trivial_unwind_info));

	DynamicFunctionTableEntry *range =
		mono_arch_unwindinfo_insert_range_in_table (chunk, range_size);
	ASSERT_NE (nullptr, range);

	// UnwindData is an offset from the chunk, so zero is a valid value.
	uint8_t *zero_offset_code = chunk + 16;
	ASSERT_NE (nullptr, mono_arch_unwindinfo_insert_rt_func_in_table (
		zero_offset_code, 8, chunk));

	// A 256-byte range starts with room for three records.
	uint8_t *code1 = chunk + 32;
	memcpy (code1 + 8, trivial_unwind_info, sizeof (trivial_unwind_info));
	ASSERT_NE (nullptr, mono_arch_unwindinfo_insert_rt_func_in_table (
		code1, 8, code1 + 8));

	uint8_t *code2 = chunk + 56;
	memcpy (code2 + 8, trivial_unwind_info, sizeof (trivial_unwind_info));
	ASSERT_NE (nullptr, mono_arch_unwindinfo_insert_rt_func_in_table (
		code2, 8, code2 + 8));

	// The fourth record grows the table.
	uint8_t *code3 = chunk + 80;
	memcpy (code3 + 8, trivial_unwind_info, sizeof (trivial_unwind_info));
	ASSERT_NE (nullptr, mono_arch_unwindinfo_insert_rt_func_in_table (
		code3, 8, code3 + 8));

	EXPECT_NE (nullptr, mono_arch_unwindinfo_find_rt_func_in_table (zero_offset_code, 8))
		<< "table growth dropped the record with zero UnwindData";
	EXPECT_NE (nullptr, mono_arch_unwindinfo_find_rt_func_in_table (code1, 8));
	EXPECT_NE (nullptr, mono_arch_unwindinfo_find_rt_func_in_table (code2, 8));
	EXPECT_NE (nullptr, mono_arch_unwindinfo_find_rt_func_in_table (code3, 8));

	mono_arch_unwindinfo_remove_pc_range_in_table (chunk);
	VirtualFree (chunk, 0, MEM_RELEASE);
}

#endif /* MONO_ARCH_HAVE_UNWIND_TABLE */

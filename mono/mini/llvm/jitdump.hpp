/**
 * \file
 * jitdump.hpp - C++-only interface for the perf jitdump writer.
 *
 * This header is consumed ONLY by jitdump.cpp and by mono/unit-tests/
 * test-llvm-jitdump.cpp. The runtime entry point is
 * mono_llvm_jitdump_emit_method () over in backend.h; what is exposed here is
 * the one piece worth driving on its own - the rewrite of LLVM's .eh_frame
 * into the tables perf expects - because every offset in its output is
 * measured against a layout that only exists inside `perf inject --jit`, so
 * nothing about the running process can show whether it came out right.
 */

#ifndef __MONO_MINI_LLVM_JITDUMP_HPP__
#define __MONO_MINI_LLVM_JITDUMP_HPP__

#include <cstdint>
#include <string>
#include <vector>

namespace mono {

/* The .eh_frame_hdr build_perf_unwind_data () appends: version, three encoding
 * bytes, eh_frame_ptr, fde_count, and one search-table entry. */
constexpr std::uint32_t PERF_EH_FRAME_HDR_SIZE = 4 + 4 + 4 + 4 + 4;

/* .text sits at this offset in the ELF perf injects, and .eh_frame follows it
 * aligned to 8. */
constexpr std::uint32_t PERF_ELF_TEXT_OFFSET = 128;
constexpr std::uint32_t PERF_ELF_EH_FRAME_ALIGN = 8;

/*
 * Rebuild the unwind tables for [CODE, CODE + CODE_SIZE) out of the .eh_frame
 * section at EH_FRAME: a one-CIE, one-FDE .eh_frame followed by an
 * .eh_frame_hdr, with every offset written for the layout perf's injected DSO
 * will give them (see the header comment on jitdump.cpp).
 *
 * FALSE means no tables we are sure of could be produced - the section is
 * malformed, holds no FDE for CODE, or uses a pointer encoding we do not know
 * how to relocate. The caller then reports the method without unwind info
 * rather than with unwind info that points somewhere arbitrary.
 */
bool build_perf_unwind_data (const std::uint8_t *code, std::uint32_t code_size,
                             const std::uint8_t *eh_frame, std::uint32_t eh_frame_size,
                             std::vector<std::uint8_t> &out);

/*
 * One line-table row as perf wants it: an absolute address in the code being
 * reported, and where that address came from. `name` is whatever a reader should
 * see in the file column.
 */
struct PerfDebugEntry {
	std::uint64_t addr;
	std::int32_t lineno;
	std::int32_t discrim;
	std::string name;
};

/*
 * Serialize ENTRIES into a complete JIT_CODE_DEBUG_INFO record for the code at
 * CODE_ADDR. ENTRIES must be ascending by address - perf walks them in order and
 * treats each as running until the next.
 *
 * FALSE if there is nothing worth writing (no entries), leaving OUT untouched.
 *
 * Exposed for the same reason as build_perf_unwind_data (): the record is only
 * ever read inside `perf inject --jit`, so nothing about the running process
 * shows whether the bytes came out right.
 */
bool build_perf_debug_info (std::uint64_t code_addr,
                            const std::vector<PerfDebugEntry> &entries,
                            std::vector<std::uint8_t> &out);

} // namespace mono

#endif /* __MONO_MINI_LLVM_JITDUMP_HPP__ */

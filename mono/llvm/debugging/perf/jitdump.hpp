/**
 * \file
 * \brief What `--jitdump` says about JIT'd code: its name, and how to unwind
 *        out of it.
 */

#ifndef MONO_LLVM_DEBUGGING_PERF_JITDUMP_HPP
#define MONO_LLVM_DEBUGGING_PERF_JITDUMP_HPP

#include "debugging/perf/eh-frame.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace mono::perf {

/// Whether a dump is open at all. `--jitdump` decides that.
bool enabled ();

/// How many bytes past its end a code allocation has to keep free while a dump
/// is open. Zero when no dump is open.
///
/// perf does not read a frame description out of the record. It builds an ELF
/// image per record and maps it over the code, and the image is longer than the
/// code by the description. A second record inside that range takes the range
/// from the first, which then reads its tables out of the other one's image and
/// finds anything at all. So code the dump describes has to be spaced out.
///
/// A multiple of 16, because code that is aligned before the slack has to stay
/// aligned after it.
size_t code_slack ();

/// One range of JIT'd code as a dump record sees it.
struct CodeRange {
	const uint8_t *code = nullptr;
	/// What the record names. perf reads it as the extent of the name, so a
	/// range that covers a neighbour takes that neighbour's samples.
	size_t extent = 0;
	/// How far past the code the record can reach. A description that does not
	/// fit is left out.
	size_t room = 0;
};

/// Name a range of code in the dump, so a profile prints it instead of an
/// address, and describe the frame of each function in it.
///
/// Pass no function - or too little room for the description - and the code is
/// named but a stack walk stops at it.
void publish (const char *name, const CodeRange &range,
              std::vector<FrameFunction> functions = {});

} // namespace mono::perf

#endif /* MONO_LLVM_DEBUGGING_PERF_JITDUMP_HPP */

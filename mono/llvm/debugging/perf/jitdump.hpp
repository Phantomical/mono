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
#include <string>
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
	/// What the record names. Where two records claim one address, perf keeps
	/// the one written later, so an overlap decides a sample's name by the
	/// order the records went in.
	size_t extent = 0;
	/// How far past the code the record can reach. A description that does not
	/// fit is left out.
	size_t room = 0;
};

/// One address in a record's code, and where in a method's IL it is.
///
/// The field names are DWARF's, because a profile reads these back as a source
/// position. A method's name stands in for the file and an IL offset for the
/// line.
struct DebugLine {
	/// From the start of the record's code.
	size_t offset = 0;
	/// The IL offset in effect.
	uint32_t line = 0;
	/// The method executing at the offset. An inliner's work is what makes
	/// this something other than the record's own method.
	std::string file;
};

/// Name a range of code in the dump, so a profile prints it instead of an
/// address, and describe the frame of each function in it.
///
/// Pass no function - or too little room for the description - and the code is
/// named but a stack walk stops at it.
///
/// lines must ascend by offset, with one row at most per offset. perf turns
/// them into a DWARF line program, which can say no more than that.
void publish (const char *name, const CodeRange &range,
              std::vector<FrameFunction> functions = {},
              std::vector<DebugLine> lines = {});

/// The same, for code whose frame description is already the DWARF CFI program
/// mono keeps for a classic body or a stub (mono_unwind_ops_encode).
void publish (const char *name, const CodeRange &range, const uint8_t *cfi,
              size_t cfi_size);

} // namespace mono::perf

#endif /* MONO_LLVM_DEBUGGING_PERF_JITDUMP_HPP */

/**
 * \file
 * \brief The DWARF frame description perf wants for one JIT'd function.
 */

#ifndef MONO_LLVM_DEBUGGING_PERF_EH_FRAME_HPP
#define MONO_LLVM_DEBUGGING_PERF_EH_FRAME_HPP

#include "sidetables.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace mono::perf {

/// A `.eh_frame` and the `.eh_frame_hdr` that indexes it, in the order a jitdump
/// unwinding record carries the two.
struct EhFrame {
	std::vector<uint8_t> bytes;
	/// How many of the trailing bytes are the `.eh_frame_hdr`.
	size_t header_size = 0;
};

/// One function a record's frame description covers.
struct FrameFunction {
	size_t offset = 0;
	size_t size = 0;
	/// The CFI program `.mono_unwind` recorded for it. An empty one says the
	/// function still has the frame it was called with, which is what the
	/// linker's stubs and the runtime's thunks have.
	std::vector<UnwindRecord> records;
};

/// Describe every function in one record's code.
///
/// image_size is the length the code load declares. perf puts `.eh_frame` after
/// that many bytes of code, and one record covers a whole object, so it is more
/// than any one function.
///
/// A function whose program holds a rule this writer cannot say in DWARF is left
/// out. A description short of one rule unwinds to a wrong answer instead of
/// stopping, so a partial one is worth less than none.
///
/// Every address in the result is a displacement rather than an address. The
/// displacements come out right once `perf inject --jit` lays the pieces into an
/// ELF image, and that layout is what makes the result specific to perf.
EhFrame build_eh_frame (std::vector<FrameFunction> functions, size_t image_size);

/// Describe a function whose frame description is already a DWARF CFI program -
/// the form mono keeps for the stubs and trampolines it plants itself.
///
/// The program is taken to start from this target's initial frame state, which
/// mono leaves implicit. Its operands are taken to be scaled by the same
/// alignment factors the CIE written here declares.
EhFrame build_eh_frame (const uint8_t *cfi, size_t cfi_size, size_t code_size,
                        size_t image_size);

} // namespace mono::perf

#endif /* MONO_LLVM_DEBUGGING_PERF_EH_FRAME_HPP */
